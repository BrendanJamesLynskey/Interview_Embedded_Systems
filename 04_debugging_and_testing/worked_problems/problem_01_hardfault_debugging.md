# Worked Problem 01: HardFault Debugging on Cortex-M

## Problem Statement

A Cortex-M4 firmware that has been running in production for six months starts occasionally crashing in the field. The symptom: the device resets unexpectedly, approximately once every 24-48 hours of operation, and always during high wireless activity. No crash has been reproduced in the lab. The firmware contains a default `HardFault_Handler` that simply loops forever:

```c
void HardFault_Handler(void) {
    while (1) {}
}
```

There is no core dump mechanism. The root cause is unknown. You have been asked to diagnose and fix it.

**What would you do?**

---

## Background: Cortex-M Fault Architecture

Before the solution, understand the available diagnostic information.

### Fault Status Registers

When a Cortex-M CPU takes a fault exception, it populates a set of status registers before entering the handler:

```
SCB->HFSR  (HardFault Status Register, 0xE000ED2C):
  Bit 31 - DEBUGEVT  : set if HardFault caused by debug event
  Bit 30 - FORCED    : set if a configurable fault was escalated to HardFault
                       (e.g., BusFault with FAULTMASK set, or fault in fault handler)
  Bit  1 - VECTTBL   : set if HardFault on vector table read (very early boot)

SCB->CFSR  (Configurable Fault Status Register, 0xE000ED28):
  [31:16] = UFSR (UsageFault Status):
    Bit 25 - DIVBYZERO   : divide by zero (if DIV_0_TRP set in CCR)
    Bit 24 - UNALIGNED   : unaligned access (if UNALIGN_TRP set in CCR)
    Bit 19 - NOCP        : attempt to use coprocessor (e.g., FPU) when disabled
    Bit 18 - INVPC       : invalid EXC_RETURN value (corrupt LR)
    Bit 17 - INVSTATE    : invalid CPU state (EPSR.T=0, attempt to execute)
    Bit 16 - UNDEFINSTR  : undefined instruction executed

  [15:8]  = BFSR (BusFault Status):
    Bit 15 - BFARVALID   : SCB->BFAR holds the fault address
    Bit 13 - LSPERR      : fault on lazy FPU state save
    Bit 12 - STKERR      : fault during exception entry stack push
    Bit 11 - UNSTKERR    : fault during exception return unstack
    Bit 10 - IMPRECISERR : imprecise data bus error (write buffer; address not captured)
    Bit  9 - PRECISERR   : precise data bus error (SCB->BFAR valid)
    Bit  8 - IBUSERR     : instruction fetch bus error

  [7:0]   = MMFSR (MemManage Fault Status):
    Bit  7 - MMARVALID   : SCB->MMFAR holds the fault address
    Bit  5 - MLSPERR     : MPU fault on lazy FPU state save
    Bit  4 - MSTKERR     : MPU violation during exception entry
    Bit  3 - MUNSTKERR   : MPU violation during exception return
    Bit  1 - DACCVIOL    : data access violation (MPU or XN region)
    Bit  0 - IACCVIOL    : instruction access violation

SCB->BFAR  (Bus Fault Address Register, 0xE000ED38):
  Valid only when BFSR.BFARVALID is set.
  Contains the address that caused the bus fault.

SCB->MMFAR (MemManage Fault Address Register, 0xE000ED34):
  Valid only when MMFSR.MMARVALID is set.
```

### Exception Stack Frame

When an exception fires, Cortex-M hardware automatically pushes a frame onto the stack before jumping to the handler. This frame contains the CPU state at the moment of the fault:

```
Stack grows downward. After exception entry, SP points to:

  SP + 0x00 : R0    (argument 0 at time of fault)
  SP + 0x04 : R1    (argument 1 at time of fault)
  SP + 0x08 : R2    (argument 2 at time of fault)
  SP + 0x0C : R3    (argument 3 at time of fault)
  SP + 0x10 : R12   (scratch register at time of fault)
  SP + 0x14 : LR    (return address of the function that faulted)
  SP + 0x18 : PC    (address of the instruction that caused the fault)
  SP + 0x1C : xPSR  (processor status, including interrupt number)

If FPU state was active (Cortex-M4F with lazy stacking):
  Additional 16 words (64 bytes) of FP registers may also be pushed.
```

---

## Solution: Step-by-Step

### Step 1 — Implement a Fault Information Handler

The immediate priority is to capture fault state before the next reset. A persistent memory region (backup SRAM, or the end of normal SRAM) stores the fault record across software resets.

```c
/* fault_handler.h */
#ifndef FAULT_HANDLER_H
#define FAULT_HANDLER_H

#include <stdint.h>

#define FAULT_MAGIC  0xDEADFACEU

typedef struct __attribute__((packed)) {
    uint32_t magic;     /* FAULT_MAGIC if this record is valid */
    uint32_t hfsr;      /* HardFault Status Register */
    uint32_t cfsr;      /* Configurable Fault Status Register */
    uint32_t mmfar;     /* MemManage Fault Address */
    uint32_t bfar;      /* Bus Fault Address */
    uint32_t r0;        /* Stacked registers from exception frame */
    uint32_t r1;
    uint32_t r2;
    uint32_t r3;
    uint32_t r12;
    uint32_t lr;        /* Link register (return address) at time of fault */
    uint32_t pc;        /* Program counter -- the faulting instruction */
    uint32_t xpsr;      /* Program status register */
    uint32_t sp;        /* Stack pointer at time of fault */
} FaultRecord_t;

/* Place fault record at a fixed, known address that survives soft reset.
   On STM32: use backup SRAM (0x40024000) or reserve the top of SRAM. */
#define FAULT_RECORD_ADDR  ((volatile FaultRecord_t *)0x20017F00U)

void fault_handler_init(void);     /* check for prior fault record on startup */
void fault_record_print(void);     /* output fault record via UART or ITM */

#endif /* FAULT_HANDLER_H */
```

```c
/* fault_handler.c */
#include "fault_handler.h"
#include "core_cm4.h"
#include <string.h>

/* Called from the real HardFault_Handler in assembly */
void HardFault_Handler_C(uint32_t *stacked_args, uint32_t lr_value) {
    volatile FaultRecord_t *rec = FAULT_RECORD_ADDR;

    rec->magic = FAULT_MAGIC;
    rec->hfsr  = SCB->HFSR;
    rec->cfsr  = SCB->CFSR;
    rec->mmfar = SCB->MMFAR;
    rec->bfar  = SCB->BFAR;

    /* Extract stacked exception frame */
    rec->r0   = stacked_args[0];
    rec->r1   = stacked_args[1];
    rec->r2   = stacked_args[2];
    rec->r3   = stacked_args[3];
    rec->r12  = stacked_args[4];
    rec->lr   = stacked_args[5];
    rec->pc   = stacked_args[6];
    rec->xpsr = stacked_args[7];
    rec->sp   = (uint32_t)stacked_args + 0x20U;  /* SP before exception entry */

    /* Optionally: try to send via UART here if it remains functional */

    /* Clear fault status registers (write-1-to-clear) */
    SCB->HFSR = SCB->HFSR;
    SCB->CFSR = SCB->CFSR;

    /* Reset -- do not loop forever */
    NVIC_SystemReset();
}

/* Assembly wrapper: determines which stack was active and passes to C handler */
__attribute__((naked)) void HardFault_Handler(void) {
    __asm volatile (
        /* Check bit 2 of LR (EXC_RETURN): 0 = MSP was active, 1 = PSP was active */
        "tst    lr, #4           \n"
        "ite    eq               \n"
        "mrseq  r0, msp          \n"   /* fault on MSP (main context) */
        "mrsne  r0, psp          \n"   /* fault on PSP (task context) */
        "mov    r1, lr           \n"   /* pass LR (EXC_RETURN) as second argument */
        "b      HardFault_Handler_C \n"
        ::: "r0", "r1"
    );
}

void fault_handler_init(void) {
    volatile FaultRecord_t *rec = FAULT_RECORD_ADDR;
    if (rec->magic == FAULT_MAGIC) {
        /* A fault record exists from a previous run -- print it */
        fault_record_print();
        /* Clear the record */
        rec->magic = 0;
    }
}

void fault_record_print(void) {
    volatile FaultRecord_t *rec = FAULT_RECORD_ADDR;
    /* Output via UART, ITM, or RTT -- implementation-specific */
    debug_printf("=== FAULT RECORD ===\r\n");
    debug_printf("HFSR  = 0x%08X\r\n", rec->hfsr);
    debug_printf("CFSR  = 0x%08X\r\n", rec->cfsr);
    debug_printf("BFAR  = 0x%08X\r\n", rec->bfar);
    debug_printf("MMFAR = 0x%08X\r\n", rec->mmfar);
    debug_printf("PC    = 0x%08X\r\n", rec->pc);
    debug_printf("LR    = 0x%08X\r\n", rec->lr);
    debug_printf("R0    = 0x%08X\r\n", rec->r0);
    debug_printf("SP    = 0x%08X\r\n", rec->sp);
    debug_printf("XPSR  = 0x%08X\r\n", rec->xpsr);
    debug_printf("====================\r\n");
}
```

Call `fault_handler_init()` early in `main()`, before any peripheral initialisation.

---

### Step 2 — Decode the Fault Record

After deploying the improved firmware and waiting for a crash, the fault record is read back on next boot:

```
=== FAULT RECORD ===
HFSR  = 0x40000000    <- FORCED bit set (configurable fault escalated to HardFault)
CFSR  = 0x00008200    <- bits 15 (BFARVALID) and 9 (PRECISERR) set in BFSR
BFAR  = 0x00000000    <- bus fault at address 0x00000000 (NULL dereference!)
MMFAR = 0x00000000
PC    = 0x08003A42    <- instruction that faulted
LR    = 0x08003A6D    <- caller of the faulting function (return address)
R0    = 0x00000000    <- first argument was NULL
SP    = 0x20009E80
XPSR  = 0x21000000    <- no active interrupt (bit 8 = 0, thread mode)
====================
```

**Decode the register values:**

```
HFSR = 0x40000000: FORCED bit set.
  A BusFault was escalated to HardFault because BUSFAULTENA in SHCSR was not set.
  (BusFault is disabled by default -- only HardFault is always enabled.)

CFSR = 0x00008200:
  BFSR = 0x82 = 0b10000010
    Bit 15 (BFARVALID) = 1 : BFAR contains the valid fault address
    Bit  9 (PRECISERR)  = 1 : precise data bus error

BFAR = 0x00000000: The instruction at PC=0x08003A42 attempted to
  read from or write to address 0x00000000. This is a NULL pointer dereference.

PC = 0x08003A42: the faulting instruction.
LR = 0x08003A6D: where execution will return to (the caller's code).
R0 = 0x00000000: at the time of the fault, R0 = 0x00000000.
  On Cortex-M, R0 is the first argument. The function was called with a NULL pointer.
```

---

### Step 3 — Map PC to Source Code

```bash
# Use addr2line to find which source line corresponds to PC = 0x08003A42
arm-none-eabi-addr2line -e firmware.elf -f -p 0x08003A42
# Output:
# wifi_process_rx_frame
# /home/dev/project/src/wifi_driver.c:347

# Find the caller from LR = 0x08003A6D
# LR in Thumb mode: actual call site = LR - 1 (bit[0] always set for Thumb)
arm-none-eabi-addr2line -e firmware.elf -f -p 0x08003A6C
# Output:
# wifi_task
# /home/dev/project/src/wifi_task.c:112
```

---

### Step 4 — Inspect the Source

```c
/* wifi_driver.c, around line 347 */
void wifi_process_rx_frame(const wifi_frame_t *frame) {
    /* Line 347: */
    uint32_t payload_len = frame->header.length;   /* <-- frame is NULL here */
    /* ... */
}

/* wifi_task.c, around line 112 */
void wifi_task(void *params) {
    wifi_frame_t *frame;
    for (;;) {
        /* Receive frame from queue */
        if (xQueueReceive(rx_queue, &frame, portMAX_DELAY) == pdTRUE) {
            wifi_process_rx_frame(frame);     /* line 112 */
        }
    }
}
```

**Root cause identified:** `xQueueReceive` copies the pointer value from the queue into `frame`. If another task had freed the frame and written NULL back to its pointer, and then the NULL was enqueued (via a different code path writing NULL to the queue), `frame` would be NULL when `wifi_process_rx_frame` dereferences it.

Search for all places where the RX queue is written:

```c
/* Found in wifi_isr.c: */
void wifi_rx_irq_handler(void) {
    wifi_frame_t *new_frame = wifi_allocate_frame();
    /* ... fill frame ... */
    if (xQueueSendFromISR(rx_queue, &new_frame, NULL) != pdPASS) {
        wifi_free_frame(new_frame);
        /* BUG: new_frame is now freed but we do not prevent it from being sent.
           Actually: the send failed, so it was NOT sent. But what if
           wifi_allocate_frame() returns NULL when the pool is exhausted? */
        /* wifi_allocate_frame() returns NULL if pool is full.
           new_frame = NULL is then sent to the queue. */
    }
}
```

**The actual bug:**

```c
wifi_frame_t *new_frame = wifi_allocate_frame();
/* If pool exhausted, new_frame == NULL */
/* Code does not check return value -- sends NULL to queue */
xQueueSendFromISR(rx_queue, &new_frame, NULL);  /* sends NULL pointer */
```

---

### Step 5 — Fix and Validate

```c
/* Fixed wifi_isr.c */
void wifi_rx_irq_handler(void) {
    wifi_frame_t *new_frame = wifi_allocate_frame();

    if (new_frame == NULL) {
        /* Pool exhausted: drop this frame, increment error counter */
        wifi_stats.rx_dropped_no_buffer++;
        /* Clear the hardware FIFO to prevent further interrupts */
        wifi_hw_discard_rx_frame();
        return;
    }

    wifi_hw_read_rx_frame(new_frame);

    BaseType_t higher_priority_woken = pdFALSE;
    if (xQueueSendFromISR(rx_queue, &new_frame, &higher_priority_woken) != pdPASS) {
        /* Queue full: release the frame we just allocated */
        wifi_free_frame(new_frame);
        wifi_stats.rx_dropped_queue_full++;
    }

    portYIELD_FROM_ISR(higher_priority_woken);
}

/* Add NULL check in the receiver as defensive programming */
void wifi_process_rx_frame(const wifi_frame_t *frame) {
    if (frame == NULL) {
        /* Should not happen with the fix, but defensive */
        return;
    }
    uint32_t payload_len = frame->header.length;
    /* ... */
}
```

**Validation:**

1. Stress test: run the system with a full frame pool (configure `WIFI_FRAME_POOL_SIZE = 1`) to force the allocation failure path.
2. Verify `wifi_stats.rx_dropped_no_buffer` increments under load.
3. Verify no crash for 72 hours under high wireless activity.
4. Add a unit test for `wifi_rx_irq_handler` using a mock allocator that returns NULL.

---

## Interview Discussion Points

**Q: Why did this only happen after 24-48 hours and only during high wireless activity?**

The frame pool exhaustion is a race condition that requires a specific sequence: high wireless traffic causes many frames to be allocated simultaneously while the processing task falls behind. Under normal load, the task processes frames fast enough that the pool never fully depletes. Only under sustained peak load does the pool temporarily exhaust, exposing the missing NULL check.

**Q: Could you have caught this with a watchdog alone?**

A watchdog would reset the system after the HardFault, but the infinite-loop handler prevented even the watchdog from firing. The persistent fault record mechanism was the key addition. Without it, each crash produced a reset with no diagnostic information.

**Q: What would CFSR = 0x00020000 (DIVBYZERO bit) indicate?**

A divide-by-zero fault. This would occur if `CCR.DIV_0_TRP` is set (enabled) and the code executes an `SDIV` or `UDIV` instruction with a zero divisor. The fix would be to find the division and add a guard.

**Q: What is the significance of the XPSR value showing thread mode (no interrupt)?**

`XPSR = 0x21000000`: bits [8:0] are the exception number. Value 0 means the fault occurred in thread mode (normal task context), not in an interrupt handler. If this had been `0x21000021`, exception number 0x21 = IRQ17 was active when the fault occurred, pointing to an ISR as the culprit. This immediately narrows the search space.
