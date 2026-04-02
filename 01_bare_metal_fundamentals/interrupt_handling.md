# Interrupt Handling

## Prerequisites
- ARM Cortex-M architecture: exception model, privilege levels, execution modes
- C `volatile` keyword and its role in ISR-shared variables
- Basic NVIC register set (ISER, ICER, IPR, ISPR, ICPR)
- Startup and vector table concepts (see `startup_and_boot_sequence.md`)

---

## Concept Reference

### The ARM Cortex-M Exception Model

Cortex-M uses the term "exception" for any event that causes the CPU to suspend the current code and execute a handler. Interrupts (IRQs) are a subset. The full exception numbering:

```
Exception  Number  Type
---------  ------  ----
Reset          1   Fault (highest priority, non-maskable)
NMI            2   Non-Maskable Interrupt
HardFault      3   Fault escalation
MemManage      4   MPU violation
BusFault       5   AHB bus error
UsageFault     6   Undefined instruction, unaligned, divide by zero
Reserved    7-10
SVCall        11   Supervisor call (SVC instruction)
DebugMon      12   Debug monitor
Reserved      13
PendSV        14   Pendable service request (used by RTOS context switch)
SysTick       15   System tick timer
IRQ0          16   First vendor-specific peripheral interrupt
IRQ1          17   ...
...
IRQ239       255   Last possible (device-dependent actual maximum)
```

### NVIC -- Nested Vectored Interrupt Controller

The NVIC is a Cortex-M core peripheral that manages all IRQs (exception numbers >= 16). Key NVIC registers:

```
Register    Address (base 0xE000E000)  Function
--------    -------------------------  --------
ISER[0..7]  0x100 - 0x11C             Interrupt Set Enable: write 1 to enable IRQn
ICER[0..7]  0x180 - 0x19C             Interrupt Clear Enable: write 1 to disable IRQn
ISPR[0..7]  0x200 - 0x21C             Interrupt Set Pending: write 1 to pend IRQn in software
ICPR[0..7]  0x280 - 0x29C             Interrupt Clear Pending: write 1 to clear pending
IABR[0..7]  0x300 - 0x31C             Interrupt Active Bit Register: 1 = handler is running
IPR[0..59]  0x400 - 0x4EC             Interrupt Priority Registers: 8 bits per interrupt
```

Each register array covers 32 IRQs per 32-bit word (total 8 words = 256 IRQs maximum).

To enable IRQ17 (bit 17 within ISER[0]):
```c
NVIC->ISER[0] = (1U << 17);   /* ISER[0] covers IRQ0-IRQ31 */
```

To enable IRQ50 (bit 18 within ISER[1], since 50 - 32 = 18):
```c
NVIC->ISER[1] = (1U << (50 - 32));
```

### Priority Grouping

Cortex-M implements a two-level priority scheme: **group priority** (preemption priority) and **subpriority** (within the same group priority, lower subpriority runs last). The split is controlled by the PRIGROUP field in SCB->AIRCR[10:8].

```
PRIGROUP  |  Preemption bits  |  Subpriority bits
----------+-------------------+------------------
0b011     |  4 bits           |  0 bits (default -- only preemption priority matters)
0b100     |  3 bits           |  1 bit
0b101     |  2 bits           |  2 bits
0b110     |  1 bit            |  3 bits
0b111     |  0 bits           |  4 bits (no preemption, all subpriority)
```

Cortex-M implements 3 to 8 priority bits (device-specific). With 4 priority bits (16 levels, 0 = highest), a PRIGROUP of 0b011 gives 4 bits of preemption and 0 subpriority. Any IRQ with a lower numeric priority value (higher urgency) can preempt any running handler.

**Priority encoding rule (important):** Priority 0 = highest urgency. Priority 255 = lowest urgency. A running handler is preempted only by an exception with a *lower* numeric priority value.

### Tail-Chaining

When a Cortex-M handler finishes executing and another exception is pending, the CPU performs **tail-chaining**: instead of fully restoring the preempted context (unstacking 8 registers) and then immediately saving them again for the new handler, it skips the unstack/stack cycle entirely and fetches the new handler's vector directly.

```
Without tail-chaining:               With tail-chaining:
  IRQ_A handler finishes               IRQ_A handler finishes
  CPU unstacks 8 regs (30 cycles)      CPU detects IRQ_B pending
  CPU checks pending IRQs              CPU skips unstacking
  IRQ_B pending -- restacks 8 regs     CPU fetches IRQ_B vector directly
  IRQ_B handler starts                 IRQ_B handler starts (6 cycles saved)
  Total latency: ~30 + ~12 = 42 cyc   Total latency: ~6 cycles
```

Tail-chaining is automatic hardware behaviour, not software-controlled. It significantly reduces interrupt latency when multiple interrupts are pending simultaneously.

### Late Arrival

If an exception is pending during the stacking phase of a lower-priority exception, the CPU performs **late arrival optimisation**: it abandons the lower-priority handler's vector fetch and instead fetches the higher-priority handler's vector. The stacking work is not wasted -- the stack frame still has the preempted state correctly saved.

### Interrupt Latency

On Cortex-M3/M4/M7, the interrupt latency from IRQ assertion to first instruction of the handler is:

```
Phase               Cycles (zero wait state Flash/SRAM)
-----               -----------------------------------
IRQ recognition     1-2 cycles (synchroniser if asynchronous source)
Context saving      12 cycles (push 8 registers: xPSR, PC, LR, R12, R3-R0)
Vector fetch        1-2 cycles (read handler address from vector table)
Pipeline fill       1-2 cycles (fetch first instruction of handler)
Total               ~12-16 cycles
```

At 168 MHz (STM32F4): 12 cycles / 168e6 ≈ 71 ns minimum latency.

Flash wait states add to vector and instruction fetch latency. The I-cache (Cortex-M7) eliminates most of this overhead after the first access.

---

## Tier 1 -- Fundamentals

### Question F1
**What is the difference between enabling an interrupt at the NVIC and enabling it at the peripheral? Why are both steps required?**

**Answer:**

Interrupt delivery requires two gates to be open simultaneously:

1. **Peripheral interrupt enable:** The peripheral's own control register must assert the IRQ line. For example, a UART will not drive its IRQ line unless the UART_CR1_RXNEIE bit is set in its control register.

2. **NVIC enable:** The NVIC must be configured to forward this IRQ line to the CPU. Writing a 1 to the appropriate bit in NVIC->ISER enables the interrupt at the NVIC level.

```
Peripheral         NVIC               CPU
---------         ----               ---
[UART]            [NVIC]
 RXNE flag set     ISER bit set?
 RXNEIE=1    -->   Yes --> forward --> CPU takes exception
 RXNEIE=0    -->   (IRQ line not asserted, NVIC never sees it)
             -->   ISER bit clear --> IRQ ignored even if asserted
```

**Why two levels?** This separation of concerns allows:
- Masking all interrupts of a type without touching the peripheral (disable at NVIC, peripheral keeps running).
- Disabling a specific peripheral's interrupt without changing NVIC configuration (clear the peripheral IE bit).
- A common pattern in ISR: clear the peripheral flag first, then re-enable -- the two levels prevent accidental re-entry.

**Common mistake:** Enabling the NVIC before setting up the peripheral handler or clearing any spurious pending flags. If the peripheral's IRQ line is already asserted (from a previous run or power-on state), enabling the NVIC will immediately invoke the handler before any initialisation is complete.

**Correct initialisation order:**
```
1. Configure peripheral (baud rate, mode, etc.)
2. Clear any pending interrupt flags in the peripheral
3. Enable interrupt in the peripheral (set IEx bit)
4. Set NVIC priority (NVIC_SetPriority)
5. Enable interrupt in NVIC (NVIC_EnableIRQ)
```

---

### Question F2
**What is interrupt priority on Cortex-M? If two interrupts have the same group priority but different subpriorities, which runs first and can one preempt the other?**

**Answer:**

**Group priority (preemption priority):** Determines whether an interrupt can preempt a currently executing handler. A pending interrupt with a *lower numeric value* group priority will preempt any handler running with a higher numeric value. This is strict preemption.

**Subpriority (within the same group):** When two interrupts with the same group priority are both pending simultaneously, the one with the lower numeric subpriority value is taken first. However, subpriority does *not* enable preemption -- if a lower-subpriority handler is already running, a higher-subpriority interrupt (same group priority) cannot preempt it.

**Example (4 preemption bits, 0 subpriority bits -- PRIGROUP=3):**

```
IRQ_A: priority 0x20 (group 2, sub 0)
IRQ_B: priority 0x30 (group 3, sub 0)

CPU is executing IRQ_B handler. IRQ_A becomes pending.
  Group 2 < Group 3 (lower number = higher urgency)
  Result: IRQ_A preempts IRQ_B. IRQ_B handler is suspended, IRQ_A runs.
  When IRQ_A finishes, IRQ_B resumes.
```

**Example (same group priority):**

```
IRQ_A: group priority 2, subpriority 0
IRQ_B: group priority 2, subpriority 1

Both pending simultaneously (neither handler running):
  IRQ_A has lower subpriority value -> IRQ_A runs first.
  IRQ_B runs second.

If IRQ_B is running and IRQ_A becomes pending:
  Same group priority -> NO PREEMPTION.
  IRQ_A waits until IRQ_B handler returns.
```

**What would you do if...** an interrupt handler was taking too long and causing other time-critical interrupts to be delayed? First, check that the time-critical interrupt has a higher (lower numeric value) group priority than the slow handler. If they have the same group priority, subpriority alone will not save you -- preemption requires different group priorities. Reduce the slow handler's time, or split it into top/bottom halves using PendSV or a task flag.

---

### Question F3
**What is tail-chaining and why does it matter for systems with many peripheral interrupts?**

**Answer:**

Tail-chaining is a Cortex-M hardware optimisation that eliminates the unstack/restack overhead between two consecutive exception handlers.

**Without tail-chaining (hypothetical naive hardware):**

```
1. IRQ_A fires         -- CPU stacks 8 registers (~12 cycles)
2. IRQ_A handler runs  -- clears flag, does work, returns
3. CPU unstacks        -- 12 cycles (pops saved context)
4. CPU checks pending  -- finds IRQ_B pending
5. CPU stacks again    -- 12 cycles
6. IRQ_B handler runs
Total overhead: ~24 cycles between handlers
```

**With tail-chaining (actual Cortex-M):**

```
1. IRQ_A fires         -- CPU stacks 8 registers (~12 cycles)
2. IRQ_A handler runs  -- clears flag, does work, returns
3. CPU detects IRQ_B pending (before completing the unstack)
4. CPU skips the unstack, fetches IRQ_B vector directly
5. IRQ_B handler starts
Total overhead between handlers: ~6 cycles (vector fetch + pipeline fill only)
```

**Why it matters:**

In systems with frequent back-to-back interrupts (ADC multi-channel, UART with multiple status conditions, DMA with multiple channels completing close together), tail-chaining reduces CPU overhead from 24+ cycles to 6 cycles per consecutive interrupt. At 168 MHz, saving 18 cycles per handler transition is 107 ns -- not large in isolation, but significant when 1000 interrupts per second each waste 18 cycles.

**Tail-chaining vs. late arrival:** Tail-chaining applies when the first handler has fully completed before the CPU notices the second interrupt is pending (during the EXC_RETURN sequence). Late arrival applies when the second (higher-priority) interrupt becomes pending during the stacking phase of the first interrupt -- the CPU abandons fetching the first handler and switches to the higher-priority one instead.

---

### Question F4
**What does the CPU automatically save on the stack when an interrupt is taken? Why these specific registers?**

**Answer:**

When an exception is taken, the Cortex-M hardware automatically pushes (stacks) exactly 8 registers onto the currently active stack (MSP or PSP):

```
Stack frame layout (top = lowest address, assuming descending stack):
+-------------+   <- SP after stacking (exception entry)
| xPSR        |   Saved processor status (condition flags, IT state, THUMB bit)
| PC          |   Return address (instruction AFTER the interrupted instruction)
| LR          |   Caller's return address (link register)
| R12         |   Scratch register (inter-procedural scratch per AAPCS)
| R3          |   Argument/scratch register
| R2          |   Argument/scratch register
| R1          |   Argument/scratch register
| R0          |   Argument/scratch register
+-------------+   <- SP before stacking (pre-exception stack pointer)
```

**Why these 8 registers specifically?**

The ARM Procedure Call Standard (AAPCS) defines R0-R3 and R12 as **caller-saved** (scratch) registers. A function that gets preempted expects the caller to have saved these if needed. By having hardware save exactly the AAPCS caller-saved set, the ISR written in C can use R0-R3 and R12 freely without explicitly saving them -- they are already on the stack. The hardware effectively makes the ISR look like a normal function call.

The hardware-saved frame allows an ISR written entirely in C to execute without any assembly prologue or register-save code.

**Registers NOT automatically saved (callee-saved per AAPCS):** R4-R11. If an ISR uses these registers, the compiler-generated prologue saves and restores them. If an ISR uses the FPU (S0-S15, FPSCR), the CPU lazily extends the stack frame to save the floating-point context (lazy stacking).

---

## Tier 2 -- Intermediate

### Question I1
**Describe the concept of priority inversion in the context of bare-metal interrupt handling. How can an ISR inadvertently starve a higher-priority task?**

**Answer:**

Priority inversion in interrupt context occurs when a high-priority ISR is blocked waiting for a condition that can only be satisfied by lower-priority code that cannot run.

**Classic scenario -- ISR spinning on a shared resource:**

```c
/* BAD: shared circular buffer, protected by a disable/enable scheme */
volatile uint8_t uart_buf[256];
volatile uint8_t head = 0, tail = 0;

/* Low-priority task fills the buffer */
void task_fill_buffer(void)
{
    while (1) {
        uint8_t data = get_next_byte_from_sensor();
        __disable_irq();               /* critical section */
        uart_buf[head] = data;
        head = (head + 1) & 0xFF;
        __enable_irq();
        delay_ms(1);
    }
}

/* UART TX interrupt -- high priority */
void USART1_IRQHandler(void)
{
    while (head == tail) { }           /* SPIN WAITING -- TERRIBLE */
    USART1->DR = uart_buf[tail];
    tail = (tail + 1) & 0xFF;
}
```

If the ISR spins waiting for data and the task that produces data runs at a lower effective priority (or is preempted by an even higher-priority ISR), the ISR can loop forever. Meanwhile, the lower-priority task cannot run to supply data because the ISR is consuming 100% of CPU.

**Correct approach: make the ISR non-blocking, flag a condition, handle in task:**

```c
/* GOOD: ISR is minimal, checks flag, clears and returns quickly */
void USART1_IRQHandler(void)
{
    if (head == tail) {
        /* Buffer empty -- disable TX interrupt until data is available */
        USART1->CR1 &= ~USART_CR1_TXEIE;
        return;
    }
    USART1->DR = uart_buf[tail];
    tail = (tail + 1) & 0xFF;
}
```

The ISR never spins. If the buffer is empty, it disables itself. The producer re-enables the TX interrupt after adding data.

---

### Question I2
**A developer uses `__disable_irq()` / `__enable_irq()` to protect a shared variable between main code and an ISR. Describe three scenarios where this approach fails or is insufficient.**

**Answer:**

**Scenario 1: Nested interrupt disables**

```c
void function_a(void)
{
    __disable_irq();
    function_b();        /* also calls __disable_irq() / __enable_irq() */
    __enable_irq();      /* re-enables IRQs even though function_a still needs them masked */
}

void function_b(void)
{
    __disable_irq();
    /* shared data access */
    __enable_irq();      /* BUG: re-enables IRQs while function_a's critical section
                                still expects them disabled */
}
```

**Fix:** Use a save/restore pattern:
```c
uint32_t saved = __get_PRIMASK();
__disable_irq();
/* critical section */
__set_PRIMASK(saved);   /* restore previous state, not unconditionally enable */
```

**Scenario 2: The ISR itself has multiple priority levels**

`__disable_irq()` sets PRIMASK, which blocks all configurable-priority exceptions (priority levels > -1). It does not block:
- NMI (priority -2)
- HardFault (priority -1)

If data is shared with an NMI handler, `__disable_irq()` provides no protection. The NMI can still preempt and corrupt the shared state.

**Fix:** Use `__set_FAULTMASK(1)` to also block HardFault (use with extreme caution), or redesign the NMI handler to not share mutable state with normal code.

**Scenario 3: Long critical sections mask important timing-sensitive interrupts**

```c
void process_large_buffer(uint8_t *buf, size_t len)
{
    __disable_irq();
    for (size_t i = 0; i < len; i++) {
        buf[i] = transform(buf[i]);   /* BUG: 10 ms with IRQs disabled */
    }
    __enable_irq();
}
```

While IRQs are disabled, a UART with a 1-byte FIFO will lose received bytes (no interrupt to drain the FIFO), a system tick will be missed (affecting RTOS timing), and a motor control PWM interrupt may miss its update (causing a torque glitch).

**Fix:** Protect only the minimum necessary access -- copy the shared pointer/index, then operate outside the critical section. Or redesign to use double-buffering.

---

### Question I3
**Explain what happens when a Cortex-M hard fault occurs during an interrupt handler. What is exception escalation and how does the HFSR register help diagnose it?**

**Answer:**

**Exception escalation** occurs when a fault exception fires but cannot be taken because:
1. The faulting code is already running at the same or higher priority.
2. The fault handler itself generates a fault.

When escalation occurs, the CPU promotes the fault to a HardFault, which has a fixed priority of -1 (always higher than any configurable exception).

**Scenario -- BusFault during a BusFault handler:**

```
1. IRQ handler writes to an invalid address
2. BusFault fires, but BusFault handler is already active (or same priority)
3. BusFault cannot preempt itself
4. CPU escalates to HardFault
5. HardFault handler runs
```

**The HFSR (HardFault Status Register at 0xE000ED2C):**

```
Bit 31  DEBUGEVT   -- Reserved for debug events
Bit 30  FORCED     -- 1 if this HardFault is an escalated (forced) fault
                      Check CFSR to find the original fault cause
Bit 1   VECTBL     -- 1 if vector table read faulted on exception entry
                      (vector table at wrong address or Flash not programmed)
```

**Diagnosing with HFSR and CFSR:**

```c
void HardFault_Handler(void)
{
    volatile uint32_t hfsr  = SCB->HFSR;   /* HardFault Status Register */
    volatile uint32_t cfsr  = SCB->CFSR;   /* Configurable Fault Status Register */
    volatile uint32_t mmfar = SCB->MMFAR;  /* MemManage Fault Address Register */
    volatile uint32_t bfar  = SCB->BFAR;   /* BusFault Address Register */

    /* If FORCED bit is set, the real cause is in CFSR */
    if (hfsr & SCB_HFSR_FORCED_Msk) {
        /* Check MMFSR (bits 7:0 of CFSR): MPU violation */
        /* Check BFSR (bits 15:8 of CFSR): bus fault */
        /* Check UFSR (bits 31:16 of CFSR): usage fault */
    }
    /* If VECTBL bit is set: exception entry vector table read failed */

    __BKPT(0);  /* halt debugger here */
    while (1) {}
}
```

**CFSR key bits:**

```
CFSR [7:0]  MMFSR (MemManage Fault):
  bit 7  MMARVALID -- MMFAR holds the faulting address
  bit 1  DACCVIOL  -- data access violation
  bit 0  IACCVIOL  -- instruction access violation (execute from no-exec region)

CFSR [15:8] BFSR (BusFault):
  bit 15 BFARVALID -- BFAR holds the faulting address
  bit 3  UNSTKERR  -- fault during exception unstacking
  bit 2  STKERR    -- fault during exception stacking (SP invalid!)
  bit 1  PRECISERR -- data bus error, address in BFAR

CFSR [31:16] UFSR (UsageFault):
  bit 24 DIVBYZERO -- divide by zero (if SDIV/UDIV used and enabled in CCR)
  bit 25 UNALIGNED -- unaligned access (if enabled in CCR)
  bit 16 UNDEFINSTR -- undefined instruction executed
```

---

### Question I4
**What is the EXC_RETURN value and how does the CPU use it to restore context after an interrupt handler?**

**Answer:**

When an exception is taken, the CPU writes a special value into the Link Register (LR) called `EXC_RETURN`. This value encodes the CPU state that should be restored when the handler executes a `BX LR` (or equivalent return instruction). The CPU recognises an `EXC_RETURN` value by its upper 28 bits all being 1 (0xFFFFFFF0 base), which cannot be a valid code address.

```
EXC_RETURN bit field (Cortex-M3/M4/M7):

  Bits [31:5]  -- 0xFFFFFFF (all ones, constant)
  Bit  [4]     -- Stack frame type:
                    1 = basic frame (8 regs, no FP state)
                    0 = extended frame (26 regs, includes FP state)
  Bit  [3]     -- Return mode:
                    1 = return to Thread mode
                    0 = return to Handler mode (nested interrupt)
  Bit  [2]     -- Stack pointer selector on return:
                    1 = use PSP (Process Stack Pointer)
                    0 = use MSP (Main Stack Pointer)
  Bit  [1]     -- Reserved (must be 1)
  Bit  [0]     -- Reserved (must be 1)
```

**Common EXC_RETURN values:**

| Value      | Meaning |
|------------|---------|
| 0xFFFFFFF9 | Return to Thread mode, use MSP, basic frame |
| 0xFFFFFFFD | Return to Thread mode, use PSP, basic frame |
| 0xFFFFFFF1 | Return to Handler mode, use MSP, basic frame |
| 0xFFFFFFE9 | Return to Thread mode, use MSP, extended (FP) frame |
| 0xFFFFFFED | Return to Thread mode, use PSP, extended (FP) frame |

**How an RTOS uses EXC_RETURN:**

FreeRTOS context switching via PendSV works by manipulating EXC_RETURN. The PendSV handler:
1. Pushes the remaining callee-saved registers (R4-R11) manually onto the PSP.
2. Saves the current task's PSP.
3. Loads the next task's PSP.
4. Pops the next task's R4-R11 from its PSP.
5. Returns with `EXC_RETURN = 0xFFFFFFFD` to restore the next task's PC, LR, and R0-R3 from its saved stack frame.

The CPU then unstacks the hardware-saved frame from the new task's PSP, completing the context switch.

---

## Tier 3 -- Advanced

### Question A1
**Design an interrupt-driven UART receive system with a ring buffer that correctly handles concurrent access between the ISR and main code. Identify all the race conditions and explain how each is mitigated without using an RTOS.**

**Answer:**

**The race conditions in a naive ring buffer:**

```
Shared state: head (written by ISR), tail (read by ISR, written by main)

Race 1: Main reads `head` to check if data is available.
        ISR fires between the read of `head` and the comparison, advancing `head`.
        Main uses a stale `head` value and concludes no data is available.
        Consequence: One byte of data missed until next check.

Race 2: Main reads a byte: loads buf[tail], then increments tail.
        ISR fires between the load and the increment.
        ISR writes new data to buf[head]. head != tail so no overrun detected.
        Main's stale tail is then incremented, advancing past the ISR-written byte.
        Consequence: One byte skipped in the stream.

Race 3: Buffer full check. ISR checks (head+1)%SIZE == tail before deciding to drop.
        Main increments tail between the ISR's check and its write.
        ISR could have written -- instead it dropped a byte unnecessarily.
```

**Safe ring buffer implementation:**

```c
#define RX_BUF_SIZE 256U   /* must be a power of 2 for efficient masking */

typedef struct {
    volatile uint8_t  buf[RX_BUF_SIZE];
    volatile uint32_t head;   /* written only by ISR */
    volatile uint32_t tail;   /* written only by main, read by ISR */
} ring_buf_t;

static ring_buf_t rx_buf;

/* Called ONLY from ISR */
static inline void ring_buf_push(ring_buf_t *rb, uint8_t byte)
{
    uint32_t next_head = (rb->head + 1U) & (RX_BUF_SIZE - 1U);

    /* Read tail once -- it is volatile so this is a single load */
    if (next_head != rb->tail) {
        rb->buf[rb->head] = byte;
        /* Write barrier: ensure buf write is visible before head advances */
        __DMB();
        rb->head = next_head;
    }
    /* else: buffer full -- drop byte (or set an overflow flag) */
}

/* Called ONLY from main/tasks (not ISR) */
static inline int ring_buf_pop(ring_buf_t *rb, uint8_t *out)
{
    uint32_t head;

    /* Read head once -- prevents TOCTOU if ISR advances head between two reads */
    __DMB();                            /* read barrier: see ISR's write to head */
    head = rb->head;

    if (rb->tail == head) {
        return 0;                       /* empty */
    }

    *out = rb->buf[rb->tail];
    __DMB();                            /* read barrier: ensure buf read before tail advances */
    rb->tail = (rb->tail + 1U) & (RX_BUF_SIZE - 1U);
    return 1;
}

void USART1_IRQHandler(void)
{
    if (USART1->SR & USART_SR_RXNE) {
        uint8_t byte = (uint8_t)(USART1->DR & 0xFFU);
        ring_buf_push(&rx_buf, byte);
    }
    /* Clear overrun error if set -- otherwise ISR fires repeatedly */
    if (USART1->SR & USART_SR_ORE) {
        (void)USART1->DR;               /* read DR to clear ORE */
    }
}
```

**Why this is safe without locking:**

1. `head` is written only by the ISR. Main reads it as a snapshot. The ISR never runs concurrently with itself on a single-core Cortex-M.
2. `tail` is written only by main. The ISR reads it once. The compiler cannot merge or reorder the volatile read.
3. `volatile` prevents the compiler from caching head/tail in a register across the critical check.
4. `__DMB()` (data memory barrier) prevents the CPU from reordering the buffer write before the head/tail advance, ensuring coherent visibility.

**Remaining limitation:** The buffer size must be known at compile time. A fixed 256-byte buffer may overflow if main does not drain it promptly. Add an overflow counter to monitor this.

---

### Question A2
**On a Cortex-M4 running FreeRTOS, a developer calls `NVIC_SetPriority(IRQ_A, 5)` and then from within IRQ_A's handler calls `xQueueSendFromISR()`. This causes an immediate crash. Explain why and how to fix it.**

**Answer:**

**Root cause: FreeRTOS priority threshold (configMAX_SYSCALL_INTERRUPT_PRIORITY)**

FreeRTOS uses `taskENTER_CRITICAL()` (which raises BASEPRI) to protect its internal data structures. `xQueueSendFromISR()` and all `*FromISR()` functions internally call `taskENTER_CRITICAL_FROM_ISR()`, which uses BASEPRI.

FreeRTOS defines `configMAX_SYSCALL_INTERRUPT_PRIORITY` (e.g., 5 in priority bits, mapped to the 8-bit IPR register value, e.g., 0x50 for a 4-bit priority device). Any ISR that calls a FreeRTOS API **must** have a numeric priority value **greater than or equal to** `configMAX_SYSCALL_INTERRUPT_PRIORITY` (i.e., equal or lower urgency).

**The crash scenario:**

```
configMAX_SYSCALL_INTERRUPT_PRIORITY = 5 (0x50 in 8-bit IPR)
IRQ_A priority set by NVIC_SetPriority(IRQ_A, 5):
  On a 4-bit priority Cortex-M4, priorities are stored in bits[7:4] of the 8-bit field.
  CMSIS NVIC_SetPriority() stores the value in the upper bits of the 8-bit IPR field:
  IPR register = (priority << (8 - __NVIC_PRIO_BITS))
  If __NVIC_PRIO_BITS = 4: IPR = (5 << 4) = 0x50.  <-- correct

  configMAX_SYSCALL_INTERRUPT_PRIORITY is typically defined as the RAW 8-bit register
  value (e.g., 0x50), not the logical priority number.

The crash occurs if:
  priority passed to NVIC_SetPriority = 5 (valid for 4-bit = logical level 5 of 0-15)
  BUT developer intends "high priority" (numerically low = urgent)
  AND configMAX_SYSCALL_INTERRUPT_PRIORITY = 5 means "level 5 (0x50) or lower urgency"
  
  If the ISR is set to priority 0 or 1 (more urgent than the FreeRTOS syscall threshold),
  xQueueSendFromISR() calls taskENTER_CRITICAL_FROM_ISR() which raises BASEPRI to 0x50.
  But BASEPRI only masks priorities EQUAL TO OR LESS URGENT than the BASEPRI value.
  An ISR running at priority 0 cannot be masked by BASEPRI=0x50.
  
  The ISR at priority 0 is therefore able to interrupt FreeRTOS's own critical section,
  corrupt the queue's internal state, and cause a crash or assertion failure.
```

**The fix:**

Ensure all ISRs that call FreeRTOS API have a priority value (in raw register terms) that is >= `configMAX_SYSCALL_INTERRUPT_PRIORITY` (i.e., they are less urgent than the threshold):

```c
/* FreeRTOSConfig.h */
#define configMAX_SYSCALL_INTERRUPT_PRIORITY  5  /* logical priority on 4-bit device */

/* Correct setup: IRQ_A is allowed to call FreeRTOS API */
NVIC_SetPriority(IRQ_A_IRQn, 5);   /* priority 5 >= threshold 5: SAFE */
NVIC_SetPriority(IRQ_A_IRQn, 10);  /* priority 10 >= threshold 5: SAFE */

/* WRONG: priority more urgent than threshold */
NVIC_SetPriority(IRQ_A_IRQn, 1);   /* priority 1 < threshold 5: CRASH */
```

**Mnemonic:** "If your ISR calls FreeRTOS, its priority must be lower urgency than (or equal to) the syscall threshold. Urgency = inverse of numeric priority value. High urgency ISRs (low numbers) cannot call FreeRTOS API."

---

### Question A3
**Describe the steps required to safely implement a "top half / bottom half" interrupt processing pattern on bare-metal Cortex-M, similar to the Linux softirq mechanism. Why is this pattern sometimes necessary?**

**Answer:**

**Why the pattern is necessary:**

ISRs must be short. Long ISRs:
- Block lower-priority interrupts for extended periods.
- Increase interrupt jitter for higher-priority interrupts.
- Risk missing edge-triggered signals (if the source fires twice while the ISR is running).

Some interrupt-driven work is inherently time-consuming: parsing a received packet, writing to an SD card, running a PID algorithm. The top/bottom-half pattern defers the heavy work to lower-priority context.

**Implementation on bare-metal Cortex-M using PendSV:**

PendSV (priority 14 in exception number, usually configured as the lowest configurable priority) is designed for deferred work. It can be pended by software from any context.

```c
/* Step 1: Configure PendSV as the lowest priority exception */
NVIC_SetPriority(PendSV_IRQn, 0xFF);   /* lowest priority: 255 */

/* Step 2: Define a bitmask of pending bottom-half jobs */
volatile uint32_t pending_work;       /* bits set by ISR top halves */

#define WORK_UART_RX    (1U << 0)
#define WORK_SPI_RX     (1U << 1)
#define WORK_TIMER_TICK (1U << 2)

/* Step 3: In each ISR (top half) -- keep it minimal */
void USART1_IRQHandler(void)
{
    /* Copy received byte into ring buffer (fast) */
    rx_buf.buf[rx_buf.head] = USART1->DR;
    rx_buf.head = (rx_buf.head + 1U) & (RX_BUF_SIZE - 1U);

    /* Flag the bottom half */
    pending_work |= WORK_UART_RX;

    /* Pend PendSV to run the bottom half */
    SCB->ICSR = SCB_ICSR_PENDSVSET_Msk;
}

/* Step 4: PendSV bottom half -- runs at lowest priority */
void PendSV_Handler(void)
{
    /* Atomically read and clear the pending work flags */
    uint32_t work;
    __disable_irq();
    work = pending_work;
    pending_work = 0;
    __enable_irq();

    if (work & WORK_UART_RX) {
        process_uart_rx_buffer();   /* heavyweight: parse packets, etc. */
    }
    if (work & WORK_SPI_RX) {
        process_spi_rx_data();
    }
    if (work & WORK_TIMER_TICK) {
        run_pid_controller();
    }
}
```

**Why PendSV is the right choice:**

1. PendSV is "sticky": if PendSV is pended while it is already pending (i.e., multiple ISRs fire before PendSV runs), it only runs once. The bottom half processes all accumulated work in a single pass.
2. PendSV at the lowest priority runs only when no other exception is pending -- it never interrupts any real-time work.
3. The `SCB->ICSR = SCB_ICSR_PENDSVSET_Msk` operation is atomic from any context including ISR.

**Limitations of this bare-metal approach vs RTOS:**
- The PendSV handler cannot block (sleep) waiting for a resource -- it is still an exception handler with the restrictions that implies.
- All bottom-half work shares a single handler; individual work items cannot have different priorities.
- Stack depth must accommodate the deepest bottom-half function call chain within the PendSV handler's allocated stack.

For more complex deferral requirements, an RTOS with task notification or queue is the appropriate abstraction.
