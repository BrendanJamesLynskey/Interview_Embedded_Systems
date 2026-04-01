# JTAG and SWD

## Prerequisites
- ARM Cortex-M architecture fundamentals: core registers, exception model
- Basic understanding of serial communication protocols
- Familiarity with GDB or similar debugger concepts

---

## Concept Reference

### JTAG Overview

JTAG (Joint Test Action Group, IEEE 1149.1) is a four-wire serial interface originally designed for board-level boundary scan testing. It was later extended to provide debug access to processor cores and on-chip peripherals.

```
JTAG Signal Lines:
  TCK   - Test Clock       : master-generated clock, all state transitions on rising edge
  TMS   - Test Mode Select : controls the TAP state machine (sampled on TCK rising edge)
  TDI   - Test Data In     : serial data shifted into the device (MSB first)
  TDO   - Test Data Out    : serial data shifted out of the device
  TRST# - Test Reset       : optional async reset of TAP controller (active low)

Pin count: 4 mandatory + 1 optional = minimum 4 pins
```

**TAP (Test Access Port) State Machine — 16-state FSM:**

```
                    +-------+
         1 -------->| Reset |<--- (hold TMS=1 for 5 TCK cycles from any state)
                    +---+---+
                    1   |0
                    |   v
                    |  +------+
                    |  | Idle |
                    |  +--+---+
                    |  0  |1
                    |  |  |
          +---------+  |  +------------------+
          |            |                     |
          v            v                     v
     +--------+   +---------+          +---------+
     | Select |   | Select  |          | Select  |
     | DR-Scan|   | IR-Scan |          | DR-Scan |
     +---+----+   +----+----+          (similar subtree)
         |             |
         v             v
    +--------+    +--------+
    | Capture|    | Capture|
    |   DR   |    |   IR   |
    +---+----+    +----+---+
        |              |
        v              v
    +--------+    +--------+
    | Shift  |    | Shift  |    <- TDI/TDO data clocked here
    |   DR   |    |   IR   |
    +---+----+    +----+---+
        |              |
        v              v
    +--------+    +--------+
    | Update |    | Update |
    |   DR   |    |   IR   |
    +--------+    +--------+
```

**JTAG Daisy-Chain (multi-device):**

```
Host debugger
    |
   TCK/TMS (broadcast to all devices)
    |
   TDI --> [Device 1 TDI->TDO] --> [Device 2 TDI->TDO] --> [Device 3 TDI->TDO] --> Host TDO

Total IR length = sum of all IR lengths in chain.
Requires knowing the IR length of every device to address a specific one.
```

---

### SWD Overview

SWD (Serial Wire Debug) is a 2-wire alternative to JTAG developed by ARM. It uses the same Debug Access Port (DAP) as JTAG but over a simpler, lower pin-count interface.

```
SWD Signal Lines:
  SWCLK - Serial Wire Clock  : host-generated clock
  SWDIO - Serial Wire Data   : bidirectional, host and target share this line

Pin count: 2 (often multiplexed with JTAG TMS/TCK on Cortex-M parts)

SWDIO line direction:
  Host drives  : when sending request phase
  Target drives: when sending acknowledge and response phases

SWD Packet Structure:
  +-------+------+-------+-------+------+----+
  | Start | APnDP| RnW   | A[2:3]| Parity| Stop|  <- 8-bit request
  +-------+------+-------+-------+------+----+
         +-------+-----+                           <- 3-bit ACK (OK/WAIT/FAULT)
                       +--------...--------+------+ <- 32-bit data + parity
```

---

### JTAG vs SWD Comparison

| Property              | JTAG                          | SWD                            |
|-----------------------|-------------------------------|--------------------------------|
| Pin count             | 4 (+ optional TRST#)          | 2                              |
| Multi-device support  | Native daisy-chain            | Not supported (single device)  |
| Bandwidth             | Higher (parallel IR/DR chains)| Lower (serial, 2-wire)         |
| Trace support         | Full ETM via JTAG             | Via SWO (single-wire output)   |
| Common usage          | Board-level test, FPGA config | Cortex-M production debugging  |
| Protocol complexity   | High (TAP state machine)      | Lower (packet-based)           |
| Pin multiplexing      | Dedicated JTAG pins           | Often shared with JTAG TMS/TCK |
| Supported on          | All ARM, MIPS, RISC-V, x86   | ARM Cortex-M/A/R series only   |

---

### Debug Access Port (DAP) Architecture

The DAP is the hardware block through which a debugger accesses the target. It sits between the physical debug interface (JTAG/SWD) and the internal debug resources.

```
+--------------------+        +--------------------+
| Debug Host (PC)    |        |   Target SoC       |
|                    |        |                    |
| GDB + OpenOCD /    |        | +--------------+   |
| J-Link software    | JTAG   | |  DP (Debug   |   |
|                    |<------>| |  Port)       |   |
|                    |  or    | |  JTAG-DP or  |   |
|                    | SWD    | |  SW-DP       |   |
+--------------------+        | +------+-------+   |
                               |        |           |
                               |  DAP internal bus  |
                               |        |           |
                               | +------+-------+   |
                               | |  AHB-AP      |   |  <- Access Port for AHB
                               | |  (AP #0)     |   |     bus transactions
                               | +------+-------+   |
                               |        |           |
                               | +------+-------+   |
                               | | CPU Core     |   |
                               | | Debug Logic  |   |
                               | | (Halt, Step) |   |
                               | +--------------+   |
                               |                    |
                               | +--------------+   |
                               | | MEM-AP       |   |  <- Access Port for
                               | | (AP #1)      |   |     direct memory access
                               | +--------------+   |
                               +--------------------+

DP (Debug Port): implements the physical JTAG or SWD interface
AP (Access Port): logical gateways to specific on-chip resources
  AHB-AP: allows read/write of any memory-mapped address
  APB-AP: allows access to CoreSight trace and debug components
```

**Key DAP registers (accessed via DP):**

```
DPIDR  (0x00) : Debug Port Identification Register -- device identification
CTRL/STAT     : Power-up request/acknowledge, sticky error flags
SELECT        : Selects which AP is active and which AP register bank
RDBUFF        : Holds the last read data value (avoids extra bus transaction)
```

---

### ETM Trace (Embedded Trace Macrocell)

ETM provides non-intrusive, real-time instruction trace. Unlike halting the core with a breakpoint, ETM records every instruction executed and streams the compressed trace data out via the trace port.

```
ETM Architecture:

  CPU Core
     |  (instruction stream)
     v
  +-----+
  | ETM |  -- generates compressed branch trace packets
  +--+--+
     |
     v
  +------+
  | FIFO |  (Trace Data Buffer)
  +--+---+
     |
  +--+--+
  | TPIU|  (Trace Port Interface Unit)
  +--+--+
     |
     +---> SWO (1-bit serial, if using SWD)
     +---> Parallel Trace Port (4-bit or 16-bit, needs trace-capable debugger probe)
     +---> ETB (Embedded Trace Buffer) -- stores trace on-chip, read back via DAP

Trace compression:
  Only branch targets and exception entries need to be fully specified.
  Sequential execution is inferred: "N instructions executed then branch to 0x0800_1234"
  Typical compression ratio: 4:1 to 10:1
```

**ETM trace use cases:**
- Profile code paths to identify which functions are called most frequently
- Reconstruct the exact execution sequence leading up to a crash
- Find sporadic timing-dependent bugs where halt-and-inspect changes behaviour
- Code coverage measurement without instrumentation overhead

---

### Breakpoints and Watchpoints

**Hardware breakpoints (FPB — Flash Patch and Breakpoint unit on Cortex-M):**

```
Cortex-M3/M4: 6 hardware breakpoints (FPB comparators)
Cortex-M33:   8 hardware breakpoints

Operation:
  1. Debugger writes target address into FPB comparator register
  2. Core compares PC against all comparator addresses every cycle
  3. On match: DebugMonitor exception or halting debug entry
  4. Core halts; debugger can inspect registers and memory

Key property: Zero execution overhead when not triggered.
              The core pipeline is unmodified; comparison is in parallel hardware.

Flash Patch capability: FPB can also remap Flash addresses to SRAM,
  allowing patching of ROM code without reflashing (useful in production).
```

**Software breakpoints (BKPT instruction):**

```c
/* ARM Thumb breakpoint encoding */
__asm("BKPT #0");    /* Encoding: 0xBExx, where xx is the immediate */

/* The debugger inserts BKPT by temporarily replacing the original
   instruction in memory with the BKPT encoding.
   On execution: UsageFault (or DebugMonitor) exception fires.
   Debugger restores original instruction and halts.

   Limitation: Cannot be set in Flash (ROM) unless Flash Patch is used.
   Unlimited in number (only limited by available RAM/Flash). */
```

**Watchpoints (DWT — Data Watchpoint and Trace unit):**

```
Cortex-M3/M4: 4 watchpoint comparators
Cortex-M33:   Up to 8 comparators (implementation-defined)

Types:
  Read watchpoint   : triggers when specified address is read
  Write watchpoint  : triggers when specified address is written
  Read/Write        : triggers on either access

Masking capability:
  DWT_MASK register allows masking of low address bits, creating
  a range watchpoint (e.g., watch any write to a 256-byte buffer):
    DWT_COMP = 0x20001000  (base address)
    DWT_MASK = 7           (ignore bits [7:0] => watch 256 bytes)
    DWT_FUNCTION = 0x6     (write access trigger)
```

---

## Tier 1 — Fundamentals

### Question F1
**What is the difference between JTAG and SWD? When would you choose one over the other?**

**Answer:**

JTAG uses four wires (TCK, TMS, TDI, TDO) and an optional TRST#. Data is shifted into the device via TDI and out via TDO, controlled by the TAP state machine driven via TMS and TCK. JTAG supports daisy-chaining multiple devices on a single chain.

SWD uses two wires (SWCLK, SWDIO) with SWDIO being bidirectional. It is specific to ARM Cortex processors and provides access to the same Debug Access Port as JTAG but with fewer pins.

**Choose SWD when:**
- Pin count is constrained (common on small MCU packages: QFN-32 or smaller)
- Debugging a single Cortex-M device (no need for multi-device chain)
- Using SWO for lightweight trace output (ITM printf-style trace)
- Production debug connectors on space-constrained PCBs (e.g., Tag-Connect 2-pin footprint)

**Choose JTAG when:**
- Debugging a multi-device board scan chain (MCU + FPGA + boundary scan ICs)
- Full ETM parallel trace is required (needs the extra trace port pins, but the debug connector is JTAG-based)
- Working with processors that do not support SWD (MIPS, RISC-V, x86)
- Boundary scan testing of PCB interconnects (a JTAG-only feature)

**Common mistake:** Candidates assume JTAG is always "better" because it has more pins. In practice, SWD is overwhelmingly preferred for Cortex-M development because of its lower pin count and sufficient bandwidth for typical debugging tasks.

---

### Question F2
**What is the Debug Access Port (DAP) and what are Access Ports (APs)?**

**Answer:**

The DAP is the hardware block that translates debug interface transactions (JTAG or SWD) into internal bus accesses. It consists of two layers:

**Debug Port (DP):** The physical interface layer. On ARM Cortex-M devices, this is either a JTAG-DP (uses the JTAG TAP) or SW-DP (uses SWD protocol). The DP handles clocking, framing, error detection, and power control requests.

**Access Ports (APs):** Logical endpoints reached through the DP. Each AP provides access to a different internal resource:

- **AHB-AP (AP #0 on most Cortex-M):** Allows the debugger to perform read/write transactions on the AHB bus, giving access to all memory-mapped addresses (Flash, SRAM, peripherals). This is how GDB reads/writes variables and sets software breakpoints.
- **APB-AP:** Provides access to CoreSight debug components (ETM, DWT, FPB, ITM) which hang off an internal debug APB bus.

A debugger program (OpenOCD, pyOCD, J-Link) writes to the DP SELECT register to select the desired AP, then accesses the AP's registers via the DP's DRW (Data Read/Write) register.

---

### Question F3
**What is the difference between a hardware breakpoint and a software breakpoint? When can each be used?**

**Answer:**

**Hardware breakpoints** use dedicated comparator circuits (FPB on Cortex-M) that compare the current PC against programmed addresses entirely in hardware. They require no modification of program memory and therefore work in Flash, ROM, or any read-only region. The number is limited by hardware — typically 4-8 on Cortex-M devices.

**Software breakpoints** work by replacing the instruction at the target address with a `BKPT` instruction (or equivalent trap). When the CPU executes the trap, it takes a debug exception. The debugger catches the exception, restores the original instruction, and halts.

| Property           | Hardware           | Software                      |
|--------------------|--------------------|-------------------------------|
| Count              | 4-8 (fixed)        | Unlimited                     |
| Works in Flash/ROM | Yes                | No (cannot write Flash at runtime without explicit Flash programming) |
| Works in RAM       | Yes                | Yes                           |
| Execution overhead | Zero               | Zero (halts on first execution) |
| Running from ROM   | Required           | Cannot be used                |

**Common mistake:** In systems that copy code from Flash to RAM and execute from RAM, software breakpoints work fine. But when code executes directly from Flash (XIP), only hardware breakpoints can be used until all 4-8 are exhausted.

---

### Question F4
**Explain what a watchpoint is and give a practical scenario where you would use one.**

**Answer:**

A watchpoint (also called a data breakpoint) triggers when a specific memory address is accessed. Unlike a breakpoint which triggers on code execution, a watchpoint monitors data reads or writes. The DWT unit in Cortex-M implements watchpoints in hardware with zero execution overhead.

**Practical scenario — tracking memory corruption:**

You have a global variable `config.baud_rate` that is being set to an invalid value (0x00000000) causing a UART malfunction, but you cannot find which code path writes the wrong value.

```c
/* Variable being corrupted */
volatile uint32_t config_baud_rate;   /* stored at, say, 0x20001048 */

/* In GDB with OpenOCD/J-Link: */
(gdb) watch *(uint32_t*)0x20001048
Hardware watchpoint 1: *(uint32_t*)0x20001048
(gdb) continue

/* Execution halts when the write occurs, GDB shows:
   Hardware watchpoint 1: *(uint32_t*)0x20001048
   Old value = 115200
   New value = 0
   error_handler () at main.c:87  */
```

The CPU halts immediately after the write, with the call stack intact, revealing exactly which function performed the corrupting write. This is far more efficient than adding `assert()` checks or printf statements throughout the codebase.

---

## Tier 2 — Intermediate

### Question I1
**Describe what happens electrically and logically when a debugger probe first connects to a Cortex-M target via SWD and executes the first memory read.**

**Answer:**

The connection and first read sequence involves several distinct phases:

**Phase 1 — Line reset and dormant state exit:**
```
1. Host drives SWDIO high, then sends 50+ clock pulses with SWDIO high
   => Forces any previous SWD or JTAG state machine to reset
2. JTAG-to-SWD switch sequence: specific 16-bit code 0xE79E on TMS/TDI
   => Dormant target switches from JTAG TAP to SW-DP
3. Host sends 50 more idle clocks, then 8-clock idle
```

**Phase 2 — Read DPIDR (Debug Port ID Register):**
```
Request packet (8 bits):
  Start=1, APnDP=0 (DP), RnW=1 (read), A[2:3]=00 (DPIDR addr), Parity, Stop=0

Turnaround: SWDIO released by host, target drives
ACK (3 bits): 001 = OK

Data phase (33 bits):
  32-bit DPIDR value + 1 parity bit

DPIDR for Cortex-M4: 0x2BA01477
  Bits [3:0]  = 0x7 (version)
  Bits [11:8] = 0x4 (part number indicates M4)
  Bits [27:12]= 0x0BA0 (ARM designer ID)
```

**Phase 3 — Power up the debug domain:**
```
Write CTRL/STAT register:
  Set CSYSPWRUPREQ (bit 30) and CDBGPWRUPREQ (bit 28)
Poll CTRL/STAT until CSYSPWRUPACK and CDBGPWRUPACK are set.
Without this step, all AP accesses return FAULT.
```

**Phase 4 — Select AHB-AP and read memory:**
```
Write SELECT register: AP=0 (AHB-AP), APBANKSEL=0 (bank containing CSW/TAR/DRW)
Write AHB-AP CSW: set Size=010 (32-bit), AddrInc=01 (auto-increment)
Write AHB-AP TAR: 0x20000000  (SRAM address to read)
Read  AHB-AP DRW: => 32-bit value from address 0x20000000
```

**Why this matters in an interview:** Understanding this sequence shows you know the difference between the DP (physical interface) and AP (logical bus access), why power-up sequencing matters, and how the debugger translates a high-level "read memory at 0x20000000" into a precise protocol exchange.

---

### Question I2
**What is ETM trace and how does it differ from ITM (Instrumentation Trace Macrocell) output? When would you use each?**

**Answer:**

**ETM (Embedded Trace Macrocell):** Hardware that captures the instruction stream as the CPU executes, producing a compressed trace of every instruction executed. The CPU is not halted and the trace is completely non-intrusive.

**ITM (Instrumentation Trace Macrocell):** A software-driven mechanism where code writes to ITM stimulus registers. The ITM serialises these writes and outputs them on SWO. Commonly used for `printf`-style debug output without a UART.

```c
/* ITM output — requires code instrumentation */
void ITM_SendChar(uint8_t ch) {
    /* Wait until stimulus port is ready */
    while (ITM->PORT[0].u32 == 0) {}
    ITM->PORT[0].u8 = ch;  /* Write triggers SWO output */
}

/* SWD + SWO wiring:
   SWCLK  -> probe
   SWDIO  -> probe
   SWO    -> probe (third wire, often on Cortex-M SWO pin = PB3 on STM32) */
```

| Property             | ETM                                  | ITM                              |
|----------------------|--------------------------------------|----------------------------------|
| Intrusiveness        | None (hardware)                      | Slight (code writes to register) |
| Data captured        | Every instruction executed           | Only what software writes        |
| Pin requirement      | 1-wire SWO (limited) or 4-bit trace port | 1-wire SWO                   |
| Bandwidth            | Can overwhelm SWO; needs parallel port for high-speed code | Low (application-rate data) |
| Use case             | Crash reconstruction, profiling, coverage | Printf debug, event logging |
| Availability         | Cortex-M3 and above (optional)       | Cortex-M3 and above              |
| Debugger support     | Needs ETB or high-speed trace probe  | Any probe with SWO support       |

**Practical guidance:**
- Use **ITM** for day-to-day debug logging — it is lightweight and widely supported.
- Use **ETM** when debugging a non-reproducible crash, a timing-sensitive bug that halting disturbs, or when profiling a performance-critical path with cycle accuracy.
- ETM requires an ETB (on-chip trace buffer) or a fast-enough trace port — standard CMSIS-DAP probes typically cannot keep up with ETM at full CPU speed.

---

### Question I3
**You are debugging a Cortex-M4 target and you have used all 6 hardware breakpoints. The code runs from Flash. How do you set additional breakpoints?**

**Answer:**

When hardware breakpoints are exhausted and code runs from Flash, software breakpoints cannot be directly used. There are four approaches, each with trade-offs:

**Option 1 — Remove unused hardware breakpoints**

Review all active breakpoints; delete those no longer needed. Trivial but often overlooked.

**Option 2 — Use the Flash Patch (FPB remap) capability**

The FPB on Cortex-M3/M4 can remap a Flash address to a SRAM address. The debugger can:
1. Copy the target function to SRAM.
2. Patch the remap table to redirect the Flash address to SRAM.
3. Place software breakpoints freely in the SRAM copy.

This is transparent at the source level but requires debugger support (J-Link does this automatically).

**Option 3 — Manually instrument code in RAM**

```c
/* Add a BKPT instruction manually in a RAM-resident function */
void debug_hook(void) {
    __asm volatile ("BKPT #0");  /* Always in RAM; software breakpoint works */
}

/* Call from the code path under investigation */
debug_hook();
```

**Option 4 — Use conditional watchpoints as pseudo-breakpoints**

If the function under investigation writes a unique value to a variable, set a write watchpoint on that variable:

```c
volatile uint32_t debug_marker = 0;

/* At the desired stop point: */
debug_marker = 0xDEADBEEF;  /* watchpoint on this address triggers */
```

**Interview insight:** Knowing the FPB remap mechanism demonstrates understanding of Cortex-M CoreSight architecture beyond simply "set breakpoint in IDE". Candidates who have debugged real ROM-based systems will know this problem and at least one solution.

---

### Question I4
**What is boundary scan (JTAG) and how is it used for board-level testing? How does it differ from processor debug use of JTAG?**

**Answer:**

**Boundary scan** (IEEE 1149.1) places a shift-register cell between each IC pin and the internal logic. These boundary scan cells form a daisy-chain (the boundary scan register) that can be read and written via the JTAG TAP. This allows the tester to:

1. **Drive outputs:** Force pin values to specific logic levels to stimulate other devices.
2. **Sample inputs:** Read back pin values to observe logic states.
3. **Test interconnect:** Detect open circuits and short circuits between ICs on the PCB.

```
Example: Testing a bus connection between two ICs

IC_A pin PA3 --- PCB trace --- IC_B pin PB7

JTAG test:
  1. Load IC_A boundary scan register: set PA3=1 in EXTEST mode
  2. Apply EXTEST instruction: PA3 now drives the PCB trace
  3. Shift in IC_B boundary scan register: read PB7 value
  4. Expected: PB7=1 (trace connected)
     If PB7=0: open circuit fault detected
     If another pin also reads 1 unexpectedly: short circuit detected

BSDL (Boundary Scan Description Language) file:
  Describes each IC's boundary scan register structure.
  Required by test generation tools (e.g., XJTAG, Boundary Scan Tools).
```

**Processor debug vs boundary scan:**

| Aspect              | Processor Debug                          | Boundary Scan                          |
|---------------------|------------------------------------------|----------------------------------------|
| Target              | CPU core internals (halt, step, memory)  | IC pin states and PCB interconnects    |
| JTAG instruction    | EXTEST/IDCODE not used; custom ARM TAP   | EXTEST, SAMPLE, BYPASS (IEEE 1149.1)  |
| When used           | Software development and field debug     | PCB manufacturing test, bring-up       |
| Requires            | JTAG debug adapter and GDB               | Boundary scan test system + BSDL files |
| Intrusive?          | Yes (can halt CPU)                       | Yes to IC pins, not to running code    |

The two uses share the same physical JTAG wires but use entirely different TAP instructions and scan chains. On multi-device boards, the processor's ARM TAP sits in the scan chain alongside ICs with boundary scan TAPs.

---

## Tier 3 — Advanced

### Question A1
**Explain how OpenOCD translates a GDB `read memory` command into actual JTAG/SWD transactions. Trace the full path from GDB client to target memory.**

**Answer:**

The path involves four software and hardware layers:

```
GDB Client (gdb or arm-none-eabi-gdb)
    |
    | GDB Remote Serial Protocol (RSP) over TCP socket (port 3333)
    | Packet: $m20001000,4#xx  (read 4 bytes at 0x20001000)
    v
OpenOCD (gdbserver + target layer)
    |
    | 1. RSP packet parsed: address=0x20001000, length=4
    | 2. Calls target_read_memory(target, 0x20001000, 4, 1, buffer)
    | 3. Routes to cortex_m_read_memory()
    | 4. Calls dap_ap_mem_access_tar() to set AHB-AP TAR register
    | 5. Calls dap_ap_read() on DRW register
    v
libusb / FTDI driver (for FTDI-based probes) or USB HID (for CMSIS-DAP)
    |
    | Queues DAP commands into USB bulk transfer to probe hardware
    v
Debug Probe Hardware (J-Link, CMSIS-DAP adapter, ST-Link)
    |
    | Translates DAP command into SWD/JTAG bit-banging:
    |
    | SWD transactions (in order):
    |   1. Write DP SELECT: APSEL=0 (AHB-AP), APBANKSEL=0
    |   2. Write AHB-AP TAR: 0x20001000
    |   3. Read  AHB-AP DRW  -- triggers AHB read on target
    |   4. Read  DP RDBUFF   -- retrieves the actual data (posted read)
    v
Target SoC
    |
    | AHB-AP generates AHB read transaction:
    |   HADDR=0x20001000, HSIZE=010 (32-bit), HTRANS=NONSEQ
    | AHB interconnect routes to SRAM
    | SRAM returns 32-bit data
    | AHB-AP latches data into DRW register
    v
Data travels back up the chain to GDB, displayed in memory window.
```

**Posted read mechanism:** AHB-AP uses a "posted read" protocol. The first read of DRW triggers the AHB transaction but returns stale data; the actual result is available on the *next* DP RDBUFF read. OpenOCD automatically handles this by always reading RDBUFF after DRW. This is a common source of bugs in custom DAP drivers.

---

### Question A2
**A production device must be debuggable in the field without exposing security vulnerabilities. Describe a secure debug architecture using ARM CoreSight features.**

**Answer:**

Unrestricted debug access is a significant security risk: an attacker with JTAG access can read encryption keys from RAM, bypass secure boot, or inject code. ARM provides several mechanisms for secure debug access control.

**Debug Authentication Signals (DBGEN, SPIDEN, NIDEN, SPNIDEN):**

```
Signal    Enables                                    Typical control
--------  -----------------------------------------  --------------------------------
DBGEN     Invasive debug (halt, step, breakpoints)   Tied low in production, or
           for non-secure world                       gated by auth token in OTP
SPIDEN    Invasive debug for secure world            Never enabled in deployed product
NIDEN     Non-invasive debug (ETM trace) non-secure  Can be enabled for profiling
SPNIDEN   Non-invasive debug for secure world        Controlled separately

These signals are sampled by the CoreSight components at power-on.
They can be driven from:
  - OTP fuse bits (permanent, one-time programmable)
  - Secure firmware after cryptographic authentication
  - Hardware security module (HSM) after challenge-response
```

**Challenge-Response Debug Unlock (ARM Platform Security Architecture approach):**

```
1. Developer connects JTAG probe, reads Device Unique ID via JTAG
   (readable even with DBGEN=0 from DPIDR/target identification regs or OTP)

2. Developer sends Device UID to secure key server

3. Key server computes HMAC-SHA256(UID, debug_unlock_secret_key)
   => 32-byte debug token

4. Developer writes debug token to a specific OTP location or sends via
   authenticated firmware command

5. Secure bootloader on target:
   a. Reads Device UID from OTP
   b. Derives expected token: HMAC-SHA256(UID, secret_key)
   c. Compares with received token
   d. On match: drives DBGEN=1, enabling debug access
   e. On mismatch: ignores, DBGEN remains 0

6. Debugger now has full access; token is single-use or time-limited
```

**Memory Protection Unit (MPU) as a second layer:**

Even with DBGEN enabled, the MPU can be configured by a secure firmware region to deny debugger AHB-AP access to specific address ranges (key storage, secure state). This uses the CoreSight MPU override registers.

**Interview insight:** Senior candidates should articulate the tension between debuggability and security, and be aware that "disable JTAG in production" is not the only option. Challenge-response debug unlock is used in production devices that need RMA debug capability.

---

### Question A3
**Describe the mechanism by which a JTAG scan chain configuration error causes incorrect device programming on a multi-device board. How would you diagnose it?**

**Answer:**

In a multi-device JTAG chain, the host must know the exact instruction register (IR) length of every device in the chain to address any one of them correctly. The JTAG standard only mandates that IR is at least 2 bits; actual lengths are device-specific (4-bit for many ARM Cortex-M, 10-bit for Xilinx FPGAs, 5-bit for various boundary-scan ICs).

**The misconfiguration scenario:**

```
Chain: [MCU (IR=4)] --> [FPGA (IR=10)] --> [Flash IC (IR=8)]
Total IR length: 22 bits

Correct addressing of Flash IC (device 3):
  Shift 22 bits: [Flash IR=8 bits] [FPGA BYPASS=1 bit] [MCU BYPASS=1 bit]
  Wait -- BYPASS instruction is 1 bit all-ones per device.
  So to program Flash: 0xFF [8b] | 0x1FF [10b but we use BYPASS=all-1s] | 0xF [4b]

If someone misconfigures FPGA IR as 6 bits instead of 10:
  Tool shifts only 18 bits total
  FPGA actually receives garbled non-bypass instruction
  FPGA enters unknown TAP state
  Flash IC never receives its programming instruction
  Programmer reports "flash write failure" or "verify error"
```

**Diagnosis procedure:**

```
Step 1 — Auto-detect IR lengths (OpenOCD auto-scan or J-Link auto-configuration):
  $ openocd -c "jtag scan_chain"
  Output shows each device's IDCODE and detected IR length.
  Compare against datasheets.

Step 2 — Verify IDCODE for each device:
  After reset (TMS=1 for 5 cycles), all devices load IDCODE instruction.
  Each device shifts its IDCODE (32 bits) out on TDO.
  Total shift: 32 * N bits for N devices.
  Decode each 32-bit IDCODE and verify manufacturer/part/version fields.

Step 3 — Check TDO signal integrity:
  Use oscilloscope or logic analyser on TDO.
  Expected: clean transitions correlated to TCK edges.
  If TDO is floating, stuck, or glitchy: check pull-up on TDO pin,
  verify TRST# is deasserted, check supply voltage on JTAG VCC.

Step 4 — Isolate devices:
  If possible, break the chain and test each device individually.
  Use TDI-TDO loopback (BYPASS mode) to verify each device passes data.
```

**Interview insight:** IR length misconfiguration is a classic bring-up problem. Knowing to start with IDCODE verification and auto-scan before assuming a silicon fault demonstrates systematic board bring-up methodology.
