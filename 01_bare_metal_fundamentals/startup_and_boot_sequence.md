# Startup and Boot Sequence

## Prerequisites
- Basic ARM Cortex-M architecture: registers, program counter, stack pointer
- C memory model: stack, heap, global variables
- Linker concepts: sections, symbols, scatter loading

---

## Concept Reference

### What Happens Between Power-On and `main()`

The path from reset to the first line of application code involves four distinct phases:

```
Power-on / reset assertion
        |
        v
1. Reset vector fetch
   - CPU reads the vector table from address 0x00000000 (or remapped address)
   - Entry 0: initial stack pointer value
   - Entry 1: reset handler address (bit[0] = 1 for Thumb mode)
        |
        v
2. Reset handler executes (startup code in assembly or C)
   - Optionally copy .data section from Flash to SRAM
   - Zero-fill .bss section
   - Optionally call SystemInit() (clock/PLL setup, cache enable)
   - Call main()
        |
        v
3. C runtime initialisation (if using C++ or libc constructors)
   - Call global constructors (__libc_init_array or equivalent)
        |
        v
4. main() begins
```

### The Vector Table

On Cortex-M, the vector table is an array of 32-bit words located at the base of the memory map (default: 0x00000000, remapped from Flash). Each entry is the address of an exception/interrupt handler, with the two special entries at the start:

```
Offset  Entry
0x0000  Initial Stack Pointer value  (loaded directly into SP on reset)
0x0004  Reset Handler address        (PC jumps here; bit[0] must be 1 for Thumb)
0x0008  NMI Handler address
0x000C  HardFault Handler address
0x0010  MemManage Handler address    (MPU fault)
0x0014  BusFault Handler address
0x0018  UsageFault Handler address
0x001C  Reserved (x4)
0x002C  SVC Handler address
0x0030  DebugMon Handler address
0x0034  Reserved
0x0038  PendSV Handler address
0x003C  SysTick Handler address
0x0040  IRQ0 Handler address         (vendor-specific from here)
...
```

The CPU hardware reads entries 0 and 1 directly out of the bus fabric on reset -- this happens before any software runs.

### Stack Pointer Initialisation

Before any C code can execute, the stack pointer (SP, r13) must point to a valid region of SRAM. The Cortex-M hardware initialises SP automatically from vector table entry 0.

```
Typical linker script symbol:
  _estack = 0x20010000;   /* top of SRAM -- stack grows downward */

Vector table in C:
  const uint32_t __isr_vector[] __attribute__((section(".isr_vector"))) = {
      (uint32_t)&_estack,         /* entry 0: initial SP */
      (uint32_t)&Reset_Handler,   /* entry 1: reset handler */
      (uint32_t)&NMI_Handler,
      /* ... */
  };
```

The stack pointer is initialised to `_estack` (top of SRAM) before the reset handler's first instruction executes. This is why the `Reset_Handler` itself can use a stack -- it does not need to initialise SP manually.

### The Reset Handler

A minimal Cortex-M reset handler performs three jobs before calling `main()`:

```c
/* Symbols provided by the linker script */
extern uint32_t _sidata;  /* start of .data initialisation values in Flash */
extern uint32_t _sdata;   /* start of .data section in SRAM */
extern uint32_t _edata;   /* end of .data section in SRAM */
extern uint32_t _sbss;    /* start of .bss section */
extern uint32_t _ebss;    /* end of .bss section */

void Reset_Handler(void)
{
    /* 1. Copy initialised data from Flash to SRAM */
    uint32_t *src = &_sidata;
    uint32_t *dst = &_sdata;
    while (dst < &_edata) {
        *dst++ = *src++;
    }

    /* 2. Zero-fill the BSS section */
    dst = &_sbss;
    while (dst < &_ebss) {
        *dst++ = 0U;
    }

    /* 3. (Optional) SystemInit -- clock trees, PLLs, cache, MPU */
    SystemInit();

    /* 4. Call main (which should never return) */
    main();

    /* Defensive: if main returns, spin forever */
    while (1) { }
}
```

### Vector Table Relocation (VTOR)

After startup, software can move the vector table to a different address (typically SRAM) using the Vector Table Offset Register (VTOR). This is essential for:

- Bootloaders that jump to application firmware at a different Flash address.
- RAM-resident vector tables that allow ISR function pointers to be changed at runtime.

```c
/* Cortex-M3/M4/M7: VTOR is at address 0xE000ED08 */
#define SCB_VTOR  (*(volatile uint32_t *)0xE000ED08U)

/* Relocate vector table to application firmware at 0x08020000 */
SCB_VTOR = 0x08020000U;

/* Or to a RAM copy at 0x20000000 */
memcpy((void *)0x20000000, (void *)0x08000000, VECTOR_TABLE_SIZE);
SCB_VTOR = 0x20000000U;
```

Alignment constraint: the vector table base address must be aligned to the next power of two greater than or equal to the total vector table size. For a device with 64 external IRQs the table is (16 + 64) * 4 = 320 bytes, so the base must be aligned to 512 bytes (0x200).

---

## Tier 1 -- Fundamentals

### Question F1
**What are the first two entries in the ARM Cortex-M vector table, and why are they special compared to all other entries?**

**Answer:**

The first two entries are handled entirely by hardware on reset, before any software instruction executes:

- **Entry 0 (offset 0x0000):** The initial value to be loaded directly into the Main Stack Pointer (MSP). This is not a code address but a data value -- the top of the initial stack region. The CPU reads this value on reset and sets SP = this value before fetching any instruction.

- **Entry 1 (offset 0x0004):** The address of the Reset_Handler. The CPU reads this after loading SP, sets PC to this address (with bit[0] set to 1 to indicate Thumb mode), and execution begins.

All other vector table entries are addresses of exception and interrupt handlers. They are fetched by hardware when the corresponding exception occurs, not at startup. Entry 0 is unique in being a stack pointer value rather than a code address.

**Common mistake:** Placing an instruction address in entry 0, or placing a data value without setting bit[0] in entry 1. A reset handler address with bit[0] clear will trigger a UsageFault on processors that require Thumb-only code.

---

### Question F2
**Why must the `.data` section be copied from Flash to SRAM during startup? Why can the `.bss` section simply be zeroed rather than copied?**

**Answer:**

**`.data` section (initialised global/static variables):**

Variables like `int counter = 5;` have an initial value that must be non-zero. The compiler places the *initial values* (LMA: Load Memory Address) in Flash as part of the binary image, and defines a *runtime address* (VMA: Virtual Memory Address) in SRAM where the variable will live during execution. The startup code copies the Flash image of `.data` to the SRAM address.

If this copy is skipped, the variable's SRAM address contains random power-on data, not `5`.

**`.bss` section (zero-initialised global/static variables):**

Variables like `int flag;` or `static int count = 0;` are required by the C standard to be initialised to zero at program startup. Rather than storing a block of zeros in Flash (wasting Flash space), the linker places these symbols in `.bss` with no stored image. The startup code simply zeroes the entire `.bss` region.

**Memory layout illustration:**

```
Flash image:                  SRAM at runtime:
+------------------+          +------------------+
| .text (code)     |          | .data (RW)       | <- copied from Flash LMA
+------------------+          +------------------+
| .rodata          |          | .bss (RW)        | <- zeroed by startup
+------------------+          +------------------+
| .data init vals  | ---copy->| stack (grows dn) |
+------------------+          +------------------+
```

**What would you do if...** you observed a global variable with an initialiser not having the correct value when `main()` starts? First suspect the `.data` copy in the reset handler -- either `_sidata`, `_sdata`, or `_edata` are incorrect linker symbols, or the copy loop has an off-by-one error.

---

### Question F3
**What is the VTOR register and when would a bootloader need to use it?**

**Answer:**

The Vector Table Offset Register (VTOR, at 0xE000ED08 in the System Control Block) holds the base address of the active vector table. At reset its value is 0, so the hardware reads the vector table from address 0x00000000 (which is remapped or aliased to Flash on most devices).

**Bootloader use case:**

A bootloader lives at the start of Flash (e.g., 0x08000000 on STM32). Application firmware is programmed starting at a higher Flash address (e.g., 0x08020000). When the bootloader wants to hand off execution to the application, it must:

1. Set VTOR to the application's vector table address so that any future exceptions are handled by the application's handlers, not the bootloader's.
2. Load the application's initial stack pointer from `*(uint32_t *)0x08020000`.
3. Load the application's reset handler address from `*(uint32_t *)0x08020004`.
4. Jump to the application reset handler.

```c
void jump_to_application(uint32_t app_flash_addr)
{
    typedef void (*app_entry_t)(void);

    /* Pointer to the application's vector table */
    uint32_t *app_vt = (uint32_t *)app_flash_addr;

    /* Relocate the vector table */
    SCB->VTOR = app_flash_addr;

    /* Set stack pointer to application's initial SP value */
    __set_MSP(app_vt[0]);

    /* Jump to application's Reset_Handler */
    app_entry_t app_entry = (app_entry_t)(app_vt[1]);
    app_entry();
}
```

**Important:** The CMSIS `__set_MSP()` intrinsic and the jump must not use the stack between them, because the stack pointer has just been changed. This is usually done in a short assembly stub or an inline function that the compiler cannot re-order.

---

### Question F4
**What is the purpose of `SystemInit()` and when is it called in the startup sequence?**

**Answer:**

`SystemInit()` is a CMSIS-defined function (implemented by the silicon vendor) called from the Reset_Handler *before* `main()`, typically after the `.data` and `.bss` initialisations are complete but before any C constructors run.

Its responsibilities vary by device but typically include:

1. **PLL and clock setup:** Configure the system clock source (HSI, HSE, PLL), set PLL multipliers/dividers, switch the CPU to run at the target frequency.
2. **Flash latency configuration:** Set the correct number of flash wait states for the target clock speed (e.g., 5 wait states for a 216 MHz Cortex-M7 on 3.3V).
3. **Cache and branch predictor enable:** On Cortex-M7, enable the instruction and data caches and the branch predictor for maximum performance.
4. **FPU enable:** On Cortex-M4/M7, enable the Floating Point Unit by setting CPACR[23:20] before any floating-point instructions run.
5. **MPU configuration:** Set up the Memory Protection Unit regions if required before any potentially unsafe code executes.

**Timing of the call:**

```
Reset_Handler:
  1. Copy .data        <- must happen before SystemInit reads/writes globals
  2. Zero .bss         <- must happen before SystemInit reads/writes globals
  3. Call SystemInit() <- can now use globals; configures hardware
  4. Call main()
```

**What would you do if...** your application appeared to run much slower than expected despite correct code? Check `SystemInit()` -- the PLL may have not locked (check the lock-ready bit before switching the clock source), or flash wait states may not be set before increasing the CPU clock frequency (causing instruction fetch errors or hard faults).

---

## Tier 2 -- Intermediate

### Question I1
**Explain the concept of LMA (Load Memory Address) versus VMA (Virtual Memory Address) in the context of the `.data` section. How does the linker encode both addresses in the output file?**

**Answer:**

Every section in a linked executable has two addresses:

- **LMA (Load Memory Address):** Where the section's raw bytes are stored in the binary image -- in Flash for an embedded system.
- **VMA (Virtual Memory Address):** Where the section's symbols will be accessed at runtime -- in SRAM for read/write data.

For `.text` and `.rodata`, LMA == VMA because code and read-only constants are accessed directly from Flash (or where they are loaded).

For `.data`, LMA != VMA. The linker records:
- The VMA as the addresses of all global variable symbols (e.g., `counter` at 0x20000000).
- The LMA as the location in the Flash image where the initial byte values are stored (e.g., at 0x08010000 after `.text` and `.rodata`).

The startup copy loop uses these linker-provided symbols:

```
_sidata (LMA start of .data initialisation image in Flash)
_sdata  (VMA start of .data region in SRAM)
_edata  (VMA end of .data region in SRAM)

Copy loop: for each word from _sdata to _edata, read from _sidata and write to VMA.
```

**GNU ld linker script syntax showing LMA/VMA distinction:**

```
.data : AT(_sidata)          /* AT() specifies the LMA */
{
    _sdata = .;              /* VMA start (dot is current VMA) */
    *(.data .data.*)
    _edata = .;              /* VMA end */
} >SRAM                      /* VMA region: SRAM */

_sidata = LOADADDR(.data);   /* = LMA start (after .text and .rodata in Flash) */
```

**What would you do if...** an embedded debugger showed global variables with correct values immediately after the reset handler ran the copy loop, but they appeared corrupted an instant later? Check for stack overflow: the stack may be growing down into the `.data` region, overwriting variable values. Examine the SRAM layout in the linker script and add a stack overflow canary.

---

### Question I2
**Describe how a minimal Cortex-M vector table is declared in C. What attributes are required and why?**

**Answer:**

```c
/*
 * Vector table for STM32F4 (Cortex-M4)
 * Placed in the .isr_vector section, which the linker script maps to the
 * start of Flash (0x08000000, aliased to 0x00000000 at reset).
 */

/* External handler declarations */
extern void Reset_Handler(void);
extern void NMI_Handler(void);
extern void HardFault_Handler(void);
extern void MemManage_Handler(void);
extern void BusFault_Handler(void);
extern void UsageFault_Handler(void);
extern void SVC_Handler(void);
extern void DebugMon_Handler(void);
extern void PendSV_Handler(void);
extern void SysTick_Handler(void);
/* Peripheral IRQ handlers ... */
extern void TIM2_IRQHandler(void);

/* Top of stack provided by linker script */
extern uint32_t _estack;

/* The vector table itself */
__attribute__((section(".isr_vector")))       /* place in named linker section */
__attribute__((used))                          /* prevent LTO from eliminating it */
const uint32_t __isr_vector[] = {
    (uint32_t)&_estack,          /* 0x0000: initial SP */
    (uint32_t)&Reset_Handler,    /* 0x0004: reset handler */
    (uint32_t)&NMI_Handler,      /* 0x0008: NMI */
    (uint32_t)&HardFault_Handler,/* 0x000C: hard fault */
    (uint32_t)&MemManage_Handler,/* 0x0010: MPU fault */
    (uint32_t)&BusFault_Handler, /* 0x0014: bus fault */
    (uint32_t)&UsageFault_Handler,/* 0x0018: usage fault */
    0, 0, 0, 0,                  /* 0x001C-0x002B: reserved */
    (uint32_t)&SVC_Handler,      /* 0x002C: SVCall */
    (uint32_t)&DebugMon_Handler, /* 0x0030: debug monitor */
    0,                           /* 0x0034: reserved */
    (uint32_t)&PendSV_Handler,   /* 0x0038: PendSV */
    (uint32_t)&SysTick_Handler,  /* 0x003C: SysTick */
    /* IRQ0 onwards */
    (uint32_t)&TIM2_IRQHandler,  /* example vendor IRQ */
};
```

**Required attributes explained:**

| Attribute | Reason |
|-----------|--------|
| `section(".isr_vector")` | Linker script maps this section to the start of Flash. Without it, the array could be placed anywhere in `.rodata` and would not be found by hardware at address 0x0. |
| `used` | Prevents Link-Time Optimisation (LTO) from discarding the array. Since no C code ever *calls* `__isr_vector`, without this attribute LTO may determine it is unreachable and remove it, resulting in no vector table in Flash. |
| `const` | Marks the array read-only, placing it in `.rodata` which the linker then relocates into `.isr_vector`. Without `const` it would go into `.data` and require copying to SRAM -- the vector table would be in SRAM, not Flash. |

---

### Question I3
**A Cortex-M device uses memory remapping to alias Flash at address 0x00000000 at reset. What exactly happens in the first few bus cycles after reset, and when does the alias disappear?**

**Answer:**

On many Cortex-M devices (e.g., STM32 with BOOT pins), the memory controller maps the selected boot source (Flash, SRAM, or system memory) to address 0x00000000 immediately on reset. The CPU then:

```
Cycle 0 (reset release):
  Hardware drives a bus transaction to address 0x00000000 to fetch the
  initial MSP value. The bus fabric decodes 0x00000000 as the aliased
  Flash and returns the 32-bit word at Flash[0x08000000] (= _estack value).
  The CPU loads this value into MSP.

Cycle 1:
  Hardware drives a bus transaction to address 0x00000004 to fetch the
  reset vector. Returns Flash[0x08000004] (= Reset_Handler address | 1).
  The CPU loads this into PC. Execution begins at Reset_Handler.

Cycle 2+:
  The CPU fetches instructions from the Reset_Handler's VMA address
  (which is in the 0x08000000 range). The alias at 0x00000000 is no longer
  needed for normal execution -- it remains in place until software
  changes the SYSCFG_MEMRMP register (if remapping is configurable).
```

The alias persists at 0x00000000 even after the reset handler completes. If the vector table is never relocated via VTOR, the hardware continues to use the alias for exception vector fetches. Once VTOR is written with a non-zero value (by the application or a bootloader), the hardware uses that address directly and the alias at 0x00000000 is no longer involved in exception dispatch.

**Implication for DMA and scatter-gather:** If a DMA descriptor inadvertently uses address 0x00000000, it will read from Flash (via the alias) rather than generating a bus error. This can cause subtle bugs where DMA appears to work but produces constant incorrect data matching the vector table content.

---

### Question I4
**What is the purpose of the `__attribute__((naked))` on a reset handler, and what are the risks of using it?**

**Answer:**

A `naked` function tells the compiler to generate *no* prologue or epilogue code -- no `PUSH {r4-r11, lr}` on entry, no `POP` on exit, no stack frame setup. The function body is entirely at the programmer's discretion.

**Why some startup code uses it:**

The Reset_Handler is the very first function called after reset. Its entry condition is guaranteed: the stack pointer has just been loaded from the vector table, the stack is valid but empty. There is no caller to return to, and no registers to preserve. A `naked` handler:

1. Avoids a redundant `PUSH/POP` that wastes cycles at startup.
2. Ensures the function can be written in inline assembly without the compiler adding instructions around the assembly blocks.
3. Prevents the compiler from assuming `lr` is a valid return address and generating an incorrect epilogue.

**Risks:**

```c
/* DANGEROUS -- do not do this in a naked function */
__attribute__((naked)) void Reset_Handler(void)
{
    int local = 5;           /* ERROR: compiler may generate stack access
                                before SP is properly set up */
    SystemInit();            /* function call -- requires stack for return address */
}
```

In a `naked` function, the compiler will not generate the stack adjustment needed before a function call or local variable access. If inline assembly does not manually save/restore registers or set up the frame, the function call will corrupt SP or LR.

**Safe pattern:**

```c
__attribute__((naked)) void Reset_Handler(void)
{
    __asm volatile (
        "ldr r0, =_estack      \n"
        "mov sp, r0            \n"   /* set SP explicitly if not done by vector table */
        "bl  startup_c_init    \n"   /* jump to C function that does .data/.bss init */
        "bl  main              \n"
        "b   .                 \n"   /* infinite loop if main returns */
    );
}
```

For most Cortex-M targets using standard vector tables (hardware-loaded SP), the `naked` attribute is unnecessary on the reset handler and introduces more risk than it eliminates.

---

## Tier 3 -- Advanced

### Question A1
**Describe the complete sequence an RTOS bootloader must perform when jumping from itself to an application image, including all safety checks and the use of VTOR. What can go wrong if any step is omitted?**

**Answer:**

A production-quality jump-to-application sequence:

```c
typedef void (*func_ptr_t)(void);

typedef enum {
    JUMP_OK = 0,
    JUMP_ERR_BAD_SP,
    JUMP_ERR_BAD_PC,
    JUMP_ERR_BAD_ALIGN,
} jump_status_t;

#define SRAM_START  0x20000000U
#define SRAM_END    0x20020000U
#define FLASH_START 0x08000000U
#define FLASH_END   0x08100000U

jump_status_t jump_to_app(uint32_t app_base)
{
    uint32_t *vt = (uint32_t *)app_base;

    /* Step 1: Validate the vector table base alignment */
    /* VTOR must be aligned to a power of two >= vector table size */
    if (app_base & 0x1FFU) {              /* at least 512-byte aligned */
        return JUMP_ERR_BAD_ALIGN;
    }

    /* Step 2: Validate the initial stack pointer */
    uint32_t app_sp = vt[0];
    if (app_sp < SRAM_START || app_sp > SRAM_END) {
        return JUMP_ERR_BAD_SP;           /* SP not in SRAM -- image corrupt/absent */
    }

    /* Step 3: Validate the reset handler address */
    uint32_t app_pc = vt[1];
    if ((app_pc & 1U) == 0U) {            /* Thumb mode bit must be set */
        return JUMP_ERR_BAD_PC;
    }
    uint32_t app_pc_real = app_pc & ~1U;
    if (app_pc_real < FLASH_START || app_pc_real >= FLASH_END) {
        return JUMP_ERR_BAD_PC;           /* reset handler not in Flash */
    }

    /* Step 4: Disable all peripherals and IRQs the bootloader enabled */
    /* Failing to do this leaves interrupt handlers pointing into bootloader code */
    __disable_irq();
    /* Clear all pending and enabled IRQs */
    for (int i = 0; i < 8; i++) {
        NVIC->ICER[i] = 0xFFFFFFFFU;     /* disable all */
        NVIC->ICPR[i] = 0xFFFFFFFFU;     /* clear all pending */
    }
    SysTick->CTRL = 0;                    /* disable SysTick */

    /* Step 5: Disable caches if enabled (Cortex-M7) */
    /* Caches must be cleaned and disabled before changing clock/VTOR,
       to avoid stale instruction fetches after VTOR relocation */
    SCB_DisableDCache();
    SCB_DisableICache();

    /* Step 6: Relocate VTOR to application's vector table */
    SCB->VTOR = app_base;
    __DSB();   /* ensure VTOR write is committed before SP/PC change */
    __ISB();   /* flush pipeline of any speculative fetches */

    /* Step 7: Set the application's stack pointer */
    __set_MSP(app_sp);

    /* Step 8: Jump -- no return */
    func_ptr_t app_reset = (func_ptr_t)app_pc;
    app_reset();

    /* Unreachable */
    return JUMP_OK;
}
```

**What goes wrong if each step is omitted:**

| Omitted step | Failure mode |
|---|---|
| VTOR alignment check | VTOR written with unaligned address; hardware reads vector table from wrong location; first exception causes hard fault |
| SP validation | CPU loads garbage SP; first stack push (function call, interrupt) corrupts memory at arbitrary address |
| PC Thumb bit check | UsageFault on first exception; hard fault if UsageFault is not enabled |
| IRQ disable / clear | Bootloader interrupt fires in application context; PC jumps into bootloader ISR; application crashes or hangs |
| Cache flush/disable | Cortex-M7 I-cache may hold stale bootloader instructions at addresses the application now owns; application runs corrupted instructions |
| DSB/ISB after VTOR | Speculative instruction fetches may have started using old VTOR value; new interrupt takes vector from bootloader table |
| MSP set | Application startup code assumes it has a fresh stack; bootloader's stack frame is still present; stack corruption |

---

### Question A2
**On a Cortex-M7 with an instruction cache, why is it necessary to perform a cache invalidation after writing new firmware to Flash and before executing it? What specific cache maintenance operations must be performed, and in what order?**

**Answer:**

The Cortex-M7 I-cache holds copies of recently fetched instruction words. If firmware is written to Flash (for example, by a bootloader performing an OTA update), the physical Flash contents change but the cache may still hold the old instruction bytes for those addresses.

When execution jumps to the new firmware, the cache may return old (pre-update) instructions instead of the newly written ones -- a silent failure mode that looks identical to a partially-flashed device.

**Required operations and order:**

```c
/* After writing new firmware to Flash, before jumping to it: */

/* 1. DSB -- ensure all Flash write operations have completed and are
        visible to all observers on the bus. Without this, the cache
        invalidation might run while write buffers are still draining. */
__DSB();

/* 2. Invalidate the entire instruction cache.
        This discards all cached lines, forcing re-fetches from Flash.
        The SCB_InvalidateICache() CMSIS function writes to ICIALLU. */
SCB_InvalidateICache();

/* 3. ISB -- flush the processor's instruction pipeline.
        After invalidating the cache, the pipeline may already contain
        instructions that were speculatively fetched from old cache lines.
        ISB ensures the pipeline is flushed; all instructions after the
        ISB are fetched fresh from the (now cache-miss, Flash-backed) cache. */
__ISB();

/* Now safe to jump to new firmware */
```

**For data caches (D-cache) when Flash-resident data is used:**

If the application image contains a `.rodata` section that the D-cache might have pre-loaded (e.g., during the Flash write verification read), the D-cache must also be cleaned and invalidated:

```c
SCB_CleanInvalidateDCache();
__DSB();
```

**Why order matters:**

- DSB before SCB_InvalidateICache: ensures the Flash write is complete before the invalidation runs. If writes are still in-flight during invalidation, the subsequent re-fetch might still get old data (implementation-dependent but best avoided).
- ISB after SCB_InvalidateICache: required by the ARMv7-M architecture specification. Without the ISB, the processor is allowed to use cached (now invalidated) branch predictor entries or prefetched instructions.

---

### Question A3
**A colleague proposes initialising the `.bss` section using `memset()` inside the Reset_Handler. What is wrong with this approach, and how should it be done correctly?**

**Answer:**

`memset()` is a C library function. Calling it requires:

1. A valid, initialised stack pointer (for the function call frame and local variables inside `memset`).
2. The C library itself to be available and correctly linked.

On Cortex-M, the stack pointer is set from the vector table entry, so (1) is satisfied. However, there is a subtler issue:

**The real problem -- `memset` may be in `.text`, which may depend on `.data`:**

Some C library implementations (particularly newlib-nano for embedded targets) have `memset` in `.text` and operate without any global state, so calling it before `.data` is copied is safe. But other library configurations use global variables inside `memset` (e.g., for SIMD optimisation dispatch, or internal state). If those globals have not yet been initialised (`.data` not yet copied), calling `memset` to clear `.bss` will itself read uninitialised memory.

**The dependency chain:**

```
You want: memset(_sbss, 0, bss_size)
But:      memset may use globals in .data
And:      .data has not been copied yet
Result:   memset runs with corrupt globals -- undefined behaviour
```

**Correct approach:** Use a bare loop with a pointer or use compiler-provided `.bss` zero-initialisation before any library calls:

```c
/* Safe -- no library dependency, no globals used */
uint32_t *bss = (uint32_t *)&_sbss;
while (bss < (uint32_t *)&_ebss) {
    *bss++ = 0U;
}
```

This hand-written loop uses only registers (loop variable in a register, no stack-allocated locals, no global state), making it safe to call before any other initialisation.

**When is `memset` safe?** After `.data` has been copied and `.bss` has been zeroed, `memset` is entirely safe for use in application code. The startup sequence is the one context where bare loops must be used instead of library functions.
