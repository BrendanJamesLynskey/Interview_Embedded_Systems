# Volatile and Memory Barriers

## Prerequisites
- C abstract machine model and compiler optimisation
- ARM Cortex-M memory model: store buffers, write buffers
- ISR and shared variable concepts
- Basic understanding of out-of-order execution (Cortex-M7)

---

## Concept Reference

### The `volatile` Keyword

`volatile` is a C type qualifier that tells the compiler: "Do not make assumptions about this object. Treat every read as a fresh load from memory and every write as a store to memory. Do not eliminate, merge, or reorder accesses to this object relative to other volatile accesses."

**What `volatile` guarantees:**
- Every read of a `volatile` object issues a load instruction.
- Every write to a `volatile` object issues a store instruction.
- Reads and writes are not eliminated (no dead-store elimination, no redundant-load elimination).
- Relative ordering among `volatile` accesses is preserved (compiler will not reorder volatile accesses relative to each other).

**What `volatile` does NOT guarantee:**
- Atomicity. A `volatile uint32_t` read-modify-write is still three instructions.
- Ordering relative to non-volatile accesses.
- CPU-level memory ordering (store buffers, cache coherency, out-of-order execution).

### When `volatile` Is Sufficient

`volatile` alone is sufficient when:
1. A single-core CPU accesses a memory-mapped hardware register (peripheral space, Device-type memory). The key requirement is that the CPU issues a real bus transaction -- `volatile` enforces this.
2. A flag is shared between an ISR and main code on a single-core CPU with a simple flag that is read or written atomically (e.g., a `volatile uint8_t` flag that is only ever set to 0 or 1, never incremented).

### When `volatile` Is Insufficient

`volatile` alone is insufficient when:
1. An ISR and main code share a multi-word data structure. Updating two words of a struct is two separate stores; an interrupt between them leaves the struct in an inconsistent state.
2. Multiple cores share data. The compiler reordering prevention of `volatile` applies only to the local compilation unit. Another core's CPU may observe stores in a different order.
3. DMA and cache are involved. `volatile` does not trigger a cache flush or invalidation.
4. Ordering relative to non-volatile operations matters (e.g., configuring a peripheral before enabling it).

### Memory Barriers on ARM Cortex-M

**DSB (Data Synchronisation Barrier) -- `__DSB()`**

Ensures all memory transactions (loads and stores) initiated before the DSB have completed before any instruction after the DSB is executed. This drains the store buffer and waits for cache write-backs to complete.

Use cases:
- After writing to a peripheral control register before reading a status register.
- After DMA setup writes before asserting the DMA enable bit.
- After cache maintenance operations (clean, invalidate).
- After enabling an interrupt at the NVIC before re-enabling global interrupts.

**DMB (Data Memory Barrier) -- `__DMB()`**

Ensures all memory transactions initiated before the DMB are observable by other observers (other cores, DMA) before any memory transaction after the DMB. Does not wait for completion, but ensures ordering of visibility.

Use cases:
- In a ring buffer between an ISR and main code: ensure data is written to the buffer before the index is advanced.
- In multi-core systems where one core writes data and another reads it.
- Between DMA buffer population and DMA enable.

**ISB (Instruction Synchronisation Barrier) -- `__ISB()`**

Flushes the instruction pipeline and ensures the CPU fetches subsequent instructions fresh. Used after:
- Updating the VTOR (vector table offset register).
- Enabling or disabling caches.
- Self-modifying code.
- Changing the CPU privilege level.

**Compiler Barrier -- `__asm volatile ("" ::: "memory")`**

Prevents the compiler from reordering instructions across the barrier and forces the compiler to assume all memory is potentially modified. Does not generate any machine instruction; it is a compiler-only fence.

```c
/* Prevent compiler from moving reads/writes across this point */
#define compiler_barrier()  __asm volatile ("" ::: "memory")
```

Use cases:
- Between operations that must remain in order but where the CPU hardware ordering is already guaranteed (e.g., within a single-core ISR on Cortex-M which has no hardware reordering on the AHB Device bus).
- When performance is critical and the full cost of `__DMB()` is unacceptable, and you know the platform provides hardware ordering.

---

## Tier 1 -- Fundamentals

### Question F1
**A developer uses a global `int done_flag` (without `volatile`) as a signal between an ISR and main code. The code compiles and runs correctly at `-O0` but fails at `-O2`. Explain exactly why.**

**Answer:**

At `-O0` (no optimisation), the compiler generates a load instruction for every C variable access. The loop:

```c
while (done_flag == 0) {}
```

compiles to something like:

```
.loop:
  LDR  R0, [done_flag_address]   ; load done_flag from memory
  CMP  R0, #0
  BEQ  .loop
```

At `-O0`, a real memory load occurs each iteration. When the ISR sets `done_flag = 1`, the main code's next load sees the updated value and exits the loop.

At `-O2` (optimisation enabled), the compiler analyses the loop:

1. `done_flag` is a plain `int`, not `volatile`.
2. Within the loop body, nothing modifies `done_flag` (from the compiler's point of view -- the ISR is invisible to the compiler's static analysis).
3. The compiler concludes: "On the first iteration, if `done_flag == 0`, it will be 0 on every subsequent iteration. This is either an infinite loop or a no-op loop."
4. The compiler may hoist the load outside the loop (hoist invariant load), producing:

```
  LDR  R0, [done_flag_address]   ; load ONCE, before the loop
  CMP  R0, #0
  BEQ  .infinite_loop            ; if 0, spin forever on a branch-to-self
```

The ISR sets the memory location, but the CPU is spinning on a branch that never re-reads memory. The program hangs.

**Fix:**

```c
volatile int done_flag = 0;  /* volatile prevents the load from being hoisted */
```

With `volatile`, the compiler is not allowed to assume the value is invariant. Every iteration generates a load instruction.

---

### Question F2
**Name three situations where `volatile` alone is not sufficient to protect shared data, and for each, state what additional mechanism is needed.**

**Answer:**

**Situation 1: Multi-word shared data structure**

```c
typedef struct {
    uint32_t timestamp;   /* milliseconds */
    uint32_t value;       /* sensor reading */
} sample_t;

volatile sample_t latest;  /* shared between ISR and main */

/* ISR writes: */
latest.timestamp = get_ms();   /* store 1 */
latest.value     = adc_read(); /* store 2 */
/* If main reads between store 1 and store 2, it gets a mismatched pair */
```

`volatile` ensures both stores happen, but does not make the two-store sequence atomic. An interrupt between the two stores corrupts the logical invariant.

**Fix:** Disable interrupts around the ISR update (if the ISR is the only writer), or use a double-buffer / sequence counter pattern.

**Situation 2: Multi-core shared data**

On a dual-core system (e.g., STM32H7 with Cortex-M7 + Cortex-M4):

```c
volatile int ready = 0;
volatile int data  = 0;

/* Core 0: */
data  = 42;      /* store to data */
ready = 1;       /* store to ready */

/* Core 1: */
while (ready == 0) {}
use(data);   /* DANGER: may see ready=1 but data still unwritten */
```

`volatile` prevents the *compiler* from reordering the two stores, but the Cortex-M7 store buffer may commit them to the shared interconnect in a different order. Core 1 could see `ready=1` before `data=42` has propagated through the bus.

**Fix:** Use `__DMB()` between the two stores (Core 0) and between the flag read and the data read (Core 1). Or use C11 `_Atomic` / `stdatomic.h`.

**Situation 3: ISR and DMA with cache enabled (Cortex-M7)**

```c
volatile uint8_t rx_buf[256];  /* DMA fills this */

/* In DMA TC ISR: */
process_data((uint8_t *)rx_buf, 256);
/* volatile forces each byte to be loaded -- but if the D-cache holds stale
   data for this SRAM region (from a previous DMA transfer that has been
   overwritten), volatile reads will return the cached (stale) value. */
```

`volatile` does not bypass the D-cache. If the cache line is valid but stale (DMA wrote new data to physical SRAM but the cache was not invalidated), volatile reads return the cached old value.

**Fix:** Invalidate the D-cache for the DMA buffer before reading (`SCB_InvalidateDCache_by_Addr`), or mark the buffer region as Non-Cacheable via the MPU.

---

### Question F3
**What is a compiler barrier? How does it differ from a hardware memory barrier, and when is it sufficient on its own?**

**Answer:**

**Compiler barrier:**

```c
__asm volatile ("" ::: "memory")
```

This inline assembly statement generates zero machine instructions but contains two compiler directives:
- `volatile`: the compiler cannot remove or move this asm statement.
- `"memory"` in the clobber list: tells the compiler that this asm can read or write any memory location; therefore, the compiler must assume all cached values in registers are invalidated and must not reorder any memory accesses across this point.

**Hardware memory barrier (`__DMB()`, `__DSB()`):**

These generate actual ARM instructions (`DMB` and `DSB`). In addition to the compiler ordering guarantee (implied by being an inline asm volatile with memory clobber), they also enforce ordering at the CPU hardware level:
- `DMB`: all memory transactions before the DMB are visible to other observers before any transaction after it.
- `DSB`: all memory transactions before the DSB have completed (bus transactions acknowledged) before execution continues.

**When compiler barrier alone is sufficient:**

On a single-core Cortex-M3/M4 accessing Device-type memory (peripheral registers), the CPU hardware guarantees that Device-memory accesses are issued strictly in program order and are not buffered in a way that could be observed out-of-order by the same core. In this case, the only risk is the *compiler* reordering; a compiler barrier is sufficient.

```c
/* Single-core, peripheral access -- compiler barrier is enough */
DMA->CR  = config_value;
compiler_barrier();     /* prevent compiler from moving EN write before CR write */
DMA->CR |= DMA_SxCR_EN;
```

**When hardware barrier is required:**

- Any access to Normal (cacheable) memory shared between code and DMA.
- Any access shared between multiple cores.
- After cache maintenance operations.
- When the ARMv7-M architecture explicitly requires a barrier (e.g., after VTOR write, after `NVIC_EnableIRQ`).

---

### Question F4
**What does the ARM DMB instruction do? Describe the difference between `DMB SY`, `DMB ST`, and `DMB LD`.**

**Answer:**

The `DMB` (Data Memory Barrier) instruction ensures that all memory transactions of a specified type issued before the DMB are globally observable before any memory transaction of the specified type after the DMB. It does not stall the pipeline waiting for transactions to complete (that is `DSB`'s job) -- it ensures ordering from other observers' perspective.

**Sharability and type options:**

`DMB` takes an option operand specifying the domain (which observers) and access type:

| Option | Observers | Types |
|--------|-----------|-------|
| `SY`   | Full system (all masters) | Loads and stores |
| `ST`   | Full system | Stores only |
| `LD`   | Full system | Loads only |
| `ISH`  | Inner Shareable domain | Loads and stores |
| `ISHST`| Inner Shareable domain | Stores only |
| `ISHLD`| Inner Shareable domain | Loads only |

**Practical meanings for Cortex-M:**

Most Cortex-M devices are single-processor with a simple bus, making the sharability domain largely academic. `DMB SY` (the default when no option is given in C intrinsics as `__DMB()`) is the most conservative and correct choice for all single and multi-core use cases on Cortex-M.

**`DMB ST` (stores only):**

Ensures all stores before the barrier are visible before any store after it. Reads/loads are not ordered. Used when the write of an index or flag must be visible before the write to a data region it protects, but reads are not a concern.

**`DMB LD` (loads only):**

Ensures all loads before the barrier complete before any load after it. Useful on the *consumer* side: read data before reading the index that tells you how much data there is.

**Ring buffer example using DMB options:**

```c
/* Producer (ISR): */
buf[head] = new_data;          /* store data */
__asm volatile ("dmb st" ::: "memory");  /* all stores before must be visible... */
head = (head + 1) & MASK;      /* ...before this store (index advance) */

/* Consumer (main): */
uint32_t h = head;             /* load index */
__asm volatile ("dmb ld" ::: "memory");  /* all loads before must complete... */
uint8_t d = buf[h - 1];        /* ...before this load (data read) */
```

In practice, CMSIS provides `__DMB()` which maps to `DMB SY` -- the safe, portable choice for all embedded use cases.

---

## Tier 2 -- Intermediate

### Question I1
**Write a correct ISR-safe ring buffer implementation for a single producer (ISR) and single consumer (main), explaining which barriers are needed and why, without disabling interrupts.**

**Answer:**

```c
/*
 * Lock-free single-producer, single-consumer ring buffer.
 *
 * Producer (ISR): writes data, then advances head.
 * Consumer (main): reads data at tail, then advances tail.
 *
 * Invariant: consumer only reads when tail != head.
 *            producer only writes when next_head != tail.
 *
 * Memory ordering requirements:
 *   Producer must ensure buf[head]=data is visible BEFORE head advances.
 *   If head advances before the data write is observable, the consumer
 *   could read uninitialised data.
 *
 *   Consumer must read head (the "available" indicator) BEFORE reading data.
 *   If the CPU reorders the data read before the head read, it could read
 *   data for a slot that the producer has not yet written.
 *
 * On Cortex-M3/M4 (no out-of-order execution): the CPU issues memory
 * transactions in program order. DMB is still needed for the compiler,
 * and for correctness on multi-core or DMA scenarios.
 *
 * On Cortex-M7 (weakly-ordered for Normal memory): DMB is required both
 * for compiler AND CPU ordering.
 */

#include <stdint.h>
#include <stddef.h>

#define RING_SIZE  256U   /* must be power of 2 */
#define RING_MASK  (RING_SIZE - 1U)

typedef struct {
    uint8_t          buf[RING_SIZE];
    volatile uint32_t head;   /* written by ISR (producer) */
    volatile uint32_t tail;   /* written by main (consumer) */
} ring_t;

static ring_t uart_ring;

/* -------------------------------------------------------------------------
 * Producer -- called from ISR
 * Pushes one byte. Returns 0 on success, -1 if buffer full.
 * -------------------------------------------------------------------------*/
int ring_push(ring_t *r, uint8_t byte)
{
    uint32_t next_head = (r->head + 1U) & RING_MASK;

    /* Snapshot tail once -- volatile ensures a real load */
    if (next_head == r->tail) {
        return -1;   /* full */
    }

    /* Write data to the slot */
    r->buf[r->head] = byte;

    /*
     * DMB: ensure the buf write above is observable to the consumer
     * BEFORE we advance head. Without this, a consumer on another core
     * (or a CPU with reorder capability) could see the updated head
     * pointing to a slot whose data has not yet been committed.
     *
     * On single-core Cortex-M3/M4 the CPU does not reorder, but DMB also
     * acts as a compiler barrier, preventing the compiler from moving the
     * head store before the buf store.
     */
    __DMB();

    /* Advance the head -- consumer can now see the new byte */
    r->head = next_head;

    return 0;
}

/* -------------------------------------------------------------------------
 * Consumer -- called from main (never from ISR)
 * Pops one byte. Returns 0 on success, -1 if empty.
 * -------------------------------------------------------------------------*/
int ring_pop(ring_t *r, uint8_t *out)
{
    /* Snapshot head -- this is the "data available" indicator */
    uint32_t head = r->head;

    /*
     * DMB: ensure we read head BEFORE reading buf[tail].
     * Without this, a CPU with load-load reordering could speculatively
     * load buf[tail] before the head read, potentially reading a slot
     * the producer has not yet written.
     */
    __DMB();

    if (r->tail == head) {
        return -1;   /* empty */
    }

    /* Read the data */
    *out = r->buf[r->tail];

    /*
     * DMB: ensure buf[tail] is read before we advance tail.
     * The producer checks tail to determine free space; we must not
     * advance tail (freeing the slot) before we are done reading it.
     */
    __DMB();

    /* Advance tail */
    r->tail = (r->tail + 1U) & RING_MASK;

    return 0;
}
```

**Why not use `__disable_irq()`/`__enable_irq()` here:**

Disabling interrupts to protect the ring buffer would work but is heavier than necessary. The lock-free approach has lower latency and avoids the risk of missing interrupts during the critical section. The DMB barriers ensure the required memory ordering with no IRQ masking.

---

### Question I2
**Explain the difference between `__DSB()` and `__DMB()` with a concrete peripheral access example where one is required and the other is not sufficient.**

**Answer:**

**`__DMB()` (Data Memory Barrier):**
Ensures ordering of visibility: all accesses before the DMB become observable to other observers before any access after the DMB. The CPU may continue executing after issuing the DMB without waiting for outstanding bus transactions to complete (acknowledgement not waited for).

**`__DSB()` (Data Synchronisation Barrier):**
Ensures completion: all accesses before the DSB have fully completed (bus transactions acknowledged by the target slave) before execution continues past the DSB. No subsequent instruction begins until all outstanding bus transactions are done.

**Example where DMB is insufficient -- DMA enable sequence:**

```c
DMA->M0AR = (uint32_t)buffer;    /* configure destination address */
DMA->NDTR = 256U;                 /* configure transfer count */
DMA->CR   = channel_config;       /* configure channel (direction, increment) */

__DMB();   /* INSUFFICIENT: orders visibility, but does not guarantee the DMA
              controller's registers have been updated before EN is written */

DMA->CR  |= DMA_SxCR_EN;         /* enable DMA */
/* DMA controller begins reading M0AR, NDTR, CR.
   If the previous writes have not actually reached the DMA peripheral
   (still in CPU store buffer), DMA reads stale register values. */
```

The DMA peripheral is a hardware master that reads its own configuration registers. Those registers must be in their correct state at the hardware level before the enable bit is set. `__DMB()` ensures ordering from the CPU's perspective but does not guarantee the write has propagated through the store buffer to the actual DMA register.

**Correct: use `__DSB()`:**

```c
DMA->M0AR = (uint32_t)buffer;
DMA->NDTR = 256U;
DMA->CR   = channel_config;

__DSB();   /* CORRECT: ensures all preceding writes have completed the bus;
              DMA registers are guaranteed to hold the written values before
              execution proceeds to the EN write */

DMA->CR  |= DMA_SxCR_EN;   /* safe: DMA sees correct configuration */
```

**Why DMB might suffice in simpler cases:**

On Cortex-M3/M4 accessing Device-type memory (which peripheral registers always are), stores are weakly ordered in the store buffer but Device memory is required to be non-bufferable (stores issued in program order, not merged). In practice, `__DMB()` is often sufficient for Device memory on these cores because the non-bufferable property already ensures ordered completion. However, `__DSB()` is the architecturally correct and portable choice for sequences where one peripheral access must complete before another begins.

---

### Question I3
**Describe what happens to a `volatile` read-modify-write operation on a peripheral register in the presence of a higher-priority interrupt. Provide a concrete failure scenario and the correct fix.**

**Answer:**

A read-modify-write of a volatile register compiles to three separate instructions:

```c
GPIOA->ODR |= (1U << 5);   /* set PA5 */

/* Compiles to (approximately): */
LDR  R0, [GPIOA_ODR]    ; instruction 1: read ODR
ORR  R0, R0, #0x20      ; instruction 2: OR with bitmask
STR  R0, [GPIOA_ODR]    ; instruction 3: write back
```

A higher-priority interrupt can fire between any of these instructions.

**Failure scenario:**

```
Initial state: ODR = 0x0000 (all pins low)

Main code issues: GPIOA->ODR |= (1U << 5);   /* wants to set PA5 */

  1. LDR R0, [GPIOA_ODR]   -> R0 = 0x0000
  2. <-- INTERRUPT FIRES HERE -->
       ISR executes: GPIOA->ODR |= (1U << 3);  /* ISR wants to set PA3 */
         LDR R1, [GPIOA_ODR]  -> R1 = 0x0000
         ORR R1, R1, #0x08    -> R1 = 0x0008
         STR R1, [GPIOA_ODR]  -> ODR is now 0x0008 (PA3 high, PA5 low)
       ISR returns
  3. ORR R0, R0, #0x20     -> R0 = 0x0020  (main code has STALE ODR value)
  4. STR R0, [GPIOA_ODR]   -> ODR = 0x0020 (PA5 high, but PA3 CLEARED!)

Result: PA3 has been inadvertently cleared, even though the ISR set it.
```

**Fix option 1: Use BSRR for GPIO (preferred):**

```c
GPIOA->BSRR = (1U << 5);   /* single write, atomic, no read needed */
```

BSRR is a write-only register where writing a bit atomically sets the corresponding ODR bit. No read-modify-write required.

**Fix option 2: Disable interrupts around the critical section:**

```c
uint32_t saved = __get_PRIMASK();
__disable_irq();
GPIOA->ODR |= (1U << 5);
__set_PRIMASK(saved);
```

This prevents the interrupt from firing between the read and the write. The save/restore pattern (rather than unconditional enable) handles the case where interrupts were already disabled by a caller.

**Fix option 3: Use exclusive access (LDREX/STREX) -- Cortex-M3+ only:**

```c
/* For non-GPIO peripheral registers that lack a dedicated atomic mechanism */
uint32_t old_val, new_val;
do {
    old_val = __LDREXW((volatile uint32_t *)&SOME_PERIPH->REG);
    new_val = old_val | (1U << 5);
} while (__STREXW(new_val, (volatile uint32_t *)&SOME_PERIPH->REG));
```

`LDREX`/`STREX` implement load-linked/store-conditional: the `STREX` fails if any other exception modified the address between the `LDREX` and the `STREX`, forcing a retry. This is lock-free and non-blocking. Note: `LDREX`/`STREX` work on Normal memory; on Device memory (peripheral registers) results are unpredictable, making this pattern suitable only for SRAM-mapped state, not peripheral registers.

---

## Tier 3 -- Advanced

### Question A1
**On a Cortex-M7 with a data cache, explain why a `volatile` spin-wait on a DMA-written memory location can fail even with a correctly placed `__DMB()`, and what the correct solution is.**

**Answer:**

**The scenario:**

```c
/* DMA writes a "done" token to a specific SRAM location when a transfer completes */
volatile uint32_t *dma_done_flag = (volatile uint32_t *)0x20001000U;

/* CPU spins waiting for DMA to set the flag */
while (*dma_done_flag == 0U) {
    __DMB();
}
```

**Why this fails with a D-cache:**

1. The D-cache on Cortex-M7 operates on Normal memory (Write-Back, Write-Allocate by default for SRAM).
2. Before the DMA starts, the CPU may have read from address 0x20001000. The cache allocates a line for this address, caching the value `0` (the initial value).
3. The DMA writes `1` to physical SRAM address 0x20001000. This write bypasses the cache entirely.
4. The CPU's spin loop reads `*dma_done_flag`. `volatile` forces a load instruction, but the load goes to the cache, not to physical SRAM. The cache line is still valid (not invalidated), so the cache returns the stale `0`.
5. `__DMB()` ensures ordering between loads and stores in the CPU's perspective, but it does not flush or bypass the D-cache.

The spin loop runs forever because the cache shields the CPU from seeing the DMA's write.

**The `volatile` keyword's limitation:**

`volatile` forces a load *instruction* to be emitted. It does not force the load to bypass the cache. The CPU issues the load to the L1 D-cache; if the cache line is present and valid, the cache services the request without going to SRAM.

**Solutions:**

**Option 1: Mark the flag as Non-Cacheable via MPU**

```c
/* Configure MPU region for DMA-shared variables as Non-Cacheable */
/* (see peripheral_register_access.md for full MPU setup) */
/* All accesses to this region bypass the cache */
volatile uint32_t *dma_done_flag = (volatile uint32_t *)0x24000000U; /* NCB region */
```

`volatile` + Non-Cacheable: every load issues a real bus transaction to SRAM. DMA writes are immediately visible.

**Option 2: Invalidate the cache line before reading**

```c
/* In the DMA TC ISR (which fires when DMA is done): */
void DMA1_Stream0_IRQHandler(void)
{
    if (DMA1->LISR & DMA_LISR_TCIF0) {
        DMA1->LIFCR = DMA_LIFCR_CTCIF0;
        SCB_InvalidateDCache_by_Addr((uint32_t *)0x20001000U, 4U);
        __DSB();
        /* Now set a flag that main can poll using a volatile variable
           that is also invalidated, or signal via SRAM */
        dma_complete = 1;
    }
}
```

**Option 3: Use a DMA transfer complete interrupt instead of polling**

The architecturally correct approach for "wait for DMA to finish" is to use the DMA transfer-complete interrupt and perform work in the ISR or set a semaphore/flag there. Polling in a spin loop is wasteful and, as shown, fragile with caches. The interrupt approach:

```c
void DMA1_Stream0_IRQHandler(void)
{
    if (DMA1->LISR & DMA_LISR_TCIF0) {
        DMA1->LIFCR = DMA_LIFCR_CTCIF0;
        /* Invalidate cache for the destination buffer */
        SCB_InvalidateDCache_by_Addr((uint32_t *)rx_buffer, BUFFER_SIZE);
        __DSB();
        /* Notify application */
        rx_complete = 1;
    }
}
```

Main code checks `rx_complete` (in `.bss`, not involved in DMA, normal SRAM -- volatile flag is sufficient here because no cache coherency issue: main code writes the flag to 0 and the ISR writes it to 1, and both accesses happen through the CPU cache consistently).

---

### Question A2
**Describe the "store-to-load forwarding" optimisation on Cortex-M7 and explain a scenario where relying on it for inter-task communication introduces a correctness hazard.**

**Answer:**

**Store-to-load forwarding:**

The Cortex-M7 is a superscalar, out-of-order processor. It contains a store buffer that holds pending (not yet committed to cache) write operations. When a subsequent load accesses the same address as a pending store, the processor can "forward" the store data directly to the load without waiting for the store to commit to the cache. This is a performance optimisation that hides store latency.

**Why it creates a hazard for inter-task communication:**

Store-to-load forwarding is local to the CPU core's store buffer. It only forwards data that the same core wrote. If data is written by one entity (DMA, another core, an ISR that runs on the same pipeline) and read by another code path, forwarding does not apply -- but the store buffer still holds the write in a pending state.

**Concrete scenario on single-core Cortex-M7:**

```c
/* Shared data written in an ISR, read in main: */
static uint32_t message_data;     /* Normal, Write-Back cached SRAM */
static volatile uint32_t message_ready;

/* ISR: */
void TIM2_IRQHandler(void)
{
    message_data  = compute_result();   /* store -- may enter store buffer */
    __DMB();                             /* programmer's intended ordering barrier */
    message_ready = 1U;                 /* store -- after DMB */
}

/* Main: */
while (message_ready == 0U) {}          /* spin on volatile */
uint32_t d = message_data;              /* read */
```

When the ISR executes:
1. `message_data = result` enters the store buffer.
2. `__DMB()` ensures the `message_data` store is visible before the `message_ready` store.
3. `message_ready = 1` is committed.

When main reads `message_data` after seeing `message_ready == 1`:
4. The load of `message_ready` returns 1 (correct -- committed).
5. The load of `message_data` may receive the value from the store buffer (forwarding), which is the correct value -- no bug here.

**Where the hazard arises -- multiple stores without barrier:**

```c
/* ISR writes two fields without a barrier between them: */
void ISR(void)
{
    shared.a = compute_a();   /* store 1 into store buffer */
    shared.b = compute_b();   /* store 2 into store buffer */
    /* No DMB -- the CPU is allowed to reorder the visibility of these
       stores to the cache (and thus to other observers) */
    ready = 1;                /* store 3 -- no barrier from stores 1 and 2 */
}

/* Main: */
while (!ready) {}
use(shared.a, shared.b);
/* If stores 1 and 2 were reordered (e.g., store 2 committed before store 1),
   main sees shared.b = new value but shared.a = old value. */
```

The Cortex-M7 architecture permits store-store reordering for Normal memory. Without `__DMB()` between the data stores and the flag store, the consumer may observe an inconsistent view.

**Correct pattern:**

```c
void ISR(void)
{
    shared.a = compute_a();
    shared.b = compute_b();
    __DMB();     /* all stores above must be visible before ready is set */
    ready = 1;
}

/* Main: */
while (!ready) {}
__DMB();         /* all loads below see what was committed before ready=1 */
use(shared.a, shared.b);
```

The `__DMB()` on the consumer side is required because the Cortex-M7 architecture permits load-load reordering for Normal memory: the CPU could speculatively load `shared.a` and `shared.b` before the load of `ready` completes, potentially getting old values even though `ready == 1`.
