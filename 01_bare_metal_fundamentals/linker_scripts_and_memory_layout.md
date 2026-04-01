# Linker Scripts and Memory Layout

## Prerequisites
- ARM Cortex-M memory map: Flash at 0x08000000, SRAM at 0x20000000 (device-specific)
- C storage classes: local vs global, static, const
- Basic ELF file structure: sections, symbols, segments
- Reset handler and startup sequence (see `startup_and_boot_sequence.md`)

---

## Concept Reference

### Why Linker Scripts Exist

The C compiler transforms source files into object files (`.o`). Each object file contains named sections (`.text`, `.data`, `.bss`, `.rodata`) but has no knowledge of the target device's memory layout. The linker's job is to:

1. Combine sections from all object files into a single executable.
2. Assign every section a concrete address in the target device's memory map.
3. Export symbols (like `_sdata`, `_estack`) that startup code uses to initialise memory.
4. Produce a binary image (`.hex` or `.bin`) that can be programmed into Flash.

Without a linker script, the linker uses a built-in default that is almost certainly wrong for a bare-metal embedded target.

### Standard Sections

| Section | Contents | LMA | VMA | Writeable? |
|---------|----------|-----|-----|-----------|
| `.text` | Compiled code (functions) | Flash | Flash | No |
| `.rodata` | Read-only data (string literals, `const` globals) | Flash | Flash | No |
| `.data` | Initialised global/static variables | Flash (init values) | SRAM | Yes |
| `.bss` | Zero-initialised global/static variables | (none) | SRAM | Yes |
| `.stack` | Stack region | (none) | SRAM | Yes |
| `.heap` | Heap region (if used) | (none) | SRAM | Yes |

### Anatomy of a GNU ld Linker Script

```
/* 1. MEMORY block: define available regions by name, origin, and length */
MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 1024K
    SRAM  (rwx) : ORIGIN = 0x20000000, LENGTH = 128K
}

/* 2. ENTRY: tell the debugger/loader the entry point symbol */
ENTRY(Reset_Handler)

/* 3. SECTIONS block: map input sections to output sections and memory regions */
SECTIONS
{
    /* Vector table -- must be first in Flash */
    .isr_vector :
    {
        . = ALIGN(4);
        KEEP(*(.isr_vector))
        . = ALIGN(4);
    } >FLASH

    /* Code and read-only data */
    .text :
    {
        . = ALIGN(4);
        *(.text .text.*)          /* all .text from all input objects */
        *(.rodata .rodata.*)      /* read-only constants */
        . = ALIGN(4);
        _etext = .;               /* exported symbol: end of .text+.rodata in Flash */
    } >FLASH

    /* Initialised data: stored in Flash (LMA), copied to SRAM (VMA) at startup */
    _sidata = LOADADDR(.data);    /* LMA start of .data initialisation image */
    .data :
    {
        . = ALIGN(4);
        _sdata = .;               /* VMA start -- used by startup copy loop */
        *(.data .data.*)
        . = ALIGN(4);
        _edata = .;               /* VMA end -- used by startup copy loop */
    } >SRAM AT>FLASH              /* VMA in SRAM, LMA (AT>) in Flash */

    /* Zero-initialised data: no Flash image needed */
    .bss :
    {
        . = ALIGN(4);
        _sbss = .;                /* start of BSS -- used by startup zero loop */
        *(.bss .bss.*)
        *(COMMON)                 /* uninitialized data from legacy C code */
        . = ALIGN(4);
        _ebss = .;                /* end of BSS -- used by startup zero loop */
    } >SRAM

    /* Stack: placed at the top of SRAM (stack grows downward) */
    ._stack :
    {
        . = ALIGN(8);             /* ABI requires 8-byte stack alignment */
        _sstack = .;              /* lowest valid stack address (overflow sentinel) */
        . = . + 0x2000;           /* 8 KB stack */
        . = ALIGN(8);
        _estack = .;              /* top of stack -- loaded into SP at reset */
    } >SRAM
}
```

### The `AT>` Syntax (Scatter Loading)

The `AT>FLASH` clause in the `.data` section specifies the LMA (where the bytes are stored in the binary image) separately from the VMA (where they will be at runtime). This is the linker script equivalent of ARM's "scatter loading":

```
>SRAM       -- VMA: the section's symbols are addressed in SRAM
AT>FLASH    -- LMA: the section's bytes are stored in the Flash image
```

The startup copy loop uses `_sidata` (= `LOADADDR(.data)` = LMA start) as the source and `_sdata`/`_edata` as the destination range.

### KEEP and Section Garbage Collection

The linker can remove unreferenced sections with `--gc-sections`. This is desirable for stripping dead code, but the vector table is an example of data that is never referenced by any C code (no function calls it by name), so it would be eliminated. The `KEEP()` directive tells the linker to retain the section regardless of references:

```
KEEP(*(.isr_vector))
```

Similarly, interrupt handlers that are only referenced from the vector table (which itself is only kept by `KEEP`) need the `__attribute__((used))` attribute or must be referenced via `KEEP` to survive garbage collection.

---

## Tier 1 -- Fundamentals

### Question F1
**What is the difference between `.data` and `.bss`? What would happen if a variable in `.bss` was placed in `.data` instead, and vice versa?**

**Answer:**

**`.data`:** Contains initialised global and static variables -- those with a non-zero (or explicitly specified) initial value:

```c
int counter = 5;          /* in .data -- initial value 5 stored in Flash */
static float gain = 1.5f; /* in .data */
```

**`.bss`:** Contains zero-initialised global and static variables -- those with no explicit initialiser or with an explicit initialiser of zero:

```c
int status;               /* in .bss -- initialised to 0 by C standard */
static char buf[256];     /* in .bss -- zeroed at startup */
int flag = 0;             /* compiler may put this in .bss (zero-init optimisation) */
```

**If a `.bss` variable were placed in `.data`:**

The Flash binary image would contain a block of zeros for the variable's initial value. This wastes Flash space. For a 256-byte buffer that should be in `.bss`, placing it in `.data` adds 256 bytes to the Flash image unnecessarily. For small embedded systems with tight Flash budgets this matters.

**If a `.data` variable were placed in `.bss`:**

The initial value would never be stored in Flash, and the startup code's `.bss` zero-fill loop would set the variable to 0 regardless of its declared initial value. The program would run with a wrong initial value -- a subtle bug with no compiler warning.

**Common mistake:** Declaring `static int table[] = {0, 0, 0, 0}` -- the programmer intends an all-zero table and thinks it will be in `.bss`, but an explicit zero initialiser may cause the compiler to put it in `.data`. Use no initialiser or memset at runtime for large zero arrays.

---

### Question F2
**What does the `MEMORY` block in a GNU ld linker script define? What happens if you specify a section that exceeds the size of its target region?**

**Answer:**

The `MEMORY` block defines the available physical memory regions of the target device:

```
MEMORY
{
    name (attributes) : ORIGIN = start_address, LENGTH = size
}
```

**Attributes:**

| Character | Meaning |
|-----------|---------|
| `r` | Region is readable |
| `w` | Region is writeable |
| `x` | Region contains executable code |
| `a` | Region is allocatable (default for all) |
| `!` | Negate the following attribute |

**What happens when a section overflows:**

The linker emits a hard error:

```
region `FLASH' overflowed by 1234 bytes
```

This is a linker error, not a warning, and the build fails. The developer must either:
1. Reduce code/data size (enable optimisation, remove unused features).
2. Use external Flash or off-chip memory.
3. Use flash compression or XIP with decompression.
4. Place less-frequently-used code in a second Flash bank (if available).

**What would you do if...** you received a Flash overflow error on a project that previously built successfully? Run `arm-none-eabi-size -A firmware.elf` to see the size breakdown by section. Use `arm-none-eabi-nm --size-sort firmware.elf | tail -30` to find the largest symbols and identify what grew. Common culprits: linking in stdio/printf unnecessarily, a large constant table added to `.rodata`, or debug symbols being emitted into the binary.

---

### Question F3
**What is the `.rodata` section and why should read-only constants be placed there rather than in `.data`?**

**Answer:**

`.rodata` (read-only data) holds:
- String literals: `"Hello, world\n"`
- `const`-qualified global variables: `const uint8_t lut[256] = {...};`
- Compiler-generated constants: jump tables for `switch` statements, vtables (C++)

**Why use `.rodata` instead of `.data`:**

1. **No SRAM consumption:** `.rodata` has LMA == VMA (both in Flash). No bytes need to be copied to SRAM at startup and no SRAM is allocated. A 4 KB lookup table in `.rodata` uses only 4 KB of Flash and 0 bytes of SRAM. The same table in `.data` would use 4 KB of Flash (for the init image) *plus* 4 KB of SRAM.

2. **MPU write-protection:** The MPU can be configured to make the Flash region containing `.rodata` read-only, so any accidental write to a constant causes a MemManage fault rather than silent data corruption.

3. **Code clarity:** `const` signals to the reader that the data never changes. The compiler may also exploit constness for optimisation (e.g., constant propagation).

**Common mistake:** Declaring `const` arrays with dynamic content:

```c
const uint32_t *ptr = &some_variable;  /* ptr is const (pointer is constant)
                                           but *ptr is NOT -- this goes to .data */
uint32_t * const ptr2 = &buf[0];       /* ptr2 is const, *ptr2 is not -- .data */
const uint32_t val = 42;               /* value is const -- .rodata */
```

---

### Question F4
**What is the `ENTRY()` directive in a linker script? Is it required for correct embedded firmware execution?**

**Answer:**

`ENTRY(Reset_Handler)` tells the linker which symbol is the logical entry point of the program. It serves two purposes:

1. **Debugger/GDB:** When you connect a debugger and use `run` or `load`, the debugger loads the program and sets PC to the entry point symbol. Without `ENTRY()`, some debuggers default to the symbol `_start` or the first byte of the Flash image.

2. **ELF `e_entry` field:** The ELF header contains an entry point address. `ENTRY()` populates this field. Bootloaders and flash loaders that parse the ELF file may use this to determine the jump target.

**Is it required for correct execution?** No, not on a standard Cortex-M target. The CPU hardware always reads the reset vector (index 1 in the vector table at address 0x00000000) independently of any ELF metadata. The program will execute correctly whether or not `ENTRY()` is present, because the CPU does not read the ELF header -- it reads memory.

`ENTRY()` is important for tooling correctness but is not a hard requirement for the firmware to boot.

---

## Tier 2 -- Intermediate

### Question I1
**Explain the purpose of `ALIGN(4)` and `. = ALIGN(8)` directives in a linker script. What happens if they are omitted and what is the `ALIGN(8)` requirement for the stack specifically?**

**Answer:**

The linker's location counter (`.`) advances as sections are placed. Without alignment, a section could start at an odd address -- for example, if `.text` ends at 0x08001003, the next section without alignment would start at 0x08001003.

**`ALIGN(4)`:** Rounds the location counter up to the next 4-byte boundary. This ensures:
- Cortex-M can fetch 32-bit instructions and data with aligned word accesses (unaligned access on Cortex-M0/M0+ causes a hard fault; on M3/M4/M7 it works but incurs an extra bus cycle).
- The startup copy loop can use 32-bit (`uint32_t`) transfers instead of byte-by-byte copies, which is faster and produces cleaner assembly.

**`ALIGN(8)` for the stack:**

The ARM Procedure Call Standard (AAPCS) requires the stack pointer to be 8-byte aligned at all public function call boundaries. If the stack starts at a 4-byte but not 8-byte aligned address, functions that use 64-bit (`double`, `long long`, NEON vectors) may receive misaligned data. On some architectures this triggers an alignment fault.

```
_estack must be a multiple of 8:
  Correct:   _estack = 0x20020000  (divisible by 8)
  Wrong:     _estack = 0x20020004  (only 4-byte aligned)
```

**Consequence of omitting ALIGN directives:**

The `.data` startup copy loop assumes 4-byte alignment of both `_sdata` and `_sidata`. If either is misaligned:

```c
uint32_t *src = &_sidata;  /* if misaligned, this is UB in strict-aliasing C */
uint32_t *dst = &_sdata;   /* Cortex-M0: unaligned word read -> HardFault */
*dst++ = *src++;            /* Cortex-M3: works but 2 bus cycles instead of 1 */
```

---

### Question I2
**What is scatter loading in an ARM toolchain context? How does it differ from using a GNU ld linker script with `AT>` syntax?**

**Answer:**

**GNU ld / GCC toolchain:** Uses linker scripts (`.ld` files). The `AT>` syntax in a section description specifies the LMA (load address in Flash) separately from the VMA (runtime address in SRAM):

```
.data : { ... } >SRAM AT>FLASH
```

**ARM Compiler 6 / armclang toolchain:** Uses scatter files (`.sct` files). The concept is identical -- separate LMA and VMA -- but the syntax is different:

```
; ARM scatter file syntax
LOAD_REGION 0x08000000 0x00100000     ; LMA region: Flash, 1MB
{
    EXEC_REGION 0x08000000 0x00100000 ; VMA same as LMA for code
    {
        *(+RO)    ; all read-only sections (.text, .rodata)
    }

    EXEC_SRAM 0x20000000 0x00020000   ; VMA: SRAM, 128KB (LMA implied from Flash)
    {
        *(+RW)    ; .data (initialised read-write)
        *(+ZI)    ; .bss (zero-initialised)
    }
}
```

**Key differences:**

| Feature | GNU ld script | ARM scatter file |
|---|---|---|
| Syntax | C-like, `SECTIONS {}` blocks | INI-like, load/exec regions |
| Section names | Standard (`+RO`, `+RW`, `+ZI` not used) | Can use ARM selectors like `+RO`, `+RW`, `+ZI` |
| Startup code generation | Manual (developer writes copy loop) | Automatic: `__scatter_copy` called by `__main` |
| Toolchain | GCC, Clang (arm-none-eabi) | ARM Compiler 5/6 (armcc/armclang) |
| Flexibility | Lower-level, highly configurable | Higher-level, more abstracted |

Both approaches accomplish the same thing: placing code in Flash and arranging for read-write data to be copied to SRAM at startup. The ARM scatter loader calls `__scatter_copy` (provided by the C library) rather than requiring the developer to write a manual copy loop.

---

### Question I3
**How would you place a specific function in a dedicated Flash section to support loading it into SRAM for execution? Walk through the linker script changes and the required C attribute.**

**Answer:**

Executing code from SRAM (instead of Flash) is required when:
- Flash has high-latency wait states at the operating frequency.
- Code modifies Flash (a write or erase routine cannot execute from the same Flash bank it is writing).
- Interrupt handlers need deterministic (low-jitter) execution time unaffected by Flash access latency.

**Step 1 -- Mark the function with a section attribute in C:**

```c
/* Place this function in the .ramfunc section */
__attribute__((section(".ramfunc"), noinline))
void flash_erase_sector(uint32_t sector)
{
    /* ... register manipulation to erase Flash sector ... */
}
```

**Step 2 -- Add the section to the linker script:**

```
/* In the linker script, after .data section */

/* Load address (LMA) in Flash, runtime address (VMA) in SRAM */
_siramfunc = LOADADDR(.ramfunc);
.ramfunc :
{
    . = ALIGN(4);
    _sramfunc = .;         /* VMA start in SRAM */
    *(.ramfunc .ramfunc.*) /* collect all .ramfunc sections */
    . = ALIGN(4);
    _eramfunc = .;         /* VMA end in SRAM */
} >SRAM AT>FLASH

```

**Step 3 -- Copy in the startup code (same pattern as `.data`):**

```c
/* In Reset_Handler, after copying .data: */
uint32_t *src = &_siramfunc;
uint32_t *dst = &_sramfunc;
while (dst < &_eramfunc) {
    *dst++ = *src++;
}
```

**Step 4 -- Verify the function actually executes from SRAM:**

```
arm-none-eabi-nm firmware.elf | grep flash_erase_sector
```

The address should be in the SRAM range (0x20000000+), not in the Flash range.

**Additional note:** On Cortex-M7 with I-cache, after copying the function to SRAM, invalidate the I-cache if there is any possibility the cache holds stale entries for those SRAM addresses (e.g., if this is not the first boot).

---

### Question I4
**A firmware image must be built so that it can run from two different Flash base addresses (0x08000000 for production, 0x08040000 for a secondary application slot). How do you build position-independent code for Cortex-M, and what are the limitations?**

**Answer:**

**Option 1: Position-Independent Code (PIC) with `-fPIC` / `-fPIE`**

GCC supports `-fPIC` (position-independent code) which uses PC-relative addressing for all code and a Global Offset Table (GOT) for data references. Cortex-M Thumb-2 supports PC-relative loads over a useful range, making PIC feasible.

Limitations on Cortex-M:
- The GOT must be initialised at runtime with the actual base address, requiring a runtime relocator.
- Code size increases (extra indirection through GOT for every global variable access).
- Function pointers stored in the vector table are absolute addresses; the startup code must patch the vector table with the actual run-time base address.
- Most bare-metal embedded toolchains (newlib) do not provide a runtime relocator.

**Option 2: Build twice with different base addresses**

The simpler production approach: build the same source twice, once with `--defsym FLASH_ORIGIN=0x08000000` and once with `--defsym FLASH_ORIGIN=0x08040000`, passed to the linker script:

```
MEMORY
{
    FLASH (rx) : ORIGIN = FLASH_ORIGIN, LENGTH = 256K
    SRAM  (rwx): ORIGIN = 0x20000000,   LENGTH = 128K
}
```

The linker resolves all absolute addresses using the specified origin. Each build produces a different binary with hardcoded addresses.

**Option 3: VTOR-relative dispatch with known offset**

If the two slots are always at a fixed offset from each other, and the code does not use absolute branch targets outside its own image (no HAL with hardcoded addresses), then a single binary can be linked at one base and have its vector table patched by the bootloader using VTOR. All Thumb branches within the image are PC-relative anyway and require no patching.

**Practical recommendation:** For most embedded bootloader/application designs, Option 2 (build for each slot address) is the most reliable and toolchain-agnostic approach.

---

## Tier 3 -- Advanced

### Question A1
**Explain how GNU ld's `--gc-sections` flag works in conjunction with `KEEP()` in a linker script. Why is this combination essential for embedded firmware, and what are the failure modes if either half is missing?**

**Answer:**

`--gc-sections` (passed to the linker via `-Wl,--gc-sections` in GCC) enables section garbage collection: the linker traces reachability from the entry point and removes any input sections that are not reachable. Each C function is placed in its own `.text.function_name` subsection by `-ffunction-sections`, and each variable in its own `.data.var_name` by `-fdata-sections`. Unreachable subsections are discarded.

**Why essential for embedded:** A large HAL or BSP library may provide hundreds of functions. Without `--gc-sections`, all of them are linked into the binary regardless of use, wasting Flash. With `--gc-sections`, only reachable functions appear in the final binary.

**The `KEEP()` exception:**

Some sections are critical but not referenced by any reachable C code:
- `.isr_vector`: the vector table is not called from C, only by hardware.
- `.eh_frame`, `.ARM.exidx`: C++ exception tables, used by the stack unwinder.
- `.noinit`: sections explicitly preserved across resets.

Without `KEEP()`, `--gc-sections` removes these sections silently:

```
KEEP(*(.isr_vector))   /* retain vector table even though no C code references it */
KEEP(*(.eh_frame))     /* retain C++ exception tables */
```

**Failure mode -- `--gc-sections` without `KEEP()` on `.isr_vector`:**

The vector table is removed from the Flash image. The linker produces no error (the table is simply absent). The binary programs to Flash without the vector table. On reset, the CPU reads zeros from address 0x00000000: SP = 0, PC = 0 (actually PC = 0 & ~1 = 0). The CPU executes from address 0x00000000, which is uninitialised memory, and immediately hard faults. This looks like a blank or corrupted device -- hard to diagnose without reading back the Flash image and examining it.

**Failure mode -- `KEEP()` without `--gc-sections`:**

`KEEP()` has no effect when garbage collection is not enabled. The linker includes all sections regardless. The binary is larger than necessary, but functionally correct.

---

### Question A2
**Walk through the exact memory layout for a Cortex-M4 device with 256 KB Flash and 64 KB SRAM, with the following firmware:**
**- 180 KB of `.text` and `.rodata`**
**- 2 KB of `.data` (initialised globals)**
**- 4 KB of `.bss` (zero-initialised globals)**
**- 8 KB stack**
**Identify where each section lives in Flash and SRAM, and calculate remaining free space.**

**Answer:**

```
FLASH layout (origin 0x08000000, 256 KB = 262144 bytes):
+---------------------------+ 0x08000000
| .isr_vector (assume 512 B)|   512 bytes
+---------------------------+ 0x08000200
| .text                     |
| .rodata                   | 180224 bytes (180 KB)
|    (combined)             |
+---------------------------+ 0x08002E00  (0x08000000 + 512 + 180224 = 0x0802CE00 approx)
```

Let me be precise:

```
Offset  Section             Size       End Address
------  -------             ----       -----------
0x0000  .isr_vector (KEEP)  512 B      0x0200
0x0200  .text + .rodata     180224 B   (180224 + 512 = 180736 = 0x2C200) -> 0x0002C200
0x2C200 .data init values   2048 B     0x0002CA00

Total Flash used: 512 + 180224 + 2048 = 182784 bytes (178.5 KB)
Flash remaining: 262144 - 182784 = 79360 bytes (77.5 KB)
```

```
SRAM layout (origin 0x20000000, 64 KB = 65536 bytes):
+---------------------------+ 0x20000000
| .data (runtime copy)      | 2048 bytes (2 KB)
+---------------------------+ 0x20000800
| .bss (zeroed at startup)  | 4096 bytes (4 KB)
+---------------------------+ 0x20001800
| (free heap, if any)       |
+---------------------------+ 0x20008000  <- _estack (top of stack)
| .stack (grows downward)   | 8192 bytes (8 KB)
+---------------------------+ 0x20006000  <- _sstack (bottom of stack)

Wait -- recalculate:
  .data  starts at 0x20000000, ends at 0x20000800 (2 KB)
  .bss   starts at 0x20000800, ends at 0x20001800 (4 KB)
  Stack  8 KB at the top of SRAM: 0x20010000 - 0x20008000 wait, 64 KB SRAM ends at:
  SRAM end = 0x20000000 + 0x10000 = 0x20010000

  Stack at top: _estack = 0x20010000, _sstack = 0x2000E000 (8 KB below top)

  .data:   0x20000000 -- 0x20000800   (2 KB)
  .bss:    0x20000800 -- 0x20001800   (4 KB)
  free:    0x20001800 -- 0x2000E000   (50176 bytes = 49 KB heap/free)
  .stack:  0x2000E000 -- 0x20010000   (8 KB, grows downward from 0x20010000)
```

**Summary table:**

| Region | Start | End | Size | Free remaining |
|---|---|---|---|---|
| Flash total | 0x08000000 | 0x08040000 | 256 KB | 77.5 KB |
| -- .isr_vector | 0x08000000 | 0x08000200 | 512 B | |
| -- .text+.rodata | 0x08000200 | 0x0802C200 | 180 KB | |
| -- .data (init) | 0x0802C200 | 0x0802CA00 | 2 KB | |
| SRAM total | 0x20000000 | 0x20010000 | 64 KB | 49 KB heap |
| -- .data | 0x20000000 | 0x20000800 | 2 KB | |
| -- .bss | 0x20000800 | 0x20001800 | 4 KB | |
| -- heap (free) | 0x20001800 | 0x2000E000 | 49 KB | |
| -- stack | 0x2000E000 | 0x20010000 | 8 KB | |

**What would you do if...** the `.bss` section unexpectedly grew by 20 KB? Use `arm-none-eabi-nm --size-sort firmware.elf | grep " B "` to list all `.bss` symbols by size. Common causes: a large statically-allocated buffer (`static uint8_t buf[20480]`), a large uninitialised global array, or accidentally including a full stdio buffer (newlib's `_impure_ptr` struct).

---

### Question A3
**You need to preserve certain variables across a software reset (warm reset) so that the device can distinguish between a power-on reset and a watchdog-triggered reset, and carry state across. How do you implement a `.noinit` section?**

**Answer:**

A `.noinit` section is a region of SRAM that is explicitly excluded from the startup zero-initialisation loop, allowing variables in it to survive a reset with their values intact (subject to the RAM retaining power, which it does across a software or watchdog reset but not across a full power cycle).

**Step 1 -- Add `.noinit` to the linker script:**

```
.noinit (NOLOAD) :
{
    . = ALIGN(4);
    _snoinit = .;
    *(.noinit .noinit.*)
    . = ALIGN(4);
    _enoinit = .;
} >SRAM
```

The `(NOLOAD)` attribute tells the linker that this section has no initialisation image and should never be loaded or written by the runtime. It is purely a VMA reservation.

**Step 2 -- Declare variables using the section attribute:**

```c
/* Magic number to detect valid data after reset */
#define RESET_MAGIC  0xDEADBEEFU

__attribute__((section(".noinit")))
static volatile uint32_t reset_magic;

__attribute__((section(".noinit")))
static volatile uint32_t reset_reason;

/* Reset cause codes */
#define RESET_CAUSE_POR      0x00000001U   /* power-on reset */
#define RESET_CAUSE_WATCHDOG 0x00000002U   /* watchdog timeout */
#define RESET_CAUSE_SOFTWARE 0x00000003U   /* software-triggered reset */
```

**Step 3 -- Check at startup (before `.bss` is zeroed):**

The check must happen in the Reset_Handler *before* the `.bss` zero loop, because if `reset_magic` were in `.bss` it would be cleared. Being in `.noinit`, it retains its value.

```c
void Reset_Handler(void)
{
    /* Check for warm reset before any zeroing */
    uint32_t is_warm = (reset_magic == RESET_MAGIC);

    if (!is_warm) {
        reset_reason = RESET_CAUSE_POR;  /* first power-on */
        reset_magic  = RESET_MAGIC;
    }
    /* If is_warm, reset_reason still holds the value set before the reset */

    /* Now proceed with .data copy and .bss zero */
    /* ... */

    main();
}
```

**Step 4 -- Set the reset cause before triggering a reset:**

```c
void trigger_software_reset(void)
{
    reset_reason = RESET_CAUSE_SOFTWARE;
    /* Ensure the write completes before the reset fires */
    __DSB();
    NVIC_SystemReset();   /* writes SCB->AIRCR to trigger soft reset */
}
```

**Important caveats:**
- On power-on, SRAM content is undefined (random). The magic number check will almost certainly fail on power-on (1-in-2^32 chance of false positive). This is intentional.
- The `.noinit` region must not overlap with `.bss`. The linker script ordering ensures this.
- After a hard power cycle (e.g., battery removal), `reset_magic` will not equal `RESET_MAGIC`, so the code correctly identifies a power-on reset.
