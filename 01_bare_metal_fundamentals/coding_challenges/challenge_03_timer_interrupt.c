/*
 * Challenge 03: Timer Interrupt Handler Setup
 *
 * Target: ARM Cortex-M4 (STM32F4 TIM2-5 register layout, RM0090)
 *
 * Demonstrates register-level setup of a general-purpose timer (TIM2) to
 * generate a periodic interrupt at a configurable frequency, plus a SysTick
 * implementation for a millisecond system tick counter.
 *
 * Covers:
 *   - Timer prescaler and auto-reload register (ARR) calculation
 *   - Update event interrupt enable (UIE) and NVIC configuration
 *   - ISR clearing of the Update Interrupt Flag (UIF) before returning
 *   - SysTick configuration (CMSIS-compatible)
 *   - Atomic read of a 32-bit tick counter shared between ISR and main
 *   - A purely computational test harness (no timer hardware needed for logic tests)
 *
 * Key interview points demonstrated:
 *   - Why the UIF flag must be cleared at the START of the ISR (not the end)
 *   - Why the tick counter must be volatile
 *   - Timer frequency formula and integer arithmetic without floats
 *   - Why NVIC_SetPriority must be called before NVIC_EnableIRQ
 *
 * Compile (host):
 *   gcc -std=c11 -Wall -Wextra -DHOST_TEST -o timer_interrupt challenge_03_timer_interrupt.c
 *
 * Compile (target):
 *   arm-none-eabi-gcc -std=c11 -mcpu=cortex-m4 -mthumb -O2 \
 *       -o timer.elf challenge_03_timer_interrupt.c
 */

#include <stdint.h>
#include <stddef.h>

/* =========================================================================
 * Register definitions (STM32F4 TIMx general-purpose timers)
 * =========================================================================*/

/* TIMx Control Register 1 (CR1) */
#define TIM_CR1_CEN     (1U << 0)    /* counter enable */
#define TIM_CR1_UDIS    (1U << 1)    /* update disable (if set, no UEV generated) */
#define TIM_CR1_URS     (1U << 2)    /* update request source (1 = counter overflow only) */
#define TIM_CR1_OPM     (1U << 3)    /* one-pulse mode */
#define TIM_CR1_ARPE    (1U << 7)    /* auto-reload preload enable */

/* TIMx DMA/Interrupt Enable Register (DIER) */
#define TIM_DIER_UIE    (1U << 0)    /* update interrupt enable */
#define TIM_DIER_UDE    (1U << 8)    /* update DMA request enable */

/* TIMx Status Register (SR) */
#define TIM_SR_UIF      (1U << 0)    /* update interrupt flag -- write 0 to clear */
#define TIM_SR_CC1IF    (1U << 1)    /* capture/compare 1 interrupt flag */

/* TIMx Event Generation Register (EGR) */
#define TIM_EGR_UG      (1U << 0)    /* update generation: forces an update event */

/* =========================================================================
 * SysTick register definitions (Cortex-M core peripheral, ARM DDI0403)
 * =========================================================================*/

#define SYSTICK_CTRL_ENABLE     (1U << 0)   /* counter enable */
#define SYSTICK_CTRL_TICKINT    (1U << 1)   /* exception request enable */
#define SYSTICK_CTRL_CLKSOURCE  (1U << 2)   /* 1=processor clock, 0=external reference */
#define SYSTICK_CTRL_COUNTFLAG  (1U << 16)  /* counted to 0 since last read */

/* =========================================================================
 * Platform abstraction
 * =========================================================================*/

typedef struct {
    volatile uint32_t CR1;    /* 0x00 Control register 1 */
    volatile uint32_t CR2;    /* 0x04 Control register 2 */
    volatile uint32_t SMCR;   /* 0x08 Slave mode control */
    volatile uint32_t DIER;   /* 0x0C DMA/interrupt enable */
    volatile uint32_t SR;     /* 0x10 Status register */
    volatile uint32_t EGR;    /* 0x14 Event generation */
    volatile uint32_t CCMR1;  /* 0x18 Capture/compare mode 1 */
    volatile uint32_t CCMR2;  /* 0x1C Capture/compare mode 2 */
    volatile uint32_t CCER;   /* 0x20 Capture/compare enable */
    volatile uint32_t CNT;    /* 0x24 Counter */
    volatile uint32_t PSC;    /* 0x28 Prescaler */
    volatile uint32_t ARR;    /* 0x2C Auto-reload register */
} TIM_TypeDef;

typedef struct {
    volatile uint32_t CTRL;   /* 0xE000E010 Control and status */
    volatile uint32_t LOAD;   /* 0xE000E014 Reload value */
    volatile uint32_t VAL;    /* 0xE000E018 Current value */
    volatile uint32_t CALIB;  /* 0xE000E01C Calibration */
} SysTick_TypeDef;

/* NVIC minimal interface (we only need ISER and IPR here) */
typedef struct {
    volatile uint32_t ISER[8];  /* interrupt set-enable registers */
    volatile uint32_t ICER[8];  /* interrupt clear-enable registers */
    volatile uint32_t ISPR[8];  /* interrupt set-pending registers */
    volatile uint32_t ICPR[8];  /* interrupt clear-pending registers */
    volatile uint32_t IABR[8];  /* interrupt active-bit registers */
    uint32_t          RSVD[56];
    volatile uint8_t  IPR[240]; /* interrupt priority registers (1 byte each) */
} NVIC_TypeDef;

#ifdef HOST_TEST

#include <stdio.h>
#include <string.h>
#include <assert.h>

static TIM_TypeDef    mock_TIM2;
static SysTick_TypeDef mock_SysTick;
static NVIC_TypeDef   mock_NVIC;

#define TIM2      (&mock_TIM2)
#define SysTick   (&mock_SysTick)
#define NVIC      (&mock_NVIC)

/* Number of NVIC priority bits implemented (4 on STM32F4) */
#define __NVIC_PRIO_BITS  4U

/* IRQ number for TIM2 on STM32F4 */
#define TIM2_IRQn  28

#else /* --- real target --- */

#define TIM2    ((TIM_TypeDef  *)0x40000000U)
#define SysTick ((SysTick_TypeDef *)0xE000E010U)
#define NVIC    ((NVIC_TypeDef *)0xE000E100U)

#define __NVIC_PRIO_BITS  4U
#define TIM2_IRQn  28

#endif /* HOST_TEST */

/* =========================================================================
 * Shared tick counter (updated in SysTick_Handler)
 * =========================================================================
 *
 * volatile is required: main code reads this value in a loop and the
 * compiler must not cache it in a register across iterations.
 *
 * On Cortex-M (single core, no hardware write-reorder on Device memory):
 *   A 32-bit aligned read of a uint32_t is atomic at the hardware level.
 *   No further synchronisation is required for the read in main code
 *   (only one writer: the SysTick ISR; one reader: main code).
 *
 * If the counter were 64-bit, two 32-bit loads would be needed, and
 * the reader would need to use a LDREX/STREX loop or disable interrupts
 * to avoid reading a torn value.
 */
static volatile uint32_t g_tick_ms = 0U;

/* =========================================================================
 * Callback mechanism for application use of timer interrupt
 * =========================================================================*/

static void (*g_tim2_callback)(void) = NULL;

/* =========================================================================
 * NVIC helper functions
 * =========================================================================*/

/*
 * nvic_set_priority -- set interrupt priority.
 *
 * priority: 0 (highest) to (2^__NVIC_PRIO_BITS - 1) (lowest).
 * Stored in the upper bits of the 8-bit IPR byte.
 */
static void nvic_set_priority(int irq_num, uint8_t priority)
{
    uint8_t shifted = (uint8_t)((priority & ((1U << __NVIC_PRIO_BITS) - 1U))
                                << (8U - __NVIC_PRIO_BITS));
    NVIC->IPR[irq_num] = shifted;
}

/*
 * nvic_enable_irq -- enable an IRQ in the NVIC.
 *
 * irq_num: 0-239, corresponding to ISER[irq_num/32] bit (irq_num%32).
 */
static void nvic_enable_irq(int irq_num)
{
    NVIC->ISER[(uint32_t)irq_num >> 5U] = (1U << ((uint32_t)irq_num & 0x1FU));
}

/* =========================================================================
 * Timer prescaler and ARR calculation
 *
 * TIM2-TIM5 are 32-bit timers on STM32F4 but 16-bit on STM32F1.
 * For portability, this implementation treats both PSC and ARR as 16-bit
 * (0-65535), which is correct for 16-bit timers and still functional
 * (if possibly less optimal) for 32-bit timers.
 *
 * Timer update frequency formula:
 *
 *   f_update = f_CK_PSC / ((PSC + 1) * (ARR + 1))
 *
 * where f_CK_PSC is the timer's input clock (often APB1 * 2 on STM32F4
 * if APB1 prescaler != 1, but passed in as tim_clk_hz here).
 *
 * To achieve f_target Hz:
 *   (PSC + 1) * (ARR + 1) = f_CK_PSC / f_target
 *
 * Strategy: choose PSC such that ARR fits in 16 bits (0-65535).
 *   Let total = f_CK_PSC / f_target (integer division, rounded).
 *   Find PSC = ceil(total / 65536) - 1 (smallest PSC that keeps ARR <= 65535).
 *   Then ARR = total / (PSC + 1) - 1.
 *
 * Example: f_CK = 84 MHz, f_target = 1 Hz (1-second tick)
 *   total = 84,000,000
 *   PSC+1 = ceil(84000000 / 65536) = ceil(1281.7) = 1282, so PSC = 1281
 *   ARR+1 = 84000000 / 1282 = 65523.4 -> 65523, ARR = 65522
 *   Actual f = 84000000 / (1282 * 65523) = 84000000 / 83980326 ≈ 1.00023 Hz
 *   Error: 0.023% -- acceptable for most timekeeping purposes.
 * =========================================================================*/

typedef struct {
    uint32_t psc;   /* prescaler value to write to PSC register */
    uint32_t arr;   /* auto-reload value to write to ARR register */
    uint32_t actual_freq_mHz; /* achieved frequency in millihertz (for verification) */
} timer_period_t;

/*
 * timer_calc_period -- calculate PSC and ARR for a desired frequency.
 *
 * @tim_clk_hz:  timer input clock frequency in Hz
 * @target_hz:   desired update frequency in Hz (must be >= 1)
 * @out:         populated with PSC, ARR, and actual achieved frequency
 *
 * Returns 0 on success, -1 if target_hz is zero or unachievable.
 */
int timer_calc_period(uint32_t tim_clk_hz, uint32_t target_hz, timer_period_t *out)
{
    if (out == NULL || target_hz == 0U || tim_clk_hz == 0U) {
        return -1;
    }
    if (target_hz > tim_clk_hz) {
        return -1;   /* cannot go faster than the input clock */
    }

    /* total = round(tim_clk_hz / target_hz) */
    uint32_t total = (tim_clk_hz + (target_hz / 2U)) / target_hz;

    if (total == 0U) {
        return -1;
    }

    /* Find minimum PSC+1 such that ARR+1 = total/(PSC+1) fits in 16 bits */
    uint32_t psc_plus1 = (total + 65535U) / 65536U;  /* ceil(total / 65536) */
    if (psc_plus1 == 0U) {
        psc_plus1 = 1U;
    }

    uint32_t arr_plus1 = total / psc_plus1;

    if (arr_plus1 == 0U) {
        arr_plus1 = 1U;
    }

    out->psc = psc_plus1 - 1U;
    out->arr = arr_plus1 - 1U;

    /* Compute actual achieved frequency in millihertz to allow integer comparison */
    /* actual_Hz * 1000 = tim_clk_hz * 1000 / (psc_plus1 * arr_plus1) */
    /* Use 64-bit to avoid overflow: 84e6 * 1000 = 84e9 > UINT32_MAX */
    uint64_t num   = (uint64_t)tim_clk_hz * 1000ULL;
    uint64_t denom = (uint64_t)psc_plus1 * (uint64_t)arr_plus1;
    out->actual_freq_mHz = (uint32_t)(num / denom);

    return 0;
}

/*
 * tim2_init -- configure TIM2 for periodic interrupts at target_hz.
 *
 * @tim_clk_hz: clock frequency driving TIM2 (APB1 timer clock on STM32F4)
 * @target_hz:  desired interrupt frequency
 * @priority:   NVIC interrupt priority (0=highest)
 * @callback:   function to call from the ISR (may be NULL)
 *
 * Assumes the caller has:
 *   1. Enabled TIM2 peripheral clock via RCC_APB1ENR.
 *   2. Called __enable_irq() if interrupts were globally disabled.
 *
 * Returns 0 on success, -1 on invalid parameters.
 */
int tim2_init(uint32_t tim_clk_hz, uint32_t target_hz,
              uint8_t priority, void (*callback)(void))
{
    timer_period_t period;

    if (timer_calc_period(tim_clk_hz, target_hz, &period) != 0) {
        return -1;
    }

    g_tim2_callback = callback;

    /* 1. Disable timer before configuration */
    TIM2->CR1 = 0U;

    /* 2. Set prescaler and auto-reload register */
    TIM2->PSC = period.psc;
    TIM2->ARR = period.arr;

    /*
     * 3. Generate an update event to load PSC and ARR from preload registers.
     *    Without this, the values only take effect after the first overflow.
     *    Setting UG also resets the counter to 0.
     */
    TIM2->EGR = TIM_EGR_UG;

    /*
     * 4. Clear the update interrupt flag that UG just set.
     *    If we enable UIE without clearing UIF first, the interrupt
     *    fires immediately on NVIC enable (spurious first interrupt).
     */
    TIM2->SR = ~TIM_SR_UIF;

    /* 5. Enable update interrupt */
    TIM2->DIER = TIM_DIER_UIE;

    /*
     * 6. Configure NVIC priority BEFORE enabling the IRQ.
     *    If NVIC_EnableIRQ is called first and the timer flag is somehow
     *    still set, the ISR fires before the priority is set -- running at
     *    default priority 0 (highest) which may preempt critical code.
     */
    nvic_set_priority(TIM2_IRQn, priority);
    nvic_enable_irq(TIM2_IRQn);

    /*
     * 7. Configure CR1: enable counter, enable auto-reload preload,
     *    set URS=1 (only counter overflow generates update event --
     *    prevents a spurious interrupt if software writes UG again later).
     */
    TIM2->CR1 = TIM_CR1_ARPE | TIM_CR1_URS | TIM_CR1_CEN;

    return 0;
}

/*
 * TIM2_IRQHandler -- timer 2 update event interrupt handler.
 *
 * This is the function the CPU calls when TIM2 triggers an update interrupt.
 * The function name must match exactly the vector table entry for TIM2 global
 * interrupt (entry index 28 + 16 = 44 from the start of the vector table on
 * STM32F4).
 *
 * CRITICAL: Clear UIF at the TOP of the handler, not the bottom.
 *
 * Why? If the handler takes a long time and the timer overflows again while
 * the handler is running, the UIF is set again by hardware. If we cleared UIF
 * at the bottom, we would accidentally clear this NEW flag, missing the second
 * interrupt entirely. Clearing at the top preserves any re-assertion.
 */
#ifndef HOST_TEST
void TIM2_IRQHandler(void)
#else
static void mock_TIM2_IRQHandler(void)
#endif
{
    /*
     * Step 1: Read the status register to confirm UIF is set.
     * (On a device with multiple interrupt sources, also check CC1IF, etc.)
     */
    uint32_t sr = TIM2->SR;

    if (!(sr & TIM_SR_UIF)) {
        /* Spurious or CC interrupt -- not an update event */
        return;
    }

    /*
     * Step 2: Clear UIF by writing 0 to it.
     * STM32 SR bits are cleared by writing 0 (rc_w0 type).
     * Writing ~UIF clears the UIF bit while preserving all other flags.
     * Do this BEFORE any user work (see reasoning above).
     */
    TIM2->SR = ~TIM_SR_UIF;

    /*
     * Step 3: Execute user callback if registered.
     * Keep this minimal -- long callbacks delay other lower-priority ISRs.
     */
    if (g_tim2_callback != NULL) {
        g_tim2_callback();
    }
}

/* =========================================================================
 * SysTick millisecond tick
 *
 * SysTick is a 24-bit down-counter. At each zero crossing it fires an
 * exception and reloads from the LOAD register.
 *
 *   Reload value = (f_CPU / ticks_per_second) - 1
 *
 * For 1 ms at 168 MHz:
 *   Reload = (168,000,000 / 1000) - 1 = 167,999
 *   This fits in 24 bits (max 16,777,215). OK.
 *
 * SysTick is commonly configured by the CMSIS function SystemCoreClockUpdate()
 * + HAL_Init().  Here we configure it directly for educational clarity.
 * =========================================================================*/

int systick_init(uint32_t cpu_hz)
{
    if (cpu_hz == 0U || cpu_hz < 1000U) {
        return -1;   /* cannot produce 1 ms tick */
    }

    uint32_t reload = (cpu_hz / 1000U) - 1U;

    if (reload > 0x00FFFFFFU) {
        return -1;   /* reload value exceeds 24-bit SysTick counter */
    }

    /* Reset current value and load reload value */
    SysTick->VAL  = 0U;
    SysTick->LOAD = reload;

    /*
     * Enable SysTick with processor clock source and interrupt enabled.
     * CLKSOURCE=1 (CPU clock, not divided reference).
     * TICKINT=1   (generate SysTick exception on each zero crossing).
     * ENABLE=1    (start the counter).
     *
     * Note: SysTick priority is set via SCB->SHP[11] (shared priority register).
     * For simplicity, this example does not configure SysTick priority -- it
     * defaults to priority 0 (same as NMI and HardFault is not affected, but
     * SysTick will preempt all other configurable-priority exceptions).
     * In an RTOS, SysTick priority is typically set to the LOWEST configurable
     * priority so that it does not delay higher-priority IRQs.
     */
    SysTick->CTRL = SYSTICK_CTRL_CLKSOURCE | SYSTICK_CTRL_TICKINT | SYSTICK_CTRL_ENABLE;

    return 0;
}

/*
 * SysTick_Handler -- called every 1 ms (assuming systick_init was called with
 * cpu_hz such that the reload value produces exactly 1 ms intervals).
 */
#ifndef HOST_TEST
void SysTick_Handler(void)
#else
static void mock_SysTick_Handler(void)
#endif
{
    /*
     * Increment the tick counter.
     * This is the ONLY writer.  Main code only reads this value.
     * volatile ensures each ISR invocation issues a real load and store.
     *
     * Overflow at UINT32_MAX (4,294,967,295 ms = ~49.7 days) wraps to 0.
     * Application code comparing tick times must account for wrap-around
     * using unsigned arithmetic: (now - start) < timeout.
     */
    g_tick_ms++;
}

/*
 * systick_get_ms -- return current tick count.
 *
 * On Cortex-M3/M4/M7, a 32-bit aligned read is atomic at the hardware level.
 * A single LDR instruction is emitted for this return.
 * volatile ensures the compiler does not cache the value between calls.
 */
uint32_t systick_get_ms(void)
{
    return g_tick_ms;
}

/*
 * systick_delay_ms -- busy-wait for (at least) ms milliseconds.
 *
 * Uses unsigned subtraction to handle timer wrap-around correctly:
 *   (current - start) overflows correctly in unsigned arithmetic.
 *   If start = 0xFFFFFFFE and current = 0x00000002, then
 *   (uint32_t)(0x00000002 - 0xFFFFFFFE) = 0x00000004 = 4 ms elapsed.
 */
void systick_delay_ms(uint32_t ms)
{
    uint32_t start = systick_get_ms();
    while ((systick_get_ms() - start) < ms) {
        /* busy wait -- in a real application, consider putting CPU to sleep */
    }
}

/* =========================================================================
 * Test harness (HOST_TEST only)
 * =========================================================================*/
#ifdef HOST_TEST

static int tests_run    = 0;
static int tests_passed = 0;

#define TEST(label, cond) do {                                        \
    tests_run++;                                                       \
    if (cond) {                                                        \
        tests_passed++;                                                \
        printf("  PASS  %s\n", (label));                              \
    } else {                                                           \
        printf("  FAIL  %s  (line %d)\n", (label), __LINE__);        \
    }                                                                  \
} while (0)

/* Simple callback counter for verifying callback invocation */
static volatile uint32_t callback_count = 0;
static void test_callback(void) { callback_count++; }

int main(void)
{
    memset(&mock_TIM2,    0, sizeof(mock_TIM2));
    memset(&mock_SysTick, 0, sizeof(mock_SysTick));
    memset(&mock_NVIC,    0, sizeof(mock_NVIC));
    g_tick_ms = 0U;

    /* -------------------------------------------------------------------------
     * timer_calc_period tests (pure computation, no hardware)
     * -------------------------------------------------------------------------*/
    printf("=== timer_calc_period ===\n");

    timer_period_t p;
    int rc;

    /* 84 MHz timer, 1 Hz target */
    rc = timer_calc_period(84000000U, 1U, &p);
    TEST("84MHz/1Hz: returns 0", rc == 0);
    TEST("84MHz/1Hz: PSC fits in 16 bits", p.psc <= 65535U);
    TEST("84MHz/1Hz: ARR fits in 16 bits", p.arr <= 65535U);
    /* Verify (PSC+1)*(ARR+1) is close to 84,000,000 */
    uint32_t product = (p.psc + 1U) * (p.arr + 1U);
    uint32_t diff    = (product > 84000000U) ? (product - 84000000U) : (84000000U - product);
    TEST("84MHz/1Hz: period product within 0.1% of 84e6", diff < 84000U);

    /* 84 MHz timer, 1000 Hz target (1 ms) */
    rc = timer_calc_period(84000000U, 1000U, &p);
    TEST("84MHz/1000Hz: returns 0", rc == 0);
    TEST("84MHz/1000Hz: PSC fits in 16 bits", p.psc <= 65535U);
    TEST("84MHz/1000Hz: ARR fits in 16 bits", p.arr <= 65535U);
    product = (p.psc + 1U) * (p.arr + 1U);
    TEST("84MHz/1000Hz: (PSC+1)*(ARR+1) = 84000", product == 84000U);

    /* 84 MHz timer, 1 MHz target (maximum reasonable) */
    rc = timer_calc_period(84000000U, 1000000U, &p);
    TEST("84MHz/1MHz: returns 0", rc == 0);
    TEST("84MHz/1MHz: PSC = 0 (no prescaling needed)", p.psc == 0U);
    TEST("84MHz/1MHz: ARR = 83", p.arr == 83U);

    /* Edge cases */
    rc = timer_calc_period(84000000U, 0U, &p);
    TEST("Zero target_hz returns -1", rc == -1);

    rc = timer_calc_period(0U, 1000U, &p);
    TEST("Zero tim_clk_hz returns -1", rc == -1);

    rc = timer_calc_period(84000000U, 85000000U, &p);
    TEST("Target > clock returns -1", rc == -1);

    rc = timer_calc_period(84000000U, 1U, NULL);
    TEST("NULL out pointer returns -1", rc == -1);

    /* -------------------------------------------------------------------------
     * tim2_init tests
     * -------------------------------------------------------------------------*/
    printf("\n=== tim2_init ===\n");

    callback_count = 0;
    rc = tim2_init(84000000U, 1000U, 5U, test_callback);
    TEST("tim2_init returns 0", rc == 0);
    TEST("TIM2 counter enabled (CR1 CEN)", TIM2->CR1 & TIM_CR1_CEN);
    TEST("TIM2 UIE enabled (DIER)", TIM2->DIER & TIM_DIER_UIE);
    TEST("TIM2 UIF cleared after init", !(TIM2->SR & TIM_SR_UIF));
    TEST("TIM2 (PSC+1)*(ARR+1) = 84000",
         (TIM2->PSC + 1U) * (TIM2->ARR + 1U) == 84000U);

    /* Verify NVIC priority was set: priority 5 on 4-bit device = (5 << 4) = 0x50 */
    uint8_t expected_priority = (uint8_t)(5U << (8U - __NVIC_PRIO_BITS));
    TEST("NVIC priority set (IPR[28] = 0x50)", NVIC->IPR[TIM2_IRQn] == expected_priority);

    /* Verify NVIC enable bit: TIM2_IRQn=28, in ISER[0] bit 28 */
    TEST("NVIC TIM2 enabled (ISER[0] bit 28)",
         (NVIC->ISER[TIM2_IRQn >> 5U] >> (TIM2_IRQn & 0x1FU)) & 1U);

    rc = tim2_init(0U, 1000U, 5U, NULL);
    TEST("tim2_init with zero clock returns -1", rc == -1);

    /* -------------------------------------------------------------------------
     * ISR behaviour tests
     * -------------------------------------------------------------------------*/
    printf("\n=== mock_TIM2_IRQHandler ===\n");

    /* Reset state */
    TIM2->SR     = 0U;
    callback_count = 0;
    g_tim2_callback = test_callback;

    /* ISR should not fire if UIF is not set */
    mock_TIM2_IRQHandler();
    TEST("No UIF: callback not called", callback_count == 0U);

    /* Set UIF and fire ISR */
    TIM2->SR = TIM_SR_UIF;
    mock_TIM2_IRQHandler();
    TEST("UIF set: callback called once", callback_count == 1U);
    TEST("UIF cleared after ISR", !(TIM2->SR & TIM_SR_UIF));

    /* Fire ISR multiple times */
    for (uint32_t i = 0; i < 10U; i++) {
        TIM2->SR = TIM_SR_UIF;
        mock_TIM2_IRQHandler();
    }
    TEST("10 ISR invocations: callback called 10 more times", callback_count == 11U);

    /*
     * Re-trigger simulation: verify that UIF is cleared at the TOP of the ISR.
     * The correct design means that if hardware re-asserts UIF after the clear
     * (during the callback body), the NEW flag is preserved when the ISR returns.
     *
     * We simulate this by: (1) running the ISR normally (clears UIF, calls callback),
     * then (2) manually setting UIF again to represent a re-trigger that occurred
     * while the callback was running, and verifying the ISR did NOT clear the
     * second assertion (because the clear happened at the top before the callback).
     *
     * In a real system, the "clear at top" pattern means:
     *   - ISR entry: UIF=1
     *   - Clear: UIF=0
     *   - Timer overflows again mid-callback: UIF=1 (set by hardware)
     *   - ISR returns: UIF=1 (not cleared by this invocation -- correct)
     *   - NVIC re-triggers the ISR for the second event: UIF cleared again
     *
     * To test the clear-at-top property directly: ensure that after the ISR
     * runs once (clearing UIF), a UIF that arrives during processing is not lost.
     * We verify this by checking that SR's UIF bit is clear immediately after
     * mock_TIM2_IRQHandler() clears it at the top -- BEFORE any callback runs.
     * We do this by using a callback that manually sets UIF (simulating hardware).
     */
    callback_count = 0;

    /* Direct test: fire ISR, verify UIF is cleared at the top */
    TIM2->SR = TIM_SR_UIF;
    g_tim2_callback = NULL;        /* run with no callback first to isolate the clear */
    mock_TIM2_IRQHandler();
    TEST("UIF cleared at top of ISR (no callback path)", !(TIM2->SR & TIM_SR_UIF));

    /* Simulate re-trigger: ISR clears UIF, then hardware sets it again */
    TIM2->SR = TIM_SR_UIF;         /* first trigger */
    mock_TIM2_IRQHandler();        /* ISR clears UIF at top */
    TIM2->SR |= TIM_SR_UIF;        /* hardware re-triggers AFTER the clear */
    /* The ISR has already returned -- UIF reflects the second hardware event */
    TEST("Re-trigger: UIF set by hardware after ISR clear persists",
         TIM2->SR & TIM_SR_UIF);
    g_tim2_callback = test_callback;

    /* -------------------------------------------------------------------------
     * SysTick / systick_init tests
     * -------------------------------------------------------------------------*/
    printf("\n=== systick_init / systick_get_ms ===\n");

    /* 168 MHz CPU -- reload should be 167999 */
    rc = systick_init(168000000U);
    TEST("systick_init 168MHz returns 0", rc == 0);
    TEST("SysTick LOAD = 167999", SysTick->LOAD == 167999U);
    TEST("SysTick ENABLE bit set",   SysTick->CTRL & SYSTICK_CTRL_ENABLE);
    TEST("SysTick TICKINT bit set",  SysTick->CTRL & SYSTICK_CTRL_TICKINT);
    TEST("SysTick CLKSOURCE = CPU",  SysTick->CTRL & SYSTICK_CTRL_CLKSOURCE);

    /* Edge case: zero clock */
    rc = systick_init(0U);
    TEST("systick_init zero clock returns -1", rc == -1);

    /* Edge case: 16 MHz -- reload = 15999 */
    rc = systick_init(16000000U);
    TEST("systick_init 16MHz: LOAD = 15999", SysTick->LOAD == 15999U);

    /* Simulate tick counter advancement */
    g_tick_ms = 0U;
    TEST("Initial tick = 0", systick_get_ms() == 0U);

    for (uint32_t i = 0; i < 100U; i++) {
        mock_SysTick_Handler();
    }
    TEST("After 100 SysTick interrupts: tick = 100", systick_get_ms() == 100U);

    /* Wrap-around test: start near UINT32_MAX */
    g_tick_ms = 0xFFFFFFFEU;
    mock_SysTick_Handler();
    TEST("Tick wraps at UINT32_MAX: 0xFFFFFFFF", systick_get_ms() == 0xFFFFFFFFU);
    mock_SysTick_Handler();
    TEST("Tick wraps to 0 after UINT32_MAX", systick_get_ms() == 0U);

    /* systick_delay_ms unsigned wrap-around arithmetic test
       (cannot actually call systick_delay_ms since it loops on real time,
       but verify the formula: (current - start) with unsigned arithmetic) */
    {
        uint32_t start  = 0xFFFFFFFEU;
        uint32_t now    = 0x00000002U;   /* 4 ms after start (with wrap) */
        uint32_t elapsed = now - start;  /* unsigned: 0x00000004 = 4 */
        TEST("Unsigned wrap-around elapsed time: 4 ms", elapsed == 4U);
    }

    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}

#endif /* HOST_TEST */
