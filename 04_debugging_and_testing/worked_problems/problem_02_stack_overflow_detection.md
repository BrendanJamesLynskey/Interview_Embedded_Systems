# Worked Problem 02: Stack Overflow Detection and Prevention

## Problem Statement

A FreeRTOS-based firmware on a Cortex-M4 has a memory fault approximately once every few hours. The crash is always a HardFault with `FORCED` set in `HFSR`, but the faulting `PC` is different every time, making it difficult to identify a single root cause. The system has four tasks:

```
Task Name       Priority   Stack Size (words)   Estimated Usage
--------------  ---------  -------------------  ---------------
CommTask        3          256                  Unknown
SensorTask      2          128                  Unknown
LogTask         1          256                  Unknown
IdleTask        0          128 (FreeRTOS idle)  Unknown
```

Stack sizes were set arbitrarily when the firmware was originally written. No stack overflow checking is currently in place. You are asked to diagnose whether stack overflow is the cause and implement a long-term prevention strategy.

**What would you do?**

---

## Background: How Stack Overflows Manifest in FreeRTOS

In FreeRTOS, each task's stack is a contiguous block of memory. By default, stacks are allocated from the heap or declared as static arrays. The stacks are not isolated from each other; they are placed adjacently in memory:

```
SRAM layout (typical, stacks growing downward):

High address  +------------------------+
              | CommTask stack[255]    |  <- top of CommTask stack
              | CommTask stack[254]    |
              | ...                    |
              | CommTask stack[0]      |  <- bottom of CommTask stack
              +------------------------+
              | LogTask stack[255]     |  <- top of LogTask stack
              | ...                    |
              | LogTask stack[0]       |  <- bottom of LogTask stack
              +------------------------+
              | SensorTask stack[127]  |
              | ...                    |
              | SensorTask stack[0]    |  <- bottom of SensorTask stack
              +------------------------+
              | other SRAM (globals,   |
              |   heap, etc.)          |
Low address   +------------------------+

If SensorTask overflows, its stack pointer goes below stack[0] into
LogTask's stack (or wherever the adjacent allocation is).

The corruption is silent. LogTask's stack is overwritten while
SensorTask runs. When LogTask next executes, its stack frame contains
garbage. LogTask crashes -- but the fault appears to be IN LogTask,
not in SensorTask which caused the problem.

This is why the PC differs every crash: it depends on which LogTask
instruction was executing when the corrupted stack frame was used.
```

---

## Solution: Step-by-Step

### Step 1 — Enable FreeRTOS Stack Overflow Hook

FreeRTOS has two built-in stack overflow detection methods, selected by `configCHECK_FOR_STACK_OVERFLOW` in `FreeRTOSConfig.h`:

```c
/* FreeRTOSConfig.h */

/* Method 1: Check if SP has gone out of bounds at context switch.
   Fast: just one comparison. May miss overflow if overflow happened
   and SP recovered before the context switch. */
#define configCHECK_FOR_STACK_OVERFLOW  1

/* Method 2: In addition to method 1, check that the last 16 bytes
   of each stack still contain the 0xA5 pattern written at task creation.
   Slower but catches overflows that occurred earlier and recovered. */
#define configCHECK_FOR_STACK_OVERFLOW  2
```

```c
/* This callback is called by FreeRTOS when overflow is detected */
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName) {
    /* Log the offending task name before taking action */
    /* WARNING: This hook runs in a CRITICAL SECTION with scheduler suspended.
       Do NOT call any FreeRTOS API here. Use direct register/UART output only. */

    /* Option 1: immediate reset with identifiable reset cause */
    fault_log("STACK OVERFLOW in task: %s\r\n", pcTaskName);
    NVIC_SystemReset();

    /* Option 2: hang for debugger attachment */
    /* taskDISABLE_INTERRUPTS(); */
    /* for (;;) {} */
}
```

**Limitations of this approach:**
- Method 1 can miss overflows that corrupt adjacent memory but return SP before the next context switch.
- Method 2 catches more cases but only checks 16 bytes, so a large overflow that writes far past the boundary is not always caught before damage is done to the adjacent task's stack.
- Neither method prevents the corruption -- they detect it after it has already occurred.

---

### Step 2 — Measure Actual Stack Usage with High Water Marks

FreeRTOS fills every task stack with `0xA5A5A5A5` at creation. The stack high water mark counts how many words at the bottom of the stack still contain `0xA5A5A5A5`:

```c
/* Task to periodically print stack usage statistics */
void vStackMonitorTask(void *params) {
    for (;;) {
        /* uxTaskGetSystemState returns a snapshot of all tasks */
        TaskStatus_t task_status[MAX_TASKS];
        UBaseType_t  n_tasks;
        uint32_t     total_runtime;

        n_tasks = uxTaskGetSystemState(
            task_status,
            MAX_TASKS,
            &total_runtime
        );

        debug_printf("Task name       | Priority | Stack HWM (words) | State\r\n");
        debug_printf("----------------|----------|-------------------|-------\r\n");

        for (UBaseType_t i = 0; i < n_tasks; i++) {
            const char *state_str[] = {"Running","Ready","Blocked","Suspended","Deleted"};
            debug_printf("%-16s|    %u     |        %4u        | %s\r\n",
                task_status[i].pcTaskName,
                task_status[i].uxCurrentPriority,
                task_status[i].usStackHighWaterMark,
                state_str[task_status[i].eCurrentState]
            );
        }
        debug_printf("\r\n");

        vTaskDelay(pdMS_TO_TICKS(5000));   /* print every 5 seconds */
    }
}
```

**Example output after running under load for 30 minutes:**

```
Task name       | Priority | Stack HWM (words) | State
----------------|----------|-------------------|-------
CommTask        |    3     |           8        | Blocked
SensorTask      |    2     |          62        | Blocked
LogTask         |    1     |         134        | Ready
IdleTask        |    0     |          78        | Running
StackMonitor    |    4     |         112        | Running
```

**Analysis:**

- `CommTask`: HWM = 8 words remaining out of 256 allocated. This is dangerously low -- 8 words = 32 bytes headroom. Any deeper call path or larger local variable will cause overflow.
- `SensorTask`: HWM = 62 words out of 128. Moderate headroom (48%). Acceptable but worth monitoring.
- `LogTask`: HWM = 134 words out of 256. Plenty of headroom -- LogTask was over-provisioned.

**Conclusion:** `CommTask` is almost certainly the source of the stack overflow.

---

### Step 3 — Find What CommTask Is Using Stack For

To understand why CommTask uses so much stack, examine the task function and all functions it calls:

```c
/* Original CommTask -- problematic */
void vCommTask(void *params) {
    for (;;) {
        /* Wait for incoming data */
        uint8_t rx_buffer[512];     /* 512 bytes = 128 words -- on STACK! */
        size_t  len;

        if (uart_receive_blocking(rx_buffer, sizeof(rx_buffer), &len, 1000)) {
            process_command(rx_buffer, len);
        }
    }
}

/* process_command calls json_parse which allocates more stack locally */
void process_command(const uint8_t *data, size_t len) {
    char json_buffer[256];          /* another 256 bytes = 64 words */
    JsonValue_t parsed;             /* sizeof(JsonValue_t) = 128 bytes = 32 words */
    /* ... */
    json_parse(json_buffer, &parsed);
}
```

**Stack usage calculation for CommTask:**

```
vCommTask frame:
  rx_buffer[512]:  512 bytes = 128 words
  len, local vars:   8 bytes =   2 words
  Saved registers:  32 bytes =   8 words
  Total:                        138 words

process_command frame (called from CommTask):
  json_buffer[256]: 256 bytes =  64 words
  parsed JsonValue:  128 bytes =  32 words
  Saved registers:   32 bytes =   8 words
  Total:                         104 words

json_parse frame (called from process_command):
  Internal parse state: ~20 words (estimated)

Total peak call depth: 138 + 104 + 20 = 262 words
CommTask stack size:   256 words
OVERFLOW by:           ~6 words = 24 bytes
```

The 8-word HWM is consistent: the stack was used down to 256 - 262 = -6 words below the bottom, but the `0xA5` pattern is only checked for 16 bytes. The overflow of 24 bytes destroyed the adjacent task's stack region.

---

### Step 4 — Fix the Stack Usage

**Fix 1 — Move large buffers out of task stack (preferred):**

```c
/* Option A: static buffer (acceptable if CommTask is the only user) */
static uint8_t rx_buffer[512];   /* global SRAM, not on any task's stack */

void vCommTask(void *params) {
    for (;;) {
        size_t len;
        if (uart_receive_blocking(rx_buffer, sizeof(rx_buffer), &len, 1000)) {
            process_command(rx_buffer, len);
        }
    }
}

/* Option B: allocate from heap, check for NULL, free when done */
void vCommTask(void *params) {
    for (;;) {
        uint8_t *rx_buffer = pvPortMalloc(512);
        if (rx_buffer != NULL) {
            size_t len;
            if (uart_receive_blocking(rx_buffer, 512, &len, 1000)) {
                process_command(rx_buffer, len);
            }
            vPortFree(rx_buffer);
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));  /* back off if heap full */
        }
    }
}
```

**Fix 2 — Resize CommTask stack based on measurements:**

```c
/* After moving large buffers off the stack, remeasure HWM.
   If HWM is now 80 words with 256 total: reduce to 128 with margin.
   If HWM = 80, recommended stack = 80 + 20% margin = 96 words. Round up to 128.
   This frees 128 words = 512 bytes of SRAM for other tasks. */

xTaskCreate(vCommTask, "CommTask", 128, NULL, 3, &xCommTaskHandle);
```

---

### Step 5 — Add MPU Stack Guard Regions

For each task, configure an MPU region at the bottom of its stack as a no-access guard. When overflow occurs, the first write past the boundary generates a MemManageFault immediately, rather than silently corrupting the adjacent task.

FreeRTOS MPU port (available with `configUSE_MPU_WRAPPERS = 1`) handles this automatically. For a manual approach:

```c
/*
 * Configure MPU guard for a task stack.
 * guard_base: bottom address of task stack (lowest address in stack region)
 *
 * The guard region is 32 bytes (minimum MPU region size) at the very bottom
 * of the stack. When SP decrements into this region, MemManageFault fires.
 */
void mpu_configure_task_guard(uint8_t mpu_region, uint32_t guard_base) {
    /* Ensure 32-byte alignment */
    guard_base &= ~0x1FU;

    MPU->CTRL = 0;  /* disable MPU while configuring */

    MPU->RNR  = mpu_region;
    MPU->RBAR = (guard_base & MPU_RBAR_ADDR_Msk)
              | MPU_RBAR_VALID_Msk
              | mpu_region;
    MPU->RASR = MPU_RASR_ENABLE_Msk
              | (0x4 << MPU_RASR_SIZE_Pos)  /* 2^(4+1) = 32 bytes */
              | (0x0 << MPU_RASR_AP_Pos)    /* no access, any privilege */
              | MPU_RASR_XN_Msk;

    MPU->CTRL = MPU_CTRL_ENABLE_Msk | MPU_CTRL_PRIVDEFENA_Msk;
    __DSB();
    __ISB();
}

/* In main(), after task creation: */
extern StackType_t comm_task_stack[];    /* declared in task creation code */
mpu_configure_task_guard(4, (uint32_t)comm_task_stack);

extern StackType_t sensor_task_stack[];
mpu_configure_task_guard(5, (uint32_t)sensor_task_stack);
```

**Limitation:** Only one MPU guard can be active per task unless the MPU is reconfigured on each context switch. FreeRTOS MPU port does this automatically. The manual approach above fixes guard regions for all tasks simultaneously, using one MPU region per task.

---

### Step 6 — Runtime Continuous Monitoring

Add a high-priority monitor task that checks stack headroom every few seconds and triggers a controlled shutdown before overflow occurs:

```c
#define STACK_CRITICAL_THRESHOLD_WORDS  16U   /* less than this = critical */
#define STACK_WARNING_THRESHOLD_WORDS   32U   /* less than this = warning */

void vStackGuardTask(void *params) {
    for (;;) {
        TaskHandle_t all_tasks[MAX_TASKS];
        UBaseType_t n = uxTaskGetNumberOfTasks();

        /* Get the handle to each task and check its watermark */
        TaskStatus_t status[MAX_TASKS];
        uxTaskGetSystemState(status, MAX_TASKS, NULL);

        for (UBaseType_t i = 0; i < n; i++) {
            UBaseType_t hwm = status[i].usStackHighWaterMark;

            if (hwm < STACK_CRITICAL_THRESHOLD_WORDS) {
                /* Immediate controlled shutdown to prevent data corruption */
                debug_printf("CRITICAL: Task '%s' stack headroom = %u words. Resetting.\r\n",
                             status[i].pcTaskName, hwm);
                /* Log to non-volatile storage before reset */
                nvlog_write_event(EVENT_STACK_CRITICAL, status[i].pcTaskName, hwm);
                NVIC_SystemReset();
            } else if (hwm < STACK_WARNING_THRESHOLD_WORDS) {
                debug_printf("WARNING: Task '%s' stack headroom = %u words.\r\n",
                             status[i].pcTaskName, hwm);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1000));  /* check every second */
    }
}
```

---

## Summary: Complete Prevention Strategy

```
Level      Mechanism                        Detects       Prevents
---------  -------------------------------  ------------  ---------
Compile    -fstack-usage flag (GCC)         Estimates      No
           Reports per-function stack usage at build time.
           Use to identify large stack consumers before runtime.

Runtime    configCHECK_FOR_STACK_OVERFLOW=2 After overflow  No
           FreeRTOS 0xA5 canary check at context switch.

Runtime    MPU no-access guard region       At first        No
           MemManageFault on first access   overflow byte   (but immediate detection)
           past stack bottom.

Runtime    High-water mark monitoring       Before overflow  No
           Pre-emptive controlled reset     (with margin)    (controlled)
           when headroom drops below 32 words.

Design     Static buffers off the stack     N/A             Yes
           Move large local arrays to
           global or heap allocation.

Design     Correct stack sizing             N/A             Yes
           Size = measured HWM + 25% margin,
           remeasured under worst-case load.
```

---

## Interview Discussion Points

**Q: The crash PC was different every time. Why is that consistent with stack overflow?**

When SensorTask's stack overflows into the adjacent LogTask stack region, it overwrites LogTask's stack frames and saved return addresses. The exact content of those frames depends on what LogTask was doing at the moment of overflow. On each crash, SensorTask overflows at a slightly different time relative to LogTask's execution, hitting different saved PC values in LogTask's frames. The corrupted return address therefore sends the CPU to a different garbage location each time.

**Q: GCC has a `-fstack-usage` flag. How would you use it?**

Compile with `-fstack-usage`. GCC generates a `.su` file alongside each `.o` file, listing the stack usage of every function:

```
sensor.c:read_temperature:96:static
comm.c:process_command:384:dynamic
uart.c:uart_receive_blocking:48:static
```

A `dynamic` annotation means the function calls another function (not just local variables). To find the worst-case stack consumption for a task, sum the `static` plus `dynamic` entries along the deepest call path. Tools like `cflow` or `avstack` automate this. Any function marked `dynamic,unbounded` (e.g., uses recursion) is a red flag.

**Q: Why would you ever use `configCHECK_FOR_STACK_OVERFLOW 1` instead of `2`?**

Method 2 checks 16 bytes at the stack bottom on every context switch, which adds a small overhead. On a system with very frequent context switches (1 kHz or higher tick rate with many tasks), Method 1 is used to reduce the monitoring overhead. In practice, Method 2 is almost always the better choice given the safety benefit and minimal overhead on modern MCUs.

**Q: Can a stack overflow cause problems that do not immediately crash the system?**

Yes. If the overflow writes into a neighbour's stack region but does not destroy a saved return address, the corrupted values may be treated as valid stack data (local variable overwritten with attacker-controlled or random values). The neighbour task continues running but with incorrect variable values. The resulting behaviour could be silent misconfiguration, wrong sensor readings, ignored commands, or security vulnerabilities if the overwritten variable controls access decisions. This is the most dangerous form because the system appears to work correctly while producing wrong results.
