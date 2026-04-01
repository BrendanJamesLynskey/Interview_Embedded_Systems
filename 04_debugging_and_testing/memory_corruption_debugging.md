# Memory Corruption Debugging

## Prerequisites
- C memory model: stack, heap, static/global storage
- ARM Cortex-M MPU basics: regions, attributes, privilege levels
- Understanding of pointers and pointer arithmetic in C

---

## Concept Reference

### Memory Corruption Categories

```
Type                  Description                          Typical Symptom
--------------------  -----------------------------------  --------------------------
Stack overflow        Stack grows into adjacent memory     HardFault, corrupted locals
Heap corruption       malloc/free misuse, off-by-one       Crash on next malloc/free
Buffer overflow       Write past end of fixed array        Corrupted adjacent variables
Use-after-free        Pointer to freed heap block used     Random data, crash later
Double free           free() called twice on same block    Heap metadata destroyed
Null dereference      Read/write via NULL pointer          HardFault at address 0x00
Uninitialised read    Variable used before assignment      Random behaviour
Wild pointer          Pointer with garbage address used    HardFault, random memory
Stack underflow       Function returns with corrupt SP     HardFault in prologue/epilogue
```

---

### Stack Architecture on Cortex-M

```
SRAM layout (typical):
  +---------------------+  <- _estack (top of SRAM, e.g., 0x20010000)
  |                     |
  |  Main Stack (MSP)   |  grows downward (push = decrement SP then write)
  |                     |
  |         |           |
  |         v           |  <-- current SP during normal execution
  |                     |
  |  [STACK GUARD ZONE] |  ideally protected by MPU (raises fault before overflow)
  |                     |
  +---------------------+  <- bottom of stack / top of heap
  |                     |
  |  Heap               |  grows upward (brk increases)
  |         ^           |
  |         |           |
  |                     |
  +---------------------+  <- _end (end of .bss, start of heap)
  |  .bss (zeroed)      |
  +---------------------+
  |  .data (init'd)     |
  +---------------------+  <- 0x20000000 (SRAM base)

Stack overflow: SP decrements past bottom of stack into heap or .bss.
  Without guard zone: corruption is silent until a future access to
  the overwritten area triggers unexpected behaviour.
```

**RTOS task stacks:**

In an RTOS, each task has its own private stack, typically allocated from a heap or statically declared. The RTOS scheduler saves/restores SP on context switch. Stack overflow in task A can corrupt task B's stack (adjacent in memory), causing task B to crash at an unrelated later time.

```c
/* FreeRTOS task creation with explicit stack size */
xTaskCreate(vTaskFunction,     /* function */
            "TaskName",        /* name */
            256,               /* stack depth in WORDS (not bytes) */
            NULL,              /* parameter */
            tskIDLE_PRIORITY,  /* priority */
            &xTaskHandle);

/* 256 words = 1024 bytes. If task uses more, overflow corrupts adjacent task's
   stack silently unless the MPU or stack watermarking detects it. */
```

---

### Stack Canary

A stack canary is a known sentinel value written at the bottom of the stack (or at a specific location within a stack frame) that is checked before the function returns or before task switching. If the value has changed, overflow has occurred.

```c
/* Compiler-inserted canary (GCC -fstack-protector-strong):
   GCC automatically inserts canary checks in functions with local arrays. */

void vulnerable_function(const char *input) {
    char buffer[64];            /* local buffer on stack */
    /* GCC inserts:
       uint32_t canary = __stack_chk_guard;  (random value set at startup)
       [function body]
       if (canary != __stack_chk_guard) __stack_chk_fail(); */
    strcpy(buffer, input);      /* potential overflow */
}

/* __stack_chk_fail() is called on canary mismatch.
   In embedded: implement this to log an error and reset. */
void __stack_chk_fail(void) {
    /* Log fault information */
    fault_log_write("Stack canary failed at PC=0x%08X", __builtin_return_address(0));
    NVIC_SystemReset();  /* or hang for debugger attach */
}
```

**Manual canary for RTOS tasks (FreeRTOS stack watermark):**

```c
/* FreeRTOS fills each task stack with 0xA5A5A5A5 at creation.
   Watermark check: find the highest 0xA5A5A5A5 pattern still intact. */
UBaseType_t uxHighWaterMark;
uxHighWaterMark = uxTaskGetStackHighWaterMark(xTaskHandle);
/* uxHighWaterMark = number of words never written (remaining headroom)
   If this reaches 0, the task stack has overflowed. */

/* Check all tasks periodically: */
void vCheckStackUsage(void) {
    TaskStatus_t task_status[MAX_TASKS];
    UBaseType_t n_tasks = uxTaskGetSystemState(task_status, MAX_TASKS, NULL);
    for (UBaseType_t i = 0; i < n_tasks; i++) {
        if (task_status[i].usStackHighWaterMark < STACK_WARNING_THRESHOLD_WORDS) {
            log_warning("Task '%s' stack headroom: %u words",
                        task_status[i].pcTaskName,
                        task_status[i].usStackHighWaterMark);
        }
    }
}
```

---

### MPU Configuration for Memory Protection

The Memory Protection Unit (MPU) is a hardware block in Cortex-M3 and above that defines up to 8 (or 16 on Cortex-M33) memory regions with independent access permissions. Attempted access violations generate a MemManage fault.

```c
/*
 * Example: Protect a 32-byte stack guard zone to detect stack overflow.
 *
 * Stack layout:
 *   [task stack top] ... [guard zone 32 bytes] ... [task stack base]
 *
 * MPU region configured as no-access. When stack overflows into the guard,
 * the first store generates a MemManageFault instead of silent corruption.
 */

#include "core_cm4.h"  /* CMSIS header */

void mpu_configure_stack_guard(uint32_t guard_base_addr) {
    /* Disable MPU during configuration */
    MPU->CTRL = 0;

    /* Region 0: stack guard -- no access (read or write causes fault) */
    MPU->RNR  = 0;                          /* select region 0 */
    MPU->RBAR = (guard_base_addr & 0xFFFFFFE0U) /* base address (32-byte aligned) */
              | MPU_RBAR_VALID_Msk           /* address update valid */
              | 0;                           /* region number embedded in RBAR */

    MPU->RASR = MPU_RASR_ENABLE_Msk         /* enable this region */
              | (0x4 << MPU_RASR_SIZE_Pos)   /* size = 2^(4+1) = 32 bytes */
              | (0x0 << MPU_RASR_AP_Pos)     /* AP=000: no access at any privilege level */
              | MPU_RASR_XN_Msk;             /* execute never */

    /* Region 1: normal SRAM -- full access (must be configured; MPU is deny-by-default) */
    MPU->RNR  = 1;
    MPU->RBAR = 0x20000000U | MPU_RBAR_VALID_Msk | 1;
    MPU->RASR = MPU_RASR_ENABLE_Msk
              | (0x11 << MPU_RASR_SIZE_Pos)  /* size = 2^(17+1) = 256 KB */
              | (0x3 << MPU_RASR_AP_Pos)     /* AP=011: full access priv and unpriv */
              | (0x1 << MPU_RASR_TEX_Pos)    /* TEX=001, C=1, B=1: normal, WB cache */
              | MPU_RASR_C_Msk
              | MPU_RASR_B_Msk;

    /* Enable MPU with default map for privileged access, enable in HardFault/NMI */
    MPU->CTRL = MPU_CTRL_ENABLE_Msk
              | MPU_CTRL_PRIVDEFENA_Msk;

    /* Ensure changes take effect before next memory access */
    __DSB();
    __ISB();
}

/* MemManage fault handler -- called when guard zone is accessed */
void MemManage_Handler(void) {
    uint32_t mmfsr = SCB->CFSR & 0xFF;  /* MemManage Fault Status Register */

    if (mmfsr & (1 << 1)) {
        /* DACCVIOL: data access violation */
        uint32_t fault_addr = SCB->MMFAR;  /* address that caused the fault */
        fault_log("MemManage: stack overflow detected, fault addr=0x%08X", fault_addr);
    }
    NVIC_SystemReset();
}
```

---

### Heap Corruption

The heap manager maintains a linked list of free blocks (and sometimes allocated blocks) using metadata stored adjacent to the user data. Overflowing a heap allocation corrupts this metadata, causing the allocator to crash or behave incorrectly on the next `malloc`/`free` call — often far from the actual corruption site.

```
Heap block layout (typical embedded allocator, e.g., FreeRTOS heap_4):

  +------------------+------------------+-------------------+
  | BlockLink_t prev | BlockLink_t next | xBlockSize | MAGIC |  <- metadata
  +------------------+------------------+-------------------+
  |                                                         |
  |   User data (returned pointer points here)             |
  |                                                         |
  +------------------+------------------+-------------------+
  | BlockLink_t prev | BlockLink_t next | xBlockSize | MAGIC |  <- next block header
  ...

Off-by-one write:   memcpy(ptr, src, sizeof(buffer) + 1)
                    The +1 byte overwrites the first byte of the next block's metadata.
                    Effect: next pvPortFree() or pvPortMalloc() traverses corrupted list
                            => accesses garbage address => HardFault.
```

**Heap corruption detection techniques:**

```c
/* Technique 1: Magic number in block header (if allocator supports it) */
/* FreeRTOS heap_5 with configHEAP_POISON defined writes 0xAA to freed memory.
   Reading 0xAA from a freed block later indicates use-after-free. */

/* Technique 2: Canary after each allocation (custom wrapper) */
#define HEAP_CANARY 0xDEADC0DEU

void *safe_malloc(size_t size) {
    /* Allocate extra 4 bytes for trailing canary */
    uint8_t *raw = pvPortMalloc(size + sizeof(uint32_t));
    if (raw == NULL) return NULL;
    /* Write canary after user data */
    *(uint32_t *)(raw + size) = HEAP_CANARY;
    return raw;
}

void safe_free(void *ptr, size_t size) {
    uint8_t *raw = (uint8_t *)ptr;
    uint32_t canary = *(uint32_t *)(raw + size);
    if (canary != HEAP_CANARY) {
        /* Canary was overwritten: buffer overflow detected */
        fault_log("Heap canary failed: ptr=%p, expected=0x%08X, got=0x%08X",
                  ptr, HEAP_CANARY, canary);
        NVIC_SystemReset();
    }
    vPortFree(raw);
}

/* Limitation: requires calling code to pass the original allocation size to free().
   Suitable for embedded systems; not practical in general-purpose code. */
```

---

### Buffer Overflow Patterns

```c
/* Pattern 1: Classic off-by-one (fence post error) */
char message[16];
/* Loop condition uses <= instead of <: writes 17 bytes into 16-byte array */
for (int i = 0; i <= 16; i++) {
    message[i] = 'A';   /* message[16] is PAST the end of the array */
}

/* Pattern 2: Unchecked user input */
void process_command(const char *input) {
    char local_buffer[32];
    strcpy(local_buffer, input);  /* DANGEROUS: no length check */
    /* If input > 31 chars + null terminator, stack frame is overwritten */
}

/* Pattern 3: Integer width mismatch */
void process_packet(uint8_t len, const uint8_t *data) {
    uint8_t buffer[256];
    /* If len is declared uint8_t, it can be 0-255, matching buffer size.
       But if the caller passes a value derived from a uint16_t or int, and
       only the low byte is passed, the high bits are silently dropped. */
    memcpy(buffer, data, len);  /* safe if len truly fits in buffer */
}

/* Correct approach: always validate length before memcpy */
void process_packet_safe(size_t len, const uint8_t *data) {
    uint8_t buffer[256];
    if (len > sizeof(buffer)) {
        log_error("Packet too large: %zu bytes", len);
        return;
    }
    memcpy(buffer, data, len);
}
```

---

## Tier 1 — Fundamentals

### Question F1
**What is a stack overflow and what are its symptoms on a Cortex-M microcontroller?**

**Answer:**

A stack overflow occurs when the stack pointer (SP) decrements past the allocated stack region into memory that belongs to something else: heap, BSS, data, or another task's stack in an RTOS.

**How it happens:**

```
Each function call:
  1. Pushes LR (return address) and callee-saved registers onto the stack
  2. Decrements SP by the size of the local variable frame

Deep call chains + large local arrays = large stack consumption.
Example: a chain of 10 functions each with a 100-byte local array = 1 KB + register saves.
```

**Symptoms (in order of detection):**

1. **Silent data corruption:** The overflowed stack writes into adjacent memory (heap or global variables). Variables that "should not" change mysteriously change. This is the hardest phase to diagnose because the cause is distant from the symptom.

2. **HardFault on return:** When an overflowed stack frame corrupts the saved LR (return address), the function return jumps to an invalid address, causing a HardFault or UsageFault (invalid instruction fetch).

3. **HardFault in ISR:** If the stack overflows during a nested interrupt, the exception entry mechanism itself can fail. Pushing the exception frame to an invalid SP location generates a MemManage or BusFault.

4. **Immediate MemManageFault (with MPU guard):** If an MPU no-access region is placed at the stack bottom, the first write past the boundary immediately generates a MemManageFault, catching the overflow at the point it occurs rather than at the point it manifests.

**Common mistake:** Candidates describe stack overflow as always causing an immediate crash. In practice, the most common first symptom is silent data corruption — the crash occurs later, in unrelated code, often after the original overflowing function has returned.

---

### Question F2
**What is a null pointer dereference? How does the Cortex-M hardware respond to it, and how do you locate it in your firmware?**

**Answer:**

A null pointer dereference occurs when a pointer variable holds the value 0x00000000 (NULL) and the program reads from or writes to it.

```c
/* Common causes: */

/* 1. Failed malloc return not checked */
char *buffer = malloc(256);
/* Missing: if (buffer == NULL) { handle_error(); return; } */
buffer[0] = 'A';    /* dereference NULL if malloc failed */

/* 2. Uninitialised pointer */
uint32_t *p;        /* unintialised -- undefined value, often 0 on Cortex-M BSS init */
*p = 0x12345678;    /* write through garbage pointer */

/* 3. Array of function pointers, missing initialisation */
typedef void (*handler_t)(void);
handler_t handlers[8];  /* in BSS, zeroed to NULL */
handlers[event_id]();   /* if handler not registered, calls NULL */
```

**Cortex-M hardware response:**

```
Address 0x00000000 on Cortex-M maps to the vector table (Flash or SRAM).
A load from 0x00000000 reads the initial stack pointer value -- this will
not cause a fault but returns meaningless data.

A store to 0x00000000 attempts to write to Flash (read-only) or to the
vector table in SRAM. If the address is not writable, a BusFault or
HardFault is generated (BFARVALID set, SCB->BFAR = 0x00000000).

With MPU configured to protect address 0:
  Define an MPU region at 0x00000000, 32 bytes, no-access.
  Any read or write via NULL pointer immediately raises MemManageFault.
  MMAR register holds 0x00000000 confirming the cause.
```

**Locating the fault:**

```
1. Inspect SCB->CFSR on fault entry:
   - DACCVIOL (bit 1) set => data access violation
   - MMARVALID (bit 7) set => SCB->MMFAR holds the faulting address

2. Capture the stacked PC (return address pushed by exception mechanism):
   void HardFault_Handler(void) {
       uint32_t *stacked = (uint32_t *)__get_MSP(); /* or PSP */
       uint32_t pc  = stacked[6];  /* PC pushed at offset 0x18 */
       uint32_t lr  = stacked[5];  /* LR at offset 0x14 */
       /* Load pc into objdump or GDB: */
       /* arm-none-eabi-addr2line -e firmware.elf 0x<pc value> */
   }

3. In GDB, run 'info registers' immediately after fault to see all register
   values including the LR chain that shows the call path.
```

---

### Question F3
**What is a use-after-free bug? Why is it particularly dangerous in embedded systems, and how can you detect it?**

**Answer:**

Use-after-free occurs when a pointer is used to read from or write to memory after that memory has been returned to the heap via `free()`.

```c
/* Example use-after-free */
typedef struct {
    uint32_t id;
    char name[16];
    void (*callback)(void);
} Device_t;

Device_t *dev = malloc(sizeof(Device_t));
dev->id = 42;
dev->callback = &my_handler;

/* Some code path frees the device */
free(dev);

/* Later -- dev pointer still held in a global or another struct */
dev->callback();    /* use-after-free: calls whatever is at the freed memory */
```

**Why dangerous in embedded systems:**

1. **No virtual memory protection:** Desktop OS systems often place freed memory in a separate pool and trigger a segfault on access. Embedded systems without MMU do not isolate freed memory. The allocator immediately makes the block available for reuse.

2. **Deterministic memory reuse:** Embedded allocators are often simple and deterministic. A freed block may be almost immediately reused by the next `malloc`. The use-after-free then corrupts the new allocation, not just reads garbage.

3. **Function pointer hijacking:** If the freed struct contained a function pointer (as in the example above), the use-after-free calls whatever value is now at that address — potentially code injected by an attacker or a different valid function, leading to arbitrary code execution.

**Detection techniques:**

```c
/* Technique 1: Poison freed memory */
void safe_free(void **ptr) {
    if (*ptr != NULL) {
        /* Overwrite with recognisable pattern before freeing */
        memset(*ptr, 0xFE, allocation_size);  /* requires size tracking */
        free(*ptr);
        *ptr = NULL;  /* CRITICAL: null the caller's pointer */
    }
}

/* Use macro to enforce nulling at call site: */
#define SAFE_FREE(p)  do { free(p); (p) = NULL; } while (0)

/* Technique 2: Compiler sanitisers (for host-based unit tests) */
/* Compile with -fsanitize=address on host:
   AddressSanitizer (ASan) tracks every allocation/free and
   intercepts use-after-free at the exact access point. */

/* Technique 3: Custom allocator with magic number */
/* Set freed block header to DEAD_MAGIC (e.g., 0xDEADDEAD).
   On malloc, verify header is FREE_MAGIC (e.g., 0xFEEDFEED).
   If block is used after free, the magic number read back will be DEAD_MAGIC. */
```

---

## Tier 2 — Intermediate

### Question I1
**Walk through how you would configure the Cortex-M MPU to detect a heap buffer overflow in an embedded system running without an RTOS.**

**Answer:**

The strategy is to place a no-access MPU region immediately after each allocated buffer. This converts a silent overflow into an immediate MemManageFault at the exact faulting instruction.

The limitation is that the MPU has only 8 regions total, so you cannot guard every allocation — but you can guard the specific allocation under investigation.

```c
#include "core_cm4.h"

/*
 * Layout goal:
 *   [normal SRAM -- full access] [MPU guard -- no access] [heap continues]
 *
 * Steps:
 *   1. Allocate the buffer under investigation.
 *   2. Ensure the allocation is followed by a known gap (or pad it).
 *   3. Configure an MPU no-access region covering that gap.
 *   4. On overflow: MemManageFault fires immediately; MMFAR = address past buffer.
 */

/* Round up to next power-of-two boundary (MPU region size must be power of 2) */
static uint32_t next_pow2(uint32_t x) {
    uint32_t p = 32;  /* minimum MPU region size */
    while (p < x) p <<= 1;
    return p;
}

/* Configure a guard region immediately after a heap allocation */
void mpu_set_heap_guard(uint8_t region_num, void *alloc_ptr, size_t alloc_size) {
    /* Guard starts immediately after the allocation */
    uint32_t guard_addr = ((uint32_t)alloc_ptr + alloc_size + 31U) & ~31U; /* 32-byte align */

    MPU->CTRL = 0;  /* disable MPU while modifying */

    MPU->RNR  = region_num;
    MPU->RBAR = (guard_addr & MPU_RBAR_ADDR_Msk)
              | MPU_RBAR_VALID_Msk
              | region_num;

    MPU->RASR = MPU_RASR_ENABLE_Msk
              | (0x4 << MPU_RASR_SIZE_Pos)   /* 32 bytes: size field = log2(32)-1 = 4 */
              | (0x0 << MPU_RASR_AP_Pos)     /* no access */
              | MPU_RASR_XN_Msk;

    MPU->CTRL = MPU_CTRL_ENABLE_Msk | MPU_CTRL_PRIVDEFENA_Msk;
    __DSB();
    __ISB();
}

/* Fault handler: report overflow details */
void MemManage_Handler(void) {
    volatile uint32_t cfsr  = SCB->CFSR;
    volatile uint32_t mmfar = SCB->MMFAR;

    if (cfsr & SCB_CFSR_MMARVALID_Msk) {
        /* mmfar holds the address that was accessed beyond the buffer */
        uint32_t stacked_pc = ((uint32_t *)__get_MSP())[6];
        fault_log("Heap overflow: wrote to 0x%08X from PC=0x%08X", mmfar, stacked_pc);
    }
    /* Clear fault status */
    SCB->CFSR = cfsr;
    NVIC_SystemReset();
}
```

**Limitations to mention:**

- Minimum MPU region size is 32 bytes, so a 1-byte overflow that falls within the 32-byte granularity of the allocation may not be caught immediately.
- MPU regions must be power-of-two sized and naturally aligned.
- Only 8 regions total; one is needed for SRAM general access, leaving 7 for guards.
- Disable and reconfigure after each test allocation.

---

### Question I2
**A firmware build compiled without `-fstack-protector` has a buffer overflow in a network-facing function that processes incoming packets. Describe the chain of events from the overflow to the crash, and how you would determine the root cause after the fact.**

**Answer:**

**Step 1 — The overflow:**

```c
/* Vulnerable function (abbreviated) */
void parse_udp_payload(uint8_t *payload, uint16_t length) {
    char name_buffer[32];     /* on stack, offset 0 from frame base */
    uint32_t checksum;        /* on stack, offset 32 */
    uint32_t saved_r4;        /* pushed by compiler ABI */
    uint32_t saved_lr;        /* return address */

    memcpy(name_buffer, payload, length);  /* length not bounded */

    /*
     * Stack frame (growing downward, lower address = deeper stack):
     *
     *   [saved_lr     ] <- SP at function entry was here; now at +24 offset
     *   [saved_r4     ]
     *   [checksum     ]
     *   [name_buffer  ] <- memcpy writes here first, continues upward
     *
     * If length = 48:
     *   Bytes 0-31:  fill name_buffer normally
     *   Bytes 32-35: overwrite checksum variable
     *   Bytes 36-39: overwrite saved_r4
     *   Bytes 40-43: overwrite saved_lr  <-- return address corrupted
     */
}
```

**Step 2 — Silent execution continues:**

The function returns normally (or appears to). The overflow is not detected. The corrupted `saved_lr` sits in the stack frame until `parse_udp_payload` returns.

**Step 3 — Crash on return:**

When the function executes `BX LR`, LR contains the corrupted value (bytes 40-43 of the attacker-controlled payload). The CPU branches to that address. If the address is:
- Unaligned or not a valid Thumb address (bit[0] not set): UsageFault
- In unmapped memory: BusFault or HardFault
- In a valid code region: code at that address executes (code reuse attack)

**Step 4 — Post-crash diagnosis:**

```
1. Recover the Cortex-M fault registers from a persistent memory area.
   Best practice: write fault state to backup SRAM or Flash in HardFault_Handler
   before resetting.

   Registers to save:
     SCB->HFSR   (HardFault Status Register)
     SCB->CFSR   (Configurable Fault Status: UFSR/BFSR/MMFSR)
     SCB->BFAR   (Bus Fault Address Register)
     Stacked PC  (the address that faulted)
     Stacked LR  (the return address pushed BEFORE the bad branch)

2. Decode stacked PC using arm-none-eabi-addr2line:
   arm-none-eabi-addr2line -e firmware.elf -f -p 0x<stacked_PC>

   If PC is 0x12345678 (attacker-controlled), it will not map to a known symbol.
   This itself is diagnostic: "PC maps to no known function" = branch through
   corrupted return address = buffer overflow.

3. Decode stacked LR to find where the corrupted function was called from:
   arm-none-eabi-addr2line -e firmware.elf -f -p 0x<stacked_LR>
   => "parse_udp_payload called from udp_receive_handler at udp.c:143"

4. Review udp.c:143 -- examine the call site and the vulnerable function.
   The combination of (a) no length check and (b) stack-resident buffer confirms
   the overflow vector.

5. To confirm the exact payload that triggered the overflow:
   Enable packet logging before the crash (circular buffer in SRAM).
   Review the last N packets received before reset.
```

---

## Tier 3 — Advanced

### Question A1
**Describe how you would implement a complete memory safety strategy for a safety-critical embedded firmware, covering both compile-time and runtime defences. Address stack, heap, and peripheral memory regions.**

**Answer:**

A layered defence strategy is required; no single mechanism is sufficient.

**Layer 1 — Compile-time hardening:**

```makefile
# GCC options for embedded safety builds
CFLAGS += -fstack-protector-strong   # canary checks on vulnerable functions
CFLAGS += -Wall -Wextra -Werror      # treat warnings as errors
CFLAGS += -Wformat -Wformat-security  # check printf format strings
CFLAGS += -D_FORTIFY_SOURCE=2        # glibc bounds checking (if using newlib)
CFLAGS += -fsanitize=undefined       # UBSan: catches integer overflow, null deref
                                     # Note: UBSan adds code size; enable in debug only

# Link-time options
LDFLAGS += -Wl,--warn-section-align  # alert on alignment padding that may hide bugs
```

**Layer 2 — MPU configuration:**

```
Region 0: Null pointer trap (0x00000000, 32 bytes, no access)
Region 1: Flash / code (0x08000000, 512 KB, read+execute, no write)
Region 2: Peripheral bus (0x40000000, 512 MB, read+write, no execute, device memory)
Region 3: Normal SRAM (0x20000000, 192 KB, read+write, no execute)
Region 4: Stack guard for main stack (32 bytes at bottom of main stack, no access)
Region 5: Stack guard for task A (32 bytes at bottom of task A stack, no access)
Region 6: Stack guard for task B (32 bytes at bottom of task B stack, no access)
Region 7: (reserved / available for dynamic heap guard during debug)

Note: with 8 regions only 3 task stack guards are possible if all other regions
used. For systems with more tasks: use RTOS MPU task-switch hooks to reconfigure
the stack guard region to match the currently running task.
```

```c
/* FreeRTOS MPU port: define stack guard in task descriptor */
static StackType_t task_a_stack[256] __attribute__((aligned(256 * 4)));

static const TaskParameters_t task_a_params = {
    .pvTaskCode     = vTaskA,
    .pcName         = "TaskA",
    .usStackDepth   = 256,
    .pvParameters   = NULL,
    .uxPriority     = 2 | portPRIVILEGE_BIT,
    .puxStackBuffer = task_a_stack,
    .xRegions = {
        /* MPU regions specific to this task */
        { peripheral_regs, 0x1000, portMPU_REGION_READ_WRITE },
        { 0, 0, 0 },
        { 0, 0, 0 },
    }
};
xTaskCreateRestricted(&task_a_params, &xTaskAHandle);
```

**Layer 3 — Heap safety:**

```c
/* Use a fixed-pool allocator instead of dynamic heap in safety-critical code.
   Fixed pools eliminate fragmentation and overflow of heap metadata. */

#define POOL_BLOCK_SIZE  64U
#define POOL_NUM_BLOCKS  32U

typedef struct {
    uint8_t  data[POOL_BLOCK_SIZE];
    uint32_t magic;          /* ALLOC_MAGIC or FREE_MAGIC */
    bool     in_use;
} PoolBlock_t;

static PoolBlock_t pool[POOL_NUM_BLOCKS];

void *pool_alloc(void) {
    for (uint32_t i = 0; i < POOL_NUM_BLOCKS; i++) {
        if (!pool[i].in_use) {
            pool[i].in_use = true;
            pool[i].magic  = ALLOC_MAGIC;
            memset(pool[i].data, 0, POOL_BLOCK_SIZE);
            return pool[i].data;
        }
    }
    return NULL;  /* pool exhausted */
}

void pool_free(void *ptr) {
    PoolBlock_t *block = (PoolBlock_t *)((uint8_t *)ptr - offsetof(PoolBlock_t, data));
    ASSERT(block->magic == ALLOC_MAGIC);  /* detect double-free / invalid pointer */
    block->magic  = FREE_MAGIC;
    block->in_use = false;
    memset(block->data, 0xFE, POOL_BLOCK_SIZE);  /* poison freed memory */
}
```

**Layer 4 — Runtime assertion and watchdog:**

```c
/* Configurable assert that captures diagnostic information before reset */
#define ASSERT(cond)  \
    do { \
        if (!(cond)) { \
            fault_log("ASSERT failed: %s at %s:%d", #cond, __FILE__, __LINE__); \
            NVIC_SystemReset(); \
        } \
    } while (0)

/* Independent hardware watchdog: if any memory safety handler hangs,
   the watchdog resets the system within its timeout period */
void watchdog_init(uint32_t timeout_ms) {
    IWDG->KR  = 0x5555;        /* unlock */
    IWDG->PR  = 4;             /* prescaler /64 */
    IWDG->RLR = (timeout_ms * 40000) / (64 * 1000);  /* 40 kHz IWDG clock */
    IWDG->KR  = 0xAAAA;        /* reload */
    IWDG->KR  = 0xCCCC;        /* start */
}

void watchdog_kick(void) {
    IWDG->KR = 0xAAAA;
}
```

**Layer 5 — Continuous monitoring in background task:**

```c
void vMemorySafetyMonitor(void *params) {
    for (;;) {
        /* Check all task stack high water marks */
        vCheckStackUsage();

        /* Verify heap pool integrity: all magic numbers intact */
        vCheckPoolIntegrity();

        /* Verify key global variable canaries */
        vCheckGlobalCanaries();

        /* Kick watchdog only if all checks pass */
        watchdog_kick();

        vTaskDelay(pdMS_TO_TICKS(100));  /* run every 100 ms */
    }
}
```

**Interview insight:** The key point interviewers want to hear is that no single mechanism is sufficient. A stack canary catches frame overflows but not heap corruption. The MPU catches access violations but has limited granularity. Compile-time sanitisers help during development but cannot all be enabled in production. A complete strategy requires all layers working together.
