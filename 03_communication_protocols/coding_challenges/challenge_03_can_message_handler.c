/*
 * Challenge 03: CAN Message Handler with Filtering
 *
 * Task: implement a CAN receive pipeline with ID-based software filtering,
 * message dispatching, and a lightweight statistics / diagnostics layer.
 *
 * The design pattern here is common in production automotive ECUs:
 *   - A hardware receive ISR places raw frames into a ring buffer.
 *   - A processing task dequeues frames, applies software filters, and
 *     dispatches to registered handlers (similar to CANopen, SAE J1939 Rx).
 *   - A diagnostics module tracks per-ID statistics for fault detection.
 *
 * Modules implemented:
 *   1. can_frame_t         - CAN frame type (standard + extended ID)
 *   2. can_filter_t        - software filter (mask mode or list mode)
 *   3. can_rx_ring_t       - lock-free SPSC ring buffer for ISR -> task handoff
 *   4. can_dispatcher_t    - filter + callback dispatch table
 *   5. can_stats_t         - per-ID frame counter and timestamp tracker
 *
 * Compile and run tests:
 *   gcc -std=c11 -Wall -Wextra -o challenge_03 challenge_03_can_message_handler.c && ./challenge_03
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <stddef.h>

/* =========================================================================
 * CAN frame type
 * =========================================================================*/

#define CAN_MAX_DLC  8u

typedef struct {
    uint32_t id;          /* 11-bit (standard) or 29-bit (extended) identifier */
    bool     ide;         /* false = standard (11-bit), true = extended (29-bit) */
    bool     rtr;         /* Remote Transmission Request */
    uint8_t  dlc;         /* Data Length Code: 0-8 */
    uint8_t  data[CAN_MAX_DLC];
    uint32_t timestamp_ms; /* reception timestamp (from SysTick or RTOS tick) */
} CanFrame;

/* =========================================================================
 * Software acceptance filter
 *
 * Supports two modes:
 *   MASK:  accept if (received_id & mask) == (filter_id & mask)
 *   LIST:  accept if received_id == filter_id (exact match)
 *
 * Note: hardware filters (e.g., STM32 FDCAN acceptance filters) implement
 * the same logic, but software filters allow more flexible configuration
 * and can span multiple hardware filter banks.
 * =========================================================================*/

typedef enum {
    CAN_FILTER_MASK = 0,   /* mask-and-compare mode */
    CAN_FILTER_LIST = 1,   /* exact match (list) mode */
} CanFilterMode;

typedef struct {
    uint32_t       id;     /* filter ID */
    uint32_t       mask;   /* mask (MASK mode: don't-care bits are 0)
                              list mode: ignored (implicit mask = 0x1FFFFFFF) */
    CanFilterMode  mode;
    bool           enabled;
    bool           extended; /* filter applies to extended (29-bit) frames */
} CanFilter;

/*
 * can_filter_matches
 *
 * Returns true if 'frame' passes the filter.
 */
static bool can_filter_matches(const CanFilter *filter, const CanFrame *frame)
{
    if (!filter->enabled) return false;
    if (filter->extended != frame->ide) return false;  /* wrong ID type */

    if (filter->mode == CAN_FILTER_MASK) {
        return (frame->id & filter->mask) == (filter->id & filter->mask);
    } else {
        /* LIST mode: exact match */
        return frame->id == filter->id;
    }
}

/* =========================================================================
 * Lock-free SPSC ring buffer for CAN Rx frames
 *
 * Used for ISR -> processing task handoff.
 * Power-of-2 capacity allows bitmask indexing.
 *
 * Only one producer (the ISR) and one consumer (the processing task) —
 * no mutex needed. Head and tail would use atomic_uint in a real RTOS;
 * here we use volatile for the PC test (single-threaded, no reordering).
 * =========================================================================*/

#define CAN_RX_RING_SIZE 16u   /* must be power of 2 */
#define CAN_RX_RING_MASK (CAN_RX_RING_SIZE - 1u)

typedef struct {
    CanFrame         buf[CAN_RX_RING_SIZE];
    volatile uint32_t head;   /* consumer index (read by consumer, write by consumer) */
    volatile uint32_t tail;   /* producer index (read by producer, write by producer) */
    uint32_t          overflow_count;   /* frames dropped due to full ring */
} CanRxRing;

static void can_rx_ring_init(CanRxRing *ring)
{
    memset(ring, 0, sizeof(CanRxRing));
}

/*
 * can_rx_ring_push — called from ISR context.
 * Returns true if the frame was enqueued, false if the ring is full.
 */
static bool can_rx_ring_push(CanRxRing *ring, const CanFrame *frame)
{
    uint32_t tail = ring->tail;
    uint32_t next = (tail + 1u) & CAN_RX_RING_MASK;

    if (next == (ring->head & CAN_RX_RING_MASK)) {
        /* Full */
        ring->overflow_count++;
        return false;
    }

    ring->buf[tail & CAN_RX_RING_MASK] = *frame;
    ring->tail = (tail + 1u) & CAN_RX_RING_MASK;
    return true;
}

/*
 * can_rx_ring_pop — called from task context.
 * Returns true and copies the oldest frame to 'out' if the ring is non-empty.
 */
static bool can_rx_ring_pop(CanRxRing *ring, CanFrame *out)
{
    uint32_t head = ring->head & CAN_RX_RING_MASK;
    uint32_t tail = ring->tail & CAN_RX_RING_MASK;

    if (head == tail) return false;   /* empty */

    *out = ring->buf[head];
    ring->head = (ring->head + 1u) & CAN_RX_RING_MASK;
    return true;
}

static uint32_t can_rx_ring_count(const CanRxRing *ring)
{
    uint32_t head = ring->head & CAN_RX_RING_MASK;
    uint32_t tail = ring->tail & CAN_RX_RING_MASK;
    return (tail >= head) ? (tail - head) : (CAN_RX_RING_SIZE - head + tail);
}

/* =========================================================================
 * CAN message handler / callback type
 * =========================================================================*/

typedef void (*CanMsgHandler)(const CanFrame *frame, void *user_ctx);

/* =========================================================================
 * CAN dispatcher
 *
 * Pairs a filter with a callback. When a frame passes the filter, the
 * corresponding callback is invoked.
 *
 * Up to CAN_DISPATCHER_MAX_ENTRIES entries.
 * =========================================================================*/

#define CAN_DISPATCHER_MAX_ENTRIES 8u

typedef struct {
    CanFilter      filter;
    CanMsgHandler  handler;
    void          *user_ctx;
    uint32_t       match_count;   /* how many frames matched this entry */
} CanDispatchEntry;

typedef struct {
    CanDispatchEntry entries[CAN_DISPATCHER_MAX_ENTRIES];
    uint32_t         count;
    uint32_t         unmatched_count;   /* frames that matched no filter */
} CanDispatcher;

static void can_dispatcher_init(CanDispatcher *disp)
{
    memset(disp, 0, sizeof(CanDispatcher));
}

/*
 * can_dispatcher_register
 *
 * Registers a filter + callback pair. Returns the entry index on success,
 * -1 if the table is full.
 */
static int can_dispatcher_register(CanDispatcher *disp,
                                   const CanFilter *filter,
                                   CanMsgHandler handler,
                                   void *user_ctx)
{
    if (disp->count >= CAN_DISPATCHER_MAX_ENTRIES) return -1;

    uint32_t idx = disp->count;
    disp->entries[idx].filter     = *filter;
    disp->entries[idx].handler    = handler;
    disp->entries[idx].user_ctx   = user_ctx;
    disp->entries[idx].match_count = 0;
    disp->count++;
    return (int)idx;
}

/*
 * can_dispatcher_dispatch
 *
 * Checks the frame against all registered filters. Calls the first matching
 * handler (or all matching handlers if multiple match).
 *
 * Policy here: call ALL matching handlers (useful for logging + processing).
 * Change to 'break after first match' for exclusive routing.
 */
static void can_dispatcher_dispatch(CanDispatcher *disp, const CanFrame *frame)
{
    bool matched = false;

    for (uint32_t i = 0; i < disp->count; i++) {
        CanDispatchEntry *entry = &disp->entries[i];
        if (can_filter_matches(&entry->filter, frame)) {
            matched = true;
            entry->match_count++;
            if (entry->handler) {
                entry->handler(frame, entry->user_ctx);
            }
        }
    }

    if (!matched) {
        disp->unmatched_count++;
    }
}

/* =========================================================================
 * CAN per-ID statistics
 *
 * Tracks frame count, last-received timestamp, and min/max inter-frame gap
 * for each unique CAN ID seen on the bus.
 *
 * Useful for detecting:
 *   - Message timeout (ID not received within expected period)
 *   - Burst overrun (inter-frame gap too small)
 *   - Ghost messages (unexpected IDs)
 * =========================================================================*/

#define CAN_STATS_MAX_IDS 16u

typedef struct {
    uint32_t id;
    uint32_t frame_count;
    uint32_t last_rx_ms;
    uint32_t min_gap_ms;    /* minimum observed inter-frame gap */
    uint32_t max_gap_ms;    /* maximum observed inter-frame gap */
    bool     active;
} CanIdStats;

typedef struct {
    CanIdStats entries[CAN_STATS_MAX_IDS];
    uint32_t   count;
    uint32_t   total_frames;
    uint32_t   overflow_count;   /* new IDs seen when table is full */
} CanStats;

static void can_stats_init(CanStats *stats)
{
    memset(stats, 0, sizeof(CanStats));
}

static void can_stats_update(CanStats *stats, const CanFrame *frame)
{
    stats->total_frames++;

    /* Find existing entry for this ID */
    for (uint32_t i = 0; i < stats->count; i++) {
        if (stats->entries[i].id == frame->id) {
            CanIdStats *s = &stats->entries[i];
            uint32_t gap = frame->timestamp_ms - s->last_rx_ms;
            if (gap < s->min_gap_ms || s->frame_count == 1) s->min_gap_ms = gap;
            if (gap > s->max_gap_ms)                         s->max_gap_ms = gap;
            s->last_rx_ms = frame->timestamp_ms;
            s->frame_count++;
            return;
        }
    }

    /* New ID */
    if (stats->count >= CAN_STATS_MAX_IDS) {
        stats->overflow_count++;
        return;
    }
    CanIdStats *s     = &stats->entries[stats->count++];
    s->id             = frame->id;
    s->frame_count    = 1;
    s->last_rx_ms     = frame->timestamp_ms;
    s->min_gap_ms     = UINT32_MAX;
    s->max_gap_ms     = 0;
    s->active         = true;
}

/*
 * can_stats_check_timeout
 *
 * Returns true if the given ID has not been received within 'timeout_ms'
 * milliseconds of 'now_ms'. Returns false if the ID has never been seen.
 */
static bool can_stats_check_timeout(const CanStats *stats,
                                    uint32_t id,
                                    uint32_t now_ms,
                                    uint32_t timeout_ms)
{
    for (uint32_t i = 0; i < stats->count; i++) {
        if (stats->entries[i].id == id) {
            return (now_ms - stats->entries[i].last_rx_ms) > timeout_ms;
        }
    }
    return false;   /* ID never seen — caller must decide if that is a fault */
}

static void can_stats_print(const CanStats *stats)
{
    printf("  CAN statistics (%u unique IDs, %u total frames):\n",
           stats->count, stats->total_frames);
    for (uint32_t i = 0; i < stats->count; i++) {
        const CanIdStats *s = &stats->entries[i];
        printf("    ID 0x%03X: %u frames, last=%u ms, gap min=%u max=%u ms\n",
               s->id, s->frame_count, s->last_rx_ms,
               s->min_gap_ms == UINT32_MAX ? 0 : s->min_gap_ms,
               s->max_gap_ms);
    }
}

/* =========================================================================
 * Full pipeline: ring buffer + dispatcher + stats
 * =========================================================================*/

typedef struct {
    CanRxRing    ring;
    CanDispatcher dispatcher;
    CanStats     stats;
} CanPipeline;

static void can_pipeline_init(CanPipeline *pipe)
{
    can_rx_ring_init(&pipe->ring);
    can_dispatcher_init(&pipe->dispatcher);
    can_stats_init(&pipe->stats);
}

/* Called from ISR: enqueue a received frame */
static bool can_pipeline_isr_enqueue(CanPipeline *pipe, const CanFrame *frame)
{
    return can_rx_ring_push(&pipe->ring, frame);
}

/* Called from task: drain the ring and process all pending frames */
static uint32_t can_pipeline_process(CanPipeline *pipe)
{
    uint32_t processed = 0;
    CanFrame frame;
    while (can_rx_ring_pop(&pipe->ring, &frame)) {
        can_stats_update(&pipe->stats, &frame);
        can_dispatcher_dispatch(&pipe->dispatcher, &frame);
        processed++;
    }
    return processed;
}

/* =========================================================================
 * Test harness
 * =========================================================================*/

static int tests_run    = 0;
static int tests_passed = 0;

static void check(const char *label, int cond)
{
    tests_run++;
    if (cond) {
        tests_passed++;
        printf("  PASS  %s\n", label);
    } else {
        printf("  FAIL  %s\n", label);
    }
}

/* --- Test 1: ring buffer basic push/pop --- */
static void test_ring_buffer(void)
{
    printf("=== Ring buffer ===\n");
    CanRxRing ring;
    can_rx_ring_init(&ring);

    CanFrame f = { .id = 0x100, .dlc = 2, .data = {0xAA, 0xBB},
                   .timestamp_ms = 1000 };
    check("push to empty ring succeeds", can_rx_ring_push(&ring, &f));
    check("ring count = 1", can_rx_ring_count(&ring) == 1);

    CanFrame out;
    memset(&out, 0, sizeof(out));
    check("pop succeeds", can_rx_ring_pop(&ring, &out));
    check("popped ID matches", out.id == 0x100);
    check("popped data matches", out.data[0] == 0xAA && out.data[1] == 0xBB);
    check("ring empty after pop", can_rx_ring_count(&ring) == 0);

    /* Fill ring to capacity */
    CanFrame dummy = { .id = 0x001 };
    uint32_t pushed = 0;
    for (uint32_t i = 0; i < CAN_RX_RING_SIZE + 5; i++) {
        dummy.id = i;
        if (can_rx_ring_push(&ring, &dummy)) pushed++;
    }
    check("ring fills to capacity (CAN_RX_RING_SIZE - 1)",
          pushed == CAN_RX_RING_SIZE - 1);
    check("overflow count > 0 when ring full", ring.overflow_count > 0);
}

/* --- Test 2: software filters --- */
static void test_filters(void)
{
    printf("\n=== Software filters ===\n");

    CanFilter mask_filter = {
        .id       = 0x200,
        .mask     = 0x7F0,   /* match IDs 0x200-0x20F */
        .mode     = CAN_FILTER_MASK,
        .enabled  = true,
        .extended = false
    };

    CanFrame f1 = { .id = 0x200, .ide = false };
    CanFrame f2 = { .id = 0x205, .ide = false };
    CanFrame f3 = { .id = 0x210, .ide = false };   /* outside range */
    CanFrame f4 = { .id = 0x200, .ide = true  };   /* extended — filter is standard */

    check("mask filter: 0x200 matches 0x200/0x7F0", can_filter_matches(&mask_filter, &f1));
    check("mask filter: 0x205 matches 0x200/0x7F0", can_filter_matches(&mask_filter, &f2));
    check("mask filter: 0x210 does NOT match",      !can_filter_matches(&mask_filter, &f3));
    check("mask filter: extended frame rejected",   !can_filter_matches(&mask_filter, &f4));

    CanFilter list_filter = {
        .id       = 0x123,
        .mode     = CAN_FILTER_LIST,
        .enabled  = true,
        .extended = false
    };
    CanFrame f5 = { .id = 0x123, .ide = false };
    CanFrame f6 = { .id = 0x124, .ide = false };
    check("list filter: exact match passes",       can_filter_matches(&list_filter, &f5));
    check("list filter: adjacent ID rejected",    !can_filter_matches(&list_filter, &f6));

    CanFilter disabled = { .id = 0x000, .mask = 0x000, .enabled = false };
    check("disabled filter rejects everything",   !can_filter_matches(&disabled, &f1));
}

/* --- Test 3: dispatcher with multiple handlers --- */

static uint32_t handler_a_calls = 0;
static uint32_t handler_b_calls = 0;
static uint32_t last_received_id = 0;

static void handler_a(const CanFrame *frame, void *ctx)
{
    (void)ctx;
    handler_a_calls++;
    last_received_id = frame->id;
}

static void handler_b(const CanFrame *frame, void *ctx)
{
    (void)frame; (void)ctx;
    handler_b_calls++;
}

static void test_dispatcher(void)
{
    printf("\n=== Dispatcher ===\n");
    CanDispatcher disp;
    can_dispatcher_init(&disp);

    /* Handler A: accepts 0x100-0x1FF (mask) */
    CanFilter fa = { .id=0x100, .mask=0x700, .mode=CAN_FILTER_MASK,
                     .enabled=true, .extended=false };
    /* Handler B: accepts exactly 0x1A0 (list) */
    CanFilter fb = { .id=0x1A0, .mode=CAN_FILTER_LIST,
                     .enabled=true, .extended=false };

    can_dispatcher_register(&disp, &fa, handler_a, NULL);
    can_dispatcher_register(&disp, &fb, handler_b, NULL);

    handler_a_calls = handler_b_calls = 0;

    /* Frame 0x1A0 should match BOTH handlers */
    CanFrame f1 = { .id = 0x1A0, .ide = false };
    can_dispatcher_dispatch(&disp, &f1);
    check("0x1A0 triggers handler A (range match)", handler_a_calls == 1);
    check("0x1A0 triggers handler B (list match)",  handler_b_calls == 1);

    /* Frame 0x100 should match handler A only */
    CanFrame f2 = { .id = 0x100, .ide = false };
    can_dispatcher_dispatch(&disp, &f2);
    check("0x100 triggers handler A only (handler_a_calls=2)", handler_a_calls == 2);
    check("0x100 does not trigger handler B (handler_b_calls still 1)", handler_b_calls == 1);

    /* Frame 0x200 matches neither */
    CanFrame f3 = { .id = 0x200, .ide = false };
    can_dispatcher_dispatch(&disp, &f3);
    check("0x200 matches no handler -> unmatched_count=1", disp.unmatched_count == 1);

    /* Verify match counts in entries */
    check("entry[0] (handler A) match_count = 2", disp.entries[0].match_count == 2);
    check("entry[1] (handler B) match_count = 1", disp.entries[1].match_count == 1);
}

/* --- Test 4: per-ID statistics --- */
static void test_stats(void)
{
    printf("\n=== Per-ID statistics ===\n");
    CanStats stats;
    can_stats_init(&stats);

    /* Inject 5 frames for ID 0x100 at 100ms intervals */
    for (uint32_t i = 0; i < 5; i++) {
        CanFrame f = { .id = 0x100, .ide = false,
                       .timestamp_ms = 1000 + i * 100 };
        can_stats_update(&stats, &f);
    }

    /* Inject 2 frames for ID 0x200 */
    CanFrame g1 = { .id = 0x200, .timestamp_ms = 2000 };
    CanFrame g2 = { .id = 0x200, .timestamp_ms = 2050 };
    can_stats_update(&stats, &g1);
    can_stats_update(&stats, &g2);

    check("total frames = 7",    stats.total_frames == 7);
    check("unique IDs = 2",      stats.count == 2);
    check("ID 0x100 count = 5",  stats.entries[0].frame_count == 5);
    check("ID 0x200 count = 2",  stats.entries[1].frame_count == 2);

    /* Gap for ID 0x100: always 100ms */
    check("ID 0x100 min gap = 100ms", stats.entries[0].min_gap_ms == 100);
    check("ID 0x100 max gap = 100ms", stats.entries[0].max_gap_ms == 100);

    /* Timeout check: last seen at 1400ms, check at 1600ms with 100ms timeout */
    check("ID 0x100 timed out at 1600ms (gap=200>100)",
          can_stats_check_timeout(&stats, 0x100, 1600, 100));
    check("ID 0x100 not timed out at 1450ms (gap=50<100)",
          !can_stats_check_timeout(&stats, 0x100, 1450, 100));

    can_stats_print(&stats);
}

/* --- Test 5: full pipeline (ring + dispatcher + stats) --- */
static uint32_t pipeline_handler_count = 0;

static void pipeline_handler(const CanFrame *frame, void *ctx)
{
    (void)frame; (void)ctx;
    pipeline_handler_count++;
}

static void test_full_pipeline(void)
{
    printf("\n=== Full pipeline (ISR enqueue -> task process -> dispatch) ===\n");
    CanPipeline pipe;
    can_pipeline_init(&pipe);

    /* Register handler for ID range 0x300-0x3FF */
    CanFilter f = { .id=0x300, .mask=0x700, .mode=CAN_FILTER_MASK,
                    .enabled=true, .extended=false };
    can_dispatcher_register(&pipe.dispatcher, &f, pipeline_handler, NULL);

    pipeline_handler_count = 0;

    /* "ISR" enqueues 3 frames in range and 2 outside */
    CanFrame frames[5] = {
        { .id=0x300, .dlc=2, .data={0x01,0x02}, .timestamp_ms=1000 },
        { .id=0x310, .dlc=3, .data={0x03,0x04,0x05}, .timestamp_ms=1010 },
        { .id=0x400, .dlc=1, .data={0xAA}, .timestamp_ms=1020 },  /* outside */
        { .id=0x3FF, .dlc=0, .timestamp_ms=1030 },
        { .id=0x200, .dlc=4, .timestamp_ms=1040 },  /* outside */
    };
    for (int i = 0; i < 5; i++) {
        can_pipeline_isr_enqueue(&pipe, &frames[i]);
    }

    check("ring has 5 pending frames", can_rx_ring_count(&pipe.ring) == 5);

    uint32_t processed = can_pipeline_process(&pipe);
    check("processed 5 frames", processed == 5);
    check("ring empty after processing", can_rx_ring_count(&pipe.ring) == 0);

    check("handler called 3 times (0x300, 0x310, 0x3FF)",
          pipeline_handler_count == 3);
    check("2 unmatched frames (0x400, 0x200)",
          pipe.dispatcher.unmatched_count == 2);
    check("stats tracks 5 unique IDs (0x300, 0x310, 0x400, 0x3FF, 0x200)",
          pipe.stats.count == 5);
    check("stats total = 5",
          pipe.stats.total_frames == 5);
}

int main(void)
{
    test_ring_buffer();
    test_filters();
    test_dispatcher();
    test_stats();
    test_full_pipeline();

    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
