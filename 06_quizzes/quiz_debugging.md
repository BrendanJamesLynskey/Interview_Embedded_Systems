# Quiz: Debugging and Testing

15 multiple-choice questions covering JTAG/SWD debug interfaces, logic analysers and oscilloscopes, memory corruption diagnosis, and unit testing in embedded systems. Questions span three difficulty tiers. Answers with explanations are collected at the end.

---

## Instructions

Select the single best answer for each question. After completing all questions, check your answers against the answer key. For each incorrect answer, read the full explanation before moving on.

Suggested time: 25 minutes.

---

## Questions

### Fundamentals (Q1 -- Q5)

**Q1.** The primary advantage of SWD (Serial Wire Debug) over JTAG for debugging an ARM Cortex-M microcontroller is:

- A) SWD operates at a higher clock frequency than JTAG
- B) SWD requires only two signal pins (SWDIO and SWDCLK) instead of JTAG's minimum of four (TCK, TMS, TDI, TDO), saving GPIO pins
- C) SWD supports debugging multiple cores simultaneously, while JTAG supports only one
- D) SWD provides access to the FPU registers, while JTAG does not

---

**Q2.** A logic analyser is the preferred tool over an oscilloscope for:

- A) Measuring the rise time of a digital signal
- B) Measuring the power supply noise on a 3.3 V rail during a burst transmission
- C) Capturing and decoding multi-channel digital protocol traffic (such as SPI, I2C, or UART) over many thousands of samples with automated protocol decoding
- D) Measuring the differential voltage on a CAN bus during a dominant bit

---

**Q3.** When a Cortex-M processor encounters an access to a null pointer (address 0x00000000 reading from flash on most devices, but writing to address 0 in some configurations), the fault type generated is typically:

- A) UsageFault, because null pointer dereference is a software error
- B) HardFault or MemManage fault, depending on whether the MPU is configured to protect the null page
- C) BusFault always, because address 0 is never a valid bus address
- D) No fault is generated; the processor silently reads the reset vector

---

**Q4.** Stack overflow in an embedded system without an MPU most commonly manifests as:

- A) An immediate HardFault at the exact instruction that overflows the stack
- B) Silent data corruption of variables adjacent to the stack in memory, leading to intermittent and hard-to-reproduce failures that may not appear until much later
- C) A compile-time error when the stack usage exceeds the allocated size
- D) An assertion failure in the C runtime library

---

**Q5.** The purpose of a watchdog timer (WDT) in an embedded system is:

- A) To measure the execution time of critical code sections for profiling
- B) To reset the processor if the software fails to periodically refresh (kick) the watchdog within a configured timeout, recovering from deadlocks, infinite loops, or hard faults
- C) To generate precise time delays without using a software delay loop
- D) To monitor external power supply voltage and reset the processor on undervoltage

---

### Intermediate (Q6 -- Q11)

**Q6.** A developer sets a hardware data watchpoint in a JTAG/SWD debugger on a global variable. When the watchpoint fires, it indicates:

- A) The instruction pointer has reached a specific address in code
- B) The target variable has been read from or written to (depending on watchpoint type), halting the CPU so the developer can inspect the call stack and registers at the exact moment of the access
- C) The variable's value has changed by more than a configured threshold
- D) A stack overflow has overwritten the variable's memory location

---

**Q7.** A firmware image is loaded into flash and runs correctly when the debugger is connected but crashes immediately after power-cycle without the debugger. The most likely cause is:

- A) The debugger is providing power to the target and the external power supply is faulty
- B) The startup code is relying on initial RAM state from a previous debug session (a previous download that left `.data` already in RAM), and without the debugger the startup copy from flash has a bug (wrong `_sidata` symbol, incorrect loop bounds, or mismatched section sizes)
- C) The CPU clock speed is too high and only works when the debugger slows it down via JTAG
- D) The `.bss` section is not being zeroed and happens to contain non-zero garbage from the factory test

---

**Q8.** When using a logic analyser to debug an I2C communication issue, a developer observes that the ACK bit from the slave is high (recessive) instead of low (dominant) after the address byte. This indicates:

- A) The I2C bus speed is too high for the slave device
- B) The slave is not responding; possible causes include the slave address being wrong, the slave not being powered, the slave not being present, or the slave being busy with a long internal operation
- C) The master has incorrectly sent a write command when a read was intended
- D) The SDA and SCL lines are swapped on the connector

---

**Q9.** Memory-mapped peripheral registers on Cortex-M are typically placed in a memory region that is configured as:

- A) Normal, Cacheable memory, to allow the CPU to cache register reads for performance
- B) Device or Strongly-Ordered memory, which prevents speculative reads, disables caching, and enforces strict ordering of accesses relative to other memory operations
- C) Normal, Write-Through Cached memory, so writes are immediately visible to the peripheral
- D) Execute-Never (XN) Cached memory, to protect against code execution from peripheral space while allowing cached reads

---

**Q10.** A developer uses `assert()` checks throughout their embedded firmware. In production builds (`NDEBUG` defined), what happens to assertions?

- A) Assertions are compiled as logging statements that write to a UART
- B) The preprocessor removes all `assert()` calls, so they have zero code size and zero runtime overhead in production
- C) Assertions remain but the failure handler is replaced with a no-op
- D) Assertions are replaced with inline error-return checks

---

**Q11.** A unit test for an embedded driver calls a function that directly reads a hardware register via a memory-mapped pointer. The test fails on the host machine with a segmentation fault. The most appropriate fix is:

- A) Run the unit test directly on the target hardware to avoid host machine limitations
- B) Abstract the hardware register access behind a function pointer or interface, then inject a mock/stub that returns configurable test values; the test then runs on the host without requiring physical hardware
- C) Use a memory-mapped file on the host to simulate the register at the same address as on the target
- D) Increase the stack size on the host test runner to accommodate the embedded driver's memory requirements

---

### Advanced (Q12 -- Q15)

**Q12.** A Cortex-M4 system exhibits random HardFaults that occur once every few hours under load. The HardFault handler logs the stacked PC (program counter) value from the exception frame, but the PC points to a different function each time. The HFSR (HardFault Status Register) shows FORCED=1 and the BFSR (BusFault Status Register) shows IBUSERR=1 (instruction bus error). What is the most likely root cause?

- A) A null pointer dereference in the function currently executing
- B) A corrupted function pointer or return address on the stack, causing the CPU to jump to an invalid address and attempt to fetch an instruction from unmapped memory
- C) Integer overflow in arithmetic code within the faulting function
- D) An unaligned word access to a SIMD instruction that is not supported on Cortex-M4

---

**Q13.** A developer is debugging a production firmware image (optimised, no debug symbols) on a target that has crashed. The only information available is the stack contents captured from the SRAM via JTAG. To reconstruct the call chain, the developer:

- A) Cannot reconstruct the call chain without debug symbols; the investigation must start over with a debug build
- B) Examines the stack for word-aligned values that fall within the address range of the flash image (indicating potential return addresses), then uses the linker map file and disassembly of the firmware binary to identify which functions these addresses fall within, reconstructing an approximate call chain
- C) Reads the CPU's internal call stack buffer, which records the last 16 function calls in hardware
- D) Restarts the target with the debugger attached and waits for the crash to recur

---

**Q14.** A CAN bus node is sending frames but other nodes are not receiving them. The developer connects a logic analyser and sees valid CAN frames on the CANTX pin of the microcontroller, but the bus (CANH/CANL) shows no activity. The most likely cause is:

- A) The CAN baud rate is configured differently between the transmitting node and the receiving nodes
- B) The CAN transceiver is disabled, unpowered, or in standby mode, so it is not driving the physical bus even though the microcontroller's CAN peripheral is generating correct digital frames on its TX pin
- C) The CAN message identifier has higher priority than the other nodes, causing permanent arbitration loss
- D) The receiving nodes' acceptance filters are blocking all incoming frames

---

**Q15.** A developer is writing a unit test for a FreeRTOS-based driver on the host machine. The driver calls `xQueueSend()` and `xQueueReceive()`. To avoid running FreeRTOS on the host, the correct approach is:

- A) Port FreeRTOS to the host operating system and run the full RTOS kernel during testing
- B) Replace FreeRTOS API calls with thin wrappers or fakes that simulate the queue behaviour (FIFO with configurable depth) without using FreeRTOS internals, allowing the driver logic to be tested in isolation on the host
- C) Use `#ifdef` guards to remove all FreeRTOS calls in test builds, then verify only the non-RTOS portions of the driver
- D) Use a hardware-in-the-loop (HIL) test setup where the host machine sends test vectors to the target over JTAG

---

## Answer Key

| Q  | Answer |
|----|--------|
| 1  | B      |
| 2  | C      |
| 3  | B      |
| 4  | B      |
| 5  | B      |
| 6  | B      |
| 7  | B      |
| 8  | B      |
| 9  | B      |
| 10 | B      |
| 11 | B      |
| 12 | B      |
| 13 | B      |
| 14 | B      |
| 15 | B      |

---

## Detailed Explanations

**Q1 -- Answer: B**

SWD (Serial Wire Debug) is a 2-pin debug interface developed by ARM as an alternative to JTAG for Cortex-M devices. JTAG requires at minimum TCK (clock), TMS (mode select), TDI (data in), TDO (data out) -- four pins -- plus optional TRST. SWD uses only SWDIO (bidirectional data) and SWDCLK. In small packages and cost-sensitive designs, saving two GPIO pins is significant. Both SWD and JTAG provide equivalent debugger capabilities on Cortex-M: instruction stepping, breakpoints, register and memory access, and data watchpoints. Option A is not generally true; both protocols support similar maximum frequencies. Option C is wrong; JTAG's daisy-chain topology is specifically designed for multi-core access. Option D is wrong; both interfaces provide identical register access capabilities.

---

**Q2 -- Answer: C**

A logic analyser digitises many channels simultaneously (8, 16, 32, or more) at high sample rates with deep memory, captures hundreds of thousands of samples, and can decode serial protocols (SPI, I2C, UART, CAN) automatically, displaying decoded values alongside the raw waveforms. This is its primary advantage. An oscilloscope measures analog signal characteristics with high bandwidth and low noise. Option A (rise time measurement) requires the analog fidelity of an oscilloscope; a logic analyser only captures a threshold crossing. Option B (power supply noise) requires analog measurement and is suited to an oscilloscope. Option D (CAN differential voltage) requires analog differential measurement; a logic analyser would need differential probes and lacks the analog precision to characterise the differential swing.

---

**Q3 -- Answer: B**

On most Cortex-M devices, address 0x00000000 is the beginning of the flash memory map (where the vector table resides) and reading from it is valid. However, if the MPU is configured to mark the null page as non-accessible (a defensive security practice), an access generates a MemManage fault. Without MPU configuration, a write to address 0 will attempt to write to flash, which on most flash controllers is either ignored or triggers a BusFault (bus error response). Whether the fault is a MemManage or HardFault depends on whether the MemManage fault is enabled and not masked; if MemManage is disabled or the fault escalates (for example, if the MemManage handler itself faults), it becomes a HardFault. Option A is wrong; UsageFault covers instruction execution errors (undefined instructions, unaligned access with strict alignment enabled), not memory access violations. Option C is wrong; address 0 is the flash origin on most Cortex-M devices and reads are valid in the default configuration. Option D is partially true for reads without MPU protection, but a write to address 0 will fault.

---

**Q4 -- Answer: B**

Without an MPU, the processor has no hardware mechanism to detect a stack overflow at the exact overflowing instruction. The stack grows downward toward other variables (typically `.bss` or `.data` variables, or even code in some layouts). When the stack overflows, it silently overwrites adjacent memory. The corrupted data may be a rarely-used variable, a rarely-executed code path, or function return addresses that are only used later. The consequence is that the crash or incorrect behaviour often happens far removed in time and code location from the actual overflow event, making it very difficult to diagnose. Option A describes MPU-protected stack behaviour, where a MemManage fault fires when the stack crosses a guard region. Option C is wrong; stack usage cannot generally be determined at compile time because it depends on runtime call depth and local variable usage. Option D is wrong; the C runtime library in bare-metal embedded does not typically monitor stack usage.

---

**Q5 -- Answer: B**

A watchdog timer is a hardware counter that must be periodically reset (kicked/refreshed) by software. If the software fails to kick the watchdog within the timeout period -- due to an infinite loop, deadlock, HardFault, or any other software failure -- the watchdog expires and forces a hardware reset of the processor, recovering system operation. Option A describes a cycle counter or DWT (Data Watchpoint and Trace) unit, which is used for timing. Option C describes a hardware timer used for delays. Option D describes a Brown-Out Detector (BOD) or Power-On Reset (POR) circuit, which is a separate hardware block.

---

**Q6 -- Answer: B**

A data watchpoint (also called a data breakpoint or access watchpoint) is a hardware debug feature that halts the CPU when a specific memory address is accessed. The developer can configure it to fire on read, write, or read-write access. When it triggers, the CPU halts after the access, and the debugger provides a full snapshot of registers, the call stack, and local variables, allowing the developer to see exactly which code path accessed the variable. This is invaluable for finding the source of unexpected variable corruption. Option A describes a code breakpoint (instruction breakpoint), which fires when the PC reaches a specific address. Option C is not a feature of standard watchpoints -- they fire on any access regardless of value change. Option D describes stack overflow detection, which is a different debug mechanism.

---

**Q7 -- Answer: B**

This is a classic and common embedded debugging issue. When a debugger downloads a firmware image, it writes the program to flash but it also leaves a copy of the `.data` section already in RAM from the download process. If the startup code has a bug (for example, `_sidata` pointing to the wrong flash address, or the copy loop using the wrong size calculation), the firmware still runs correctly because the initial values are already in RAM from the download. After a power cycle, RAM contents are indeterminate, and the buggy startup code fails to correctly initialise `.data`, causing crashes. Option A is a real possibility but describes a hardware fault, not the "most likely" software cause. Option C is possible in some systems but very unusual; JTAG does not generally control CPU clock speed. Option D is also possible but typically causes deterministic failures immediately on startup, not crashes that work with the debugger.

---

**Q8 -- Answer: B**

In I2C, after the master sends an address byte and the R/W bit, the addressed slave must pull SDA low during the 9th clock pulse to send an ACK (acknowledge). If SDA remains high (not pulled low), it is a NACK (not acknowledge). This indicates the slave did not respond. The most common causes are: incorrect slave address in the master software, the slave device is not powered, the slave is not physically present, the slave is still processing a previous command and cannot respond, or the slave has entered an error state. Option A is a different type of issue: clock speed violations cause the ACK to be incorrectly timed but typically do not prevent the slave from driving ACK once it has received the full address. Option C is wrong; the R/W bit is sent after the address -- a wrong R/W bit does not cause a NACK unless the slave does not support the requested direction. Option D (swapped lines) would show completely garbled bus traffic, not a clean address byte with a missing ACK.

---

**Q9 -- Answer: B**

Peripheral registers must be accessed in strict program order and must not be cached, because peripheral registers have side effects on read (for example, reading a FIFO pops data; reading a status register clears an interrupt flag) and writes must complete before the next operation. The Cortex-M architecture uses memory attributes to control this. Peripheral space is mapped as Device or Strongly-Ordered memory. Device memory prevents speculative reads and merging of writes but allows limited reordering within a device region. Strongly-Ordered memory prevents all reordering and is the most restrictive. Option A (cacheable) is wrong; caching a peripheral register read would cause the processor to return a stale value instead of reading the current hardware state. Option C (Write-Through Cache) still caches read data, which is wrong for peripheral registers. Option D (Execute-Never) is correct for the XN attribute (preventing code execution from peripheral space), but peripheral registers must not be cached.

---

**Q10 -- Answer: B**

`assert()` is defined in `<assert.h>` and is controlled by the `NDEBUG` macro. When `NDEBUG` is defined (as it is in release/production builds), the preprocessor expands `assert(expr)` to `((void)0)` -- a no-op expression with no code generated. This means there is literally zero runtime overhead and zero code size impact in production. Option A is wrong; `assert()` has no logging behaviour; it is a simple halt mechanism. Option C is wrong; the no-op means the expression is not even evaluated, let alone a handler called. Option D is wrong; `assert()` does not generate error-return code.

---

**Q11 -- Answer: B**

Hardware abstraction through dependency injection is the standard technique for making embedded drivers testable on a host machine. The driver's register access code is hidden behind a thin interface -- typically a function pointer, a C struct of function pointers, or a set of weak functions. In the test build, these are replaced with mock or stub implementations that do not require physical hardware. The test code can configure what values the mock returns (simulating hardware state) and verify what values were written (asserting correct driver behaviour). This is the basis of frameworks like CMock, FFF (Fake Function Framework), and similar embedded test tools. Option A is the least preferred approach; running on the target is slow, requires physical hardware, and is hard to automate. Option C does not test the RTOS interaction and leaves large parts of the driver untested. Option D is a valid integration test approach but is expensive and not suitable for unit testing.

---

**Q12 -- Answer: B**

IBUSERR (instruction bus error) in the BFSR means the CPU attempted to fetch an instruction from an address that generated a bus error -- typically an address that is not mapped in the memory map, or is in a non-executable region. Since the faulting PC varies each time, the issue is not a consistent call to one bad address but rather a different corrupted address on each occurrence. The most probable cause is stack corruption: a buffer overflow, an out-of-bounds array write, or a dangling pointer write is overwriting a return address on the stack. When the function eventually returns, the CPU loads the corrupted value as the return address and attempts to fetch instructions from an arbitrary, often unmapped location. Option A (null pointer dereference) would show a consistent PC near address 0 or in a specific function. Option C (integer overflow) does not directly cause an instruction bus fault. Option D (unaligned SIMD) would show a UsageFault with the UNALIGNED bit set in UFSR, not IBUSERR.

---

**Q13 -- Answer: B**

Even without debug symbols, the linker map file provides the start address and size of every function. The developer can walk through the captured stack memory looking for word-aligned values in the flash address range (on a Cortex-M, flash is typically at 0x08000000; return addresses from Thumb functions have the LSB set, so they appear as 0x08XXXXXX | 1). When a candidate return address is found, the developer looks it up in the linker map to identify which function it falls within, and cross-references with the binary disassembly (`objdump -d`) to see the exact call site. This technique is called manual stack unwinding and is a real-world skill used in production debugging. Option A is an overly pessimistic view; engineers successfully debug production firmware this way routinely. Option C is incorrect; Cortex-M does not have a hardware call stack buffer (though ETM trace can capture branch history, that is a different expensive probe feature). Option D is valid for intermittent bugs but may take a long time if the failure rate is low.

---

**Q14 -- Answer: B**

The CAN peripheral inside the microcontroller generates digital TX/RX signals. These are routed to a CAN transceiver IC (such as the MCP2551, TJA1050, SN65HVD230, or similar), which converts the digital signals to the differential CANH/CANL bus levels. If the transceiver is disabled (via a hardware enable pin left deasserted), is unpowered (missing VCC or power-supply issue), or is in standby or sleep mode (many transceivers have a low-power mode controlled by a pin), it will not drive the bus regardless of what the microcontroller's CAN peripheral is outputting. The observation -- correct logic on CANTX but no differential activity on the bus -- is the classic symptom of a transceiver issue. Option A would cause all nodes to disagree on bit boundaries and produce bit errors, but bus activity would still be visible. Option C is wrong; arbitration loss occurs when two nodes transmit valid competing frames, not when one node transmits into a silent bus. Option D would cause the receiving nodes to silently discard frames but not prevent bus activity.

---

**Q15 -- Answer: B**

Providing fake (stub) implementations of FreeRTOS API functions is the standard industry approach for unit testing RTOS-dependent code on a host machine. The fakes implement just enough behaviour (a queue as a circular buffer, a semaphore as a counter) to exercise the driver logic without the overhead or complexity of a full RTOS kernel. Frameworks such as FFF (Fake Function Framework) or CMock can generate these stubs automatically from header files. This gives fast, deterministic, easily automated tests that run in milliseconds on a CI server. Option A (porting FreeRTOS to the host) is possible but adds significant complexity and still runs the real RTOS scheduler, making tests non-deterministic. Option C removes the code under test rather than testing it; if queue interactions are part of the driver's behaviour, removing them leaves the driver untested. Option D is integration or HIL testing, which has its place but is not unit testing.
