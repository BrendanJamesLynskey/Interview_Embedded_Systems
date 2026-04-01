# Quiz: Bare Metal

15 multiple-choice questions covering startup sequences, linker scripts, memory layout, interrupt handling, the `volatile` keyword, and peripheral register access. Questions span three difficulty tiers. Answers with explanations are collected at the end.

---

## Instructions

Select the single best answer for each question. After completing all questions, check your answers against the answer key. For each incorrect answer, read the full explanation before moving on.

Suggested time: 25 minutes.

---

## Questions

### Fundamentals (Q1 -- Q5)

**Q1.** On a Cortex-M microcontroller, when the processor exits reset, the first two words read from the beginning of the vector table are:

- A) The reset handler address and the NMI handler address
- B) The initial stack pointer value and the reset handler address
- C) The initial program counter value and the initial stack pointer value
- D) The application entry point and the size of the `.text` section

---

**Q2.** In a typical ARM Cortex-M linker script, the `.bss` section differs from the `.data` section in that:

- A) `.bss` is stored in flash and copied to RAM at startup; `.data` remains in flash
- B) `.bss` contains uninitialised or zero-initialised variables and occupies no space in the binary image; `.data` contains initialised variables whose initial values are stored in flash
- C) `.bss` holds read-only constants; `.data` holds read-write variables
- D) `.bss` is placed in the heap; `.data` is placed in the stack

---

**Q3.** The primary reason for declaring a hardware peripheral register pointer as `volatile` in C is:

- A) To prevent the register from being placed in CPU cache
- B) To tell the compiler that the variable may change outside the program's normal control flow, preventing the compiler from optimising away reads or writes
- C) To ensure the variable is stored in a 32-bit aligned memory location
- D) To allow the variable to be accessed from both an ISR and the main loop without a race condition

---

**Q4.** A startup file (`startup.s`) for a Cortex-M device typically performs which of the following before calling `main()`?

- A) Sets up the MMU page tables, initialises the UART, and calls `main()`
- B) Sets the initial stack pointer, copies `.data` from flash to RAM, zeroes `.bss`, and calls `main()`
- C) Initialises all peripheral clocks, configures GPIO, and calls `main()`
- D) Loads the application from external flash into RAM and jumps to the entry point

---

**Q5.** When reading a 32-bit memory-mapped peripheral status register in a tight polling loop, what is the most likely symptom if the `volatile` qualifier is omitted from the pointer declaration?

- A) The program crashes immediately with a bus fault
- B) The register is read correctly but the CPU takes longer than expected
- C) The compiler optimises the loop to read the register once and loop on the cached value, so the loop never terminates even when the hardware sets the status bit
- D) The program produces incorrect results only when compiler optimisation is disabled

---

### Intermediate (Q6 -- Q11)

**Q6.** Consider the following linker script fragment:

```
MEMORY {
    FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 512K
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 128K
}
```

A global variable `uint32_t counter = 42;` will have its initial value `42` stored in:

- A) RAM at address 0x20000000
- B) Flash at some address within 0x08000000 -- 0x0807FFFF, in the `.data` load region
- C) Both flash and RAM simultaneously
- D) The `.bss` section, because it is a global variable

---

**Q7.** On a Cortex-M3 device, the NVIC allows a pending interrupt to pre-empt a running ISR if:

- A) The pending interrupt has any priority value other than zero
- B) The pending interrupt has a numerically lower priority number than the currently executing ISR (lower number = higher priority on Cortex-M)
- C) The pending interrupt has a numerically higher priority number than the currently executing ISR
- D) Pre-emption is not possible on Cortex-M3; all ISRs run to completion

---

**Q8.** In a Cortex-M system where both an ISR and the main loop share a 32-bit variable, which of the following is sufficient on its own to prevent a data race without using an RTOS?

- A) Declaring the variable `volatile`
- B) Declaring the variable `static`
- C) Disabling global interrupts around the read-modify-write operation in the main loop
- D) Placing the variable in the `.bss` section

---

**Q9.** A Cortex-M linker script uses the `ALIGN(4)` directive at the end of the `.text` section. The purpose is to:

- A) Ensure the processor can execute instructions correctly, since Thumb-2 requires 4-byte alignment
- B) Ensure the start of the `.data` load region (which immediately follows in flash) is 4-byte aligned, so the startup copy loop can use word-wide transfers without generating unaligned faults
- C) Reserve 4 bytes of padding for debugging metadata
- D) Satisfy a requirement imposed by the GNU `ld` linker for all sections

---

**Q10.** The `__attribute__((naked))` function attribute in GCC, when applied to an interrupt handler, means:

- A) The function is placed in the `.text.isr` section for interrupt-specific optimisation
- B) The compiler emits no prologue or epilogue code (no register push/pop, no stack frame setup), leaving the programmer responsible for saving and restoring context
- C) The function cannot be called from C code, only from assembly
- D) The function is guaranteed to execute at the highest possible interrupt priority

---

**Q11.** What is the purpose of a memory barrier instruction (e.g., `DSB` -- Data Synchronisation Barrier) when configuring a peripheral register on a Cortex-M processor?

- A) It flushes the CPU instruction cache so that the next instruction fetch sees updated flash contents
- B) It ensures that all preceding memory accesses complete before the barrier, preventing the CPU's write buffer from reordering peripheral register writes relative to subsequent accesses
- C) It synchronises the CPU clock with the peripheral bus clock
- D) It is only needed on multi-core systems and has no effect on single-core Cortex-M devices

---

### Advanced (Q12 -- Q15)

**Q12.** A system has 256 KB of flash at 0x08000000 and 64 KB of RAM at 0x20000000. The linker map shows the `.data` section is 20 KB. The startup code copies `.data` using the symbol pair `_sdata`/`_edata` as the destination and `_sidata` as the source. After the copy, the startup code zeroes `.bss` using `_sbss`/`_ebss`. If `_sidata` is set to `0x08020000`, what does that indicate about the flash layout?

- A) The `.text` section ends at 0x08020000 and the `.data` initial values are stored immediately after
- B) The `.data` section overlaps with the `.text` section, which is a linker error
- C) The `.data` initial values are stored at the very beginning of flash
- D) `_sidata` points into RAM, which is incorrect for a Cortex-M startup

---

**Q13.** A bare-metal ISR for a UART receive interrupt is shown below:

```c
volatile uint8_t rx_buffer[64];
volatile uint8_t rx_head = 0;

void UART_IRQHandler(void) {
    rx_buffer[rx_head] = UART->DR;
    rx_head = (rx_head + 1) % 64;
}
```

The main loop reads `rx_head` to determine how many bytes are available. Assuming a single-core Cortex-M3 with no RTOS, which statement is most accurate?

- A) The code is fully correct; `volatile` ensures atomicity of all operations
- B) Reading `rx_head` in the main loop is safe because `uint8_t` reads are atomic on Cortex-M3, but the modulo operation in the ISR is not atomic from the main loop's perspective, which is acceptable since only the ISR writes `rx_head`
- C) The buffer will overflow silently because there is no check against the consumer index
- D) Both A and C are true simultaneously

---

**Q14.** A product uses a Cortex-M4 with an FPU. The RTOS context switch routine saves and restores the core registers but not the FPU registers (S0--S31, FPSCR). In which scenario does this cause a hard-to-reproduce bug?

- A) Whenever any floating-point instruction is executed
- B) Only when two tasks both use floating-point: the FPU state of the pre-empted task is corrupted by the resumed task, producing wrong floating-point results that do not trigger a hardware fault
- C) Whenever the FPU is enabled in the CPACR register
- D) Only when the FPU operates in flush-to-zero mode

---

**Q15.** A linker script places a section called `.ccmram` at VMA 0x10000000 (Core Coupled Memory) with LMA equal to VMA. The startup code does not copy anything from flash to this region. A developer declares:

```c
__attribute__((section(".ccmram"))) uint32_t fast_table[256] = { 1, 2, 3 };
```

What is the most likely runtime behaviour?

- A) The variable is initialised correctly because the linker resolves initial values at link time
- B) The variable contains garbage or zeros at startup because the LMA equals the VMA (no load region in flash), so the initial values are never written into CCM RAM by the startup code and the compiler's init data is lost
- C) The linker produces an error because initialised variables cannot be placed in CCM RAM
- D) The variable is initialised correctly only if the device has a hardware memory controller that copies the LMA region to the VMA region automatically at boot

---

## Answer Key

| Q  | Answer |
|----|--------|
| 1  | B      |
| 2  | B      |
| 3  | B      |
| 4  | B      |
| 5  | C      |
| 6  | B      |
| 7  | B      |
| 8  | C      |
| 9  | B      |
| 10 | B      |
| 11 | B      |
| 12 | A      |
| 13 | B      |
| 14 | B      |
| 15 | B      |

---

## Detailed Explanations

**Q1 -- Answer: B**

The ARM Cortex-M architecture specifies that the vector table begins with the initial Main Stack Pointer (MSP) value at offset 0x00, followed by the reset handler address at offset 0x04. On exiting reset, the hardware loads SP from word 0 and PC from word 1 automatically. Option A is wrong because the NMI vector is at offset 0x08, not offset 0x04. Option C reverses the roles of SP and PC in the startup sequence. Option D confuses the hardware boot mechanism with a software loading scheme.

---

**Q2 -- Answer: B**

The `.data` section holds variables that have non-zero initial values. Their initial values are stored in flash (the load memory address, LMA) and the startup code copies them to RAM (the virtual memory address, VMA) before `main()` runs. The `.bss` section holds zero-initialised or uninitialised globals and statics. Because the initial value of every `.bss` variable is zero, there is no need to store that data in flash -- the startup code simply fills the `.bss` region in RAM with zeros. This is why `.bss` takes no space in the flash binary image. Option A incorrectly describes `.data`, not `.bss`. Option C confuses `.rodata` (read-only constants in flash) with `.bss`. Option D confuses section placement with heap/stack organisation.

---

**Q3 -- Answer: B**

`volatile` is a directive to the compiler, not a hardware mechanism. It tells the compiler not to cache the variable's value in a register across accesses and not to eliminate reads or writes that appear redundant from a purely data-flow perspective. Without it, the compiler may hoist a peripheral register read out of a loop (reading once and reusing the value), or may eliminate a write to a status register because the result is never read in the C source. Option A is wrong because Cortex-M devices do not have a data cache by default (Cortex-M0/M3/M4 have no D-cache in the core, though external cache controllers exist). Option C describes alignment, which is a separate concern handled by `__attribute__((aligned(4)))`. Option D is the key misconception: `volatile` does not provide atomicity or mutual exclusion -- it only prevents compiler optimisation. A race condition between an ISR and the main loop requires a different solution (interrupt disable or atomic operations).

---

**Q4 -- Answer: B**

The startup file is a minimal, hardware-specific piece of code that bridges reset hardware state to a C-runtime environment. Its responsibilities are: (1) optionally set the stack pointer (usually already set by the vector table on Cortex-M), (2) copy the `.data` section's initial values from flash to RAM, (3) zero the `.bss` region, and (4) call `main()`. Peripheral clock and GPIO initialisation are application-level concerns done inside `main()` or a board support package, not in the startup file. Option A (MMU setup) applies to Cortex-A application processors, not Cortex-M. Option D describes a secondary bootloader scenario, not a typical startup file.

---

**Q5 -- Answer: C**

When compiler optimisation is enabled (even `-O1`), the compiler performs data-flow analysis. If the pointer is not `volatile`, the compiler may determine that no C statement in the loop modifies the status register, conclude that the value can never change, and transform the loop into either an infinite loop using a register-cached value or, in the opposite case, eliminate the loop entirely. The symptom is that the polling loop never sees the hardware update. Option A is wrong because omitting `volatile` does not cause a bus fault -- the memory access itself is valid. Option B is incorrect; omitting `volatile` typically makes execution faster (fewer reads), not slower. Option D is the opposite: the bug manifests with optimisation enabled, not disabled.

---

**Q6 -- Answer: B**

A global variable with an explicit initialiser (`= 42`) is placed in the `.data` section. The linker places the initial value (42) in flash (the LMA, load memory address) so it is non-volatile across power cycles. At runtime, the startup code copies this value from flash to the variable's address in RAM (VMA). So the value 42 physically resides in flash before boot. After startup, the live variable lives in RAM and can be modified. Option A is partially correct in that the variable's runtime home is RAM, but the initial value is in flash, which is what the question asks. Option C is conceptually wrong (only one copy exists at any given time). Option D is wrong because `counter = 42` is an initialised variable, so it belongs in `.data`, not `.bss`.

---

**Q7 -- Answer: B**

On Cortex-M, interrupt priority is encoded numerically such that a lower number means higher priority (priority 0 is the highest). Pre-emption occurs when a new interrupt arrives and its priority number is strictly less than the priority number of the currently executing ISR. This is called tail-chaining or pre-emption depending on whether the first ISR has finished. Option C states the opposite (higher number = pre-empts), which is wrong. Option D is wrong; Cortex-M supports nested vectored interrupts (NVIC stands for Nested Vectored Interrupt Controller) and pre-emption is a core feature. Option A is incorrect because any priority value can be used -- what matters is the relative priority of the new interrupt versus the current ISR.

---

**Q8 -- Answer: C**

`volatile` (option A) prevents compiler optimisation but does not provide atomicity. On Cortex-M3 (which lacks the LDREX/STREX exclusive access for 32-bit integers if used without proper sequence, and for multi-byte operations), a read-modify-write is not atomic -- the ISR can fire between the read and the write. The correct bare-metal approach is to disable interrupts globally (using `__disable_irq()`) around the critical section in the main loop, perform the operation, then re-enable interrupts (`__enable_irq()`). This guarantees the ISR cannot interleave. Option B (`static`) only controls linkage/lifetime, not atomicity. Option D (`.bss` placement) is irrelevant to race conditions.

---

**Q9 -- Answer: B**

In a typical linker script for Cortex-M, the `.data` load region immediately follows the `.text` section in flash. The startup copy loop transfers `.data` initial values from flash to RAM, often using 4-byte (word-wide) reads for efficiency. If the source address in flash is not 4-byte aligned, a word read would straddle an alignment boundary. On Cortex-M3/M4, unaligned word accesses are supported but slow; on Cortex-M0, they trigger a HardFault. `ALIGN(4)` at the end of `.text` pads the section to the next 4-byte boundary so that `_sidata` (the start of `.data` in flash) is always aligned. Option A is incorrect because Thumb-2 instructions are 2-byte or 4-byte and the processor handles their fetch alignment internally. Option C is incorrect; `ALIGN` is for address alignment, not metadata padding. Option D is false; `ALIGN` is intentional by the developer, not a mandatory linker requirement.

---

**Q10 -- Answer: B**

A `naked` function has no compiler-generated prologue (push of callee-saved registers, stack frame allocation) or epilogue (pop and return). This is useful for hand-written ISR wrappers in assembly where the context save and restore is done manually, or for thunks where every instruction matters. The downside is that local variables cannot be used safely and calling conventions are the programmer's responsibility. Option A is incorrect; `naked` has nothing to do with section placement -- use `__attribute__((section("...")))` for that. Option C is wrong; a `naked` function is callable from C but the caller and callee must agree on the calling convention entirely in the programmer's assembly. Option D is wrong; priority is controlled by NVIC configuration, not function attributes.

---

**Q11 -- Answer: B**

Cortex-M processors have a write buffer between the CPU and the peripheral bus. Writes can be buffered and complete asynchronously relative to instruction retirement, meaning the processor may execute instructions after a write without that write having actually reached the peripheral register. A `DSB` instruction drains the write buffer: the processor stalls until all outstanding memory accesses (including buffered writes) complete. This is critical, for example, when writing to an NVIC register to pend an interrupt, or when writing to a DMA enable register before triggering a software event. Option A describes `ISB` (Instruction Synchronisation Barrier), which flushes the pipeline and refetches. Option C is not a real function of DSB. Option D is wrong; write buffers and memory ordering issues exist on single-core Cortex-M devices, particularly when accessing device-type memory regions.

---

**Q12 -- Answer: A**

`_sidata` is the Load Memory Address (LMA) of the `.data` section -- the location in flash where the startup code reads the initial values from. If `_sidata = 0x08020000`, this means the `.data` initial values begin at flash address 0x08020000, which is 0x20000 = 128 KB into flash. This tells us that the `.text` section (code) occupies approximately the first 128 KB of flash, ending at or near 0x08020000, and the `.data` load region follows immediately. Option B is wrong; the linker would have reported an error or warning if sections genuinely overlapped. Option C is wrong because 0x08020000 is 128 KB into flash, not the beginning. Option D is wrong because 0x08020000 is in the flash address range, not RAM (RAM starts at 0x20000000).

---

**Q13 -- Answer: B**

`uint8_t` reads and writes are single-byte operations and are inherently atomic on all Cortex-M processors (a byte load/store cannot be interrupted mid-access). Therefore, reading `rx_head` in the main loop will always see either the old value or the new value, never a torn value. The modulo operation in the ISR involves a read-modify-write of `rx_head`, but since only the ISR writes `rx_head`, there is no write conflict with the main loop, which only reads it. The code is correct for single-producer single-consumer use. Option A is incorrect in claiming `volatile` ensures atomicity: it does not. It prevents optimisation, which is valuable here, but atomicity comes from the single-byte nature of the type, not from `volatile`. Option C is a valid observation about missing overflow protection, but the question asks which statement is "most accurate" about the code's correctness -- B identifies the real technical basis for why the code works as written.

---

**Q14 -- Answer: B**

When the Cortex-M4 FPU is enabled, floating-point instructions use the S0--S31 registers and the FPSCR (floating-point status/control register). If the RTOS context switch does not save and restore these registers, a task switch between two floating-point-using tasks will leave stale FPU register values from the pre-empted task in place when the running task resumes. The resumed task then computes wrong floating-point results. This does not trigger any hardware fault because the FPU values are architecturally valid numbers. Option A is wrong; the bug only appears when two FPU-using tasks interleave, not on any single FPU use. Option C is wrong; the bug depends on context switch interleaving, not merely on FPU enablement. Option D is wrong; flush-to-zero mode is an FPSCR configuration detail unrelated to context save/restore. Note: on Cortex-M4, the hardware provides a "lazy stacking" mechanism in the exception entry hardware specifically to avoid saving FPU context when it is not needed -- correct RTOS implementations use this.

---

**Q15 -- Answer: B**

When a variable has an initialiser, the toolchain normally stores the initial values in the `.data` load region in flash and the startup code copies them to RAM. When `LMA == VMA` (as specified in this linker script for `.ccmram`), the toolchain assumes the data is already present at its runtime address -- there is no separate load region in flash. CCM RAM is not initialised by hardware at reset (it is a regular SRAM). The startup code does not copy anything there, and so the initial values never reach the CCM RAM address. The variable will contain whatever was in that SRAM at power-on (typically zero after a cold reset due to SRAM power-on state, but not guaranteed and not the declared initial values). The correct approach is either to give `.ccmram` a separate LMA in flash and add a startup copy loop for it, or to declare the variable with `__attribute__((section(".ccmram")))` without an initialiser and initialise it explicitly in code. Option A is wrong; the linker does not write directly to SRAM at runtime. Option C is wrong; the toolchain does not reject this -- it silently produces incorrect behaviour. Option D is wrong; Cortex-M devices do not have a hardware memory controller that performs LMA-to-VMA copies.
