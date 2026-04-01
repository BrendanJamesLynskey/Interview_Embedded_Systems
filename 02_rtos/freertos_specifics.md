# FreeRTOS Specifics

## Overview

FreeRTOS is the dominant open-source RTOS for microcontrollers. It runs on hundreds of hardware platforms and is the default RTOS in Amazon Web Services IoT SDKs. Embedded systems interviews at companies using ARM Cortex-M, RISC-V, or Xtensa-based MCUs will almost certainly probe FreeRTOS-specific knowledge. This document covers heap management strategies, key API details, and the `FreeRTOSConfig.h` configuration system.

---

## Key Concepts

### Heap Management: heap_1 through heap_5

FreeRTOS provides five alternative dynamic memory allocation schemes, selected by including the appropriate source file in the build. Each scheme makes different trade-offs between complexity, capability, and determinism.

---

#### heap_1 — Simplest Possible, No Free

`heap_1` allocates memory from a statically declared array using a pointer that only ever moves forward. Freed memory is never reclaimed.

```c
/* heap_1 internal state (simplified) */
static uint8_t ucHeap[configTOTAL_HEAP_SIZE];
static size_t  xNextFreeByte = 0;

void *pvPortMalloc(size_t xWantedSize)
{
    void *pvReturn = NULL;
    xWantedSize = ALIGN_UP(xWantedSize);
    if (xNextFreeByte + xWantedSize <= configTOTAL_HEAP_SIZE) {
        pvReturn = &ucHeap[xNextFreeByte];
        xNextFreeByte += xWantedSize;
    }
    return pvReturn;   /* NULL if insufficient space */
}

void vPortFree(void *pv) { (void)pv; }  /* no-op */
```

**Characteristics:**
- Deterministic O(1) allocation time.
- `vPortFree()` is a no-op — memory is never released.
- No fragmentation (allocation pointer moves forward only).
- No thread safety issues from fragmented free lists.

**When to use:** systems where all tasks, queues, and semaphores are created at startup and never deleted. Safety-critical systems where determinism is paramount and static allocation is preferred. Use with `xTaskCreateStatic` / `xQueueCreateStatic` for fully static allocation without any heap.

---

#### heap_2 — Best Fit, Allows Free, No Coalescence

`heap_2` adds `vPortFree()` support using a free list sorted by block size. Allocation uses a best-fit algorithm.

**Characteristics:**
- Allows freeing and reallocation.
- Does **not** combine adjacent free blocks (no coalescence). Leads to fragmentation over time.
- Allocation is O(n) in the number of free blocks.
- Deterministic allocation time only if block sizes are known and fixed.

**When to use:** systems that create and delete tasks/queues of always-identical sizes. Deprecated in favour of heap_4 for most new designs.

---

#### heap_3 — Standard C `malloc`/`free`

`heap_3` is a wrapper around the compiler's standard library `malloc` and `free`. It temporarily disables the FreeRTOS scheduler around each call to provide thread safety.

```c
void *pvPortMalloc(size_t xWantedSize)
{
    void *pvReturn;
    vTaskSuspendAll();          /* suspend scheduler, not interrupts */
    pvReturn = malloc(xWantedSize);
    xTaskResumeAll();
    return pvReturn;
}
```

**Characteristics:**
- Uses the linker-defined heap (`--heap_size` in scatter file / linker script).
- Non-deterministic: `malloc` performance depends on fragmentation state.
- Thread-safe via scheduler suspension.
- Full `malloc`/`free` semantics, including coalescence.

**When to use:** rarely — mostly for porting applications that already use standard `malloc`. Not suitable for hard real-time due to non-deterministic timing.

---

#### heap_4 — First Fit with Coalescence (Most Common)

`heap_4` is the recommended general-purpose allocator. It uses a first-fit algorithm with immediate coalescence of adjacent free blocks, preventing fragmentation.

```
Free list (sorted by address):
[Block A: 64 bytes] -> [Block B: 128 bytes] -> [Block C: 32 bytes]

After freeing a block between A and C:
[Block A: 64 bytes] -> [freed: 48 bytes] -> [Block C: 32 bytes]

Coalescence check: is freed block adjacent to A? No. Adjacent to C? No.
New free list: [Block A: 64 bytes] -> [freed: 48 bytes] -> [Block C: 32 bytes]

If freed block is adjacent to C:
Merged free list: [Block A: 64 bytes] -> [merged: 80 bytes]
```

**Characteristics:**
- Combines adjacent free blocks on each `vPortFree()` call.
- Reduces fragmentation significantly vs heap_2.
- First-fit allocation: O(n) but typically fast in practice.
- Not worst-case deterministic — allocation time depends on free list state.
- The heap array is declared in BSS: `static uint8_t ucHeap[configTOTAL_HEAP_SIZE]`.

**When to use:** the default choice for most FreeRTOS applications. Tasks and objects of varying sizes, where fragmentation could occur but heap_3's non-determinism is unacceptable.

---

#### heap_5 — heap_4 across Multiple Non-Contiguous Memory Regions

`heap_5` extends heap_4 to support multiple, physically separate memory regions (e.g., internal SRAM at 0x20000000 and external PSRAM at 0x60000000 on an STM32H7).

```c
/* Describe the memory regions to the heap manager */
const HeapRegion_t xHeapRegions[] = {
    { (uint8_t *)0x20000000UL, 0x00010000UL },  /* 64 KB internal SRAM */
    { (uint8_t *)0x60000000UL, 0x00100000UL },  /* 1 MB external PSRAM */
    { NULL, 0 }                                  /* terminator */
};

/* Call once before any pvPortMalloc(), typically before vTaskStartScheduler() */
vPortDefineHeapRegions(xHeapRegions);
```

**Characteristics:**
- Supports allocating from multiple disjoint memory regions.
- Uses the same first-fit + coalescence algorithm as heap_4 within and between regions.
- Useful for STM32, NXP i.MX RT, and other MCUs with multiple SRAM banks.
- Must call `vPortDefineHeapRegions()` before creating any RTOS objects.

**When to use:** systems with multiple memory regions — internal TCM/SRAM for critical tasks, external RAM for large buffers or logging queues.

---

### Comparison Summary

| Scheme | Free? | Coalescence | Deterministic | Use case |
|---|---|---|---|---|
| heap_1 | No | N/A | Yes (O(1)) | Static-only systems, safety-critical |
| heap_2 | Yes | No | Conditional | Fixed-size objects only (deprecated) |
| heap_3 | Yes | Yes (stdlib) | No | Porting legacy code |
| heap_4 | Yes | Yes | No (O(n)) | General purpose (most common) |
| heap_5 | Yes | Yes | No (O(n)) | Multi-region memory (MCUs with external RAM) |

---

### Checking Available Heap

```c
/* Remaining free heap in bytes */
size_t xFreeHeap = xPortGetFreeHeapSize();

/* Minimum free heap ever recorded since boot (watermark) */
size_t xMinFreeHeap = xPortGetMinimumEverFreeHeapSize();

/* Typical usage: log heap stats in a diagnostic task */
void diagnostics_task(void *pvParameters)
{
    for (;;) {
        printf("Heap free: %u bytes, min ever: %u bytes\n",
               (unsigned)xPortGetFreeHeapSize(),
               (unsigned)xPortGetMinimumEverFreeHeapSize());
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
```

---

### Configuring FreeRTOS: FreeRTOSConfig.h

`FreeRTOSConfig.h` is the application-specific configuration file. It must be in the include path and is included by the FreeRTOS kernel source files. Every project must supply one.

**Mandatory settings:**

```c
/* CPU clock frequency — used for runtime stats */
#define configCPU_CLOCK_HZ                ( ( unsigned long ) 168000000 )

/* Tick rate: 1000 = 1 ms tick, 100 = 10 ms tick */
#define configTICK_RATE_HZ                ( ( TickType_t ) 1000 )

/* Maximum priority level. Valid priorities: 0 to configMAX_PRIORITIES-1.
   Priority 0 = idle. Increasing this uses more RAM (one list per priority). */
#define configMAX_PRIORITIES              ( 5 )

/* Minimum stack size for the idle task (in words, not bytes) */
#define configMINIMAL_STACK_SIZE          ( ( unsigned short ) 128 )

/* Total heap size in bytes (used by heap_1 through heap_5) */
#define configTOTAL_HEAP_SIZE             ( ( size_t ) ( 32 * 1024 ) )

/* Maximum length of a task name string (including null terminator) */
#define configMAX_TASK_NAME_LEN           ( 16 )

/* Width of TickType_t: 1 = 16-bit (max timeout ~65 ticks),
                        0 = 32-bit (max timeout ~49 days at 1 kHz) */
#define configUSE_16_BIT_TICKS            0

/* Stack checking: 0=off, 1=simple check (fast), 2=full paint check (thorough) */
#define configCHECK_FOR_STACK_OVERFLOW    2

/* Memory allocation: 1=only static API, 0=dynamic API available */
#define configSUPPORT_STATIC_ALLOCATION   0
#define configSUPPORT_DYNAMIC_ALLOCATION  1
```

**Scheduler options:**

```c
#define configUSE_PREEMPTION              1   /* 1=preemptive, 0=cooperative */
#define configUSE_TIME_SLICING            1   /* round-robin within same priority */
#define configUSE_TICKLESS_IDLE           0   /* 1=enable tickless idle (power saving) */
#define configIDLE_SHOULD_YIELD           1   /* idle yields when same-priority tasks ready */
```

**Feature toggles (set 0 to exclude from build, saves code space):**

```c
#define configUSE_MUTEXES                 1
#define configUSE_RECURSIVE_MUTEXES       1
#define configUSE_COUNTING_SEMAPHORES     1
#define configUSE_TASK_NOTIFICATIONS      1
#define configUSE_TIMERS                  1
#define configUSE_EVENT_GROUPS            1
#define configUSE_QUEUE_SETS              0
#define configUSE_TASK_FPU_SUPPORT        1   /* FPU context save/restore */
```

**Software timer configuration:**

```c
#define configUSE_TIMERS                  1
#define configTIMER_TASK_PRIORITY         ( configMAX_PRIORITIES - 1 )
#define configTIMER_QUEUE_LENGTH          10
#define configTIMER_TASK_STACK_DEPTH      ( configMINIMAL_STACK_SIZE * 2 )
```

**Runtime statistics (requires a high-resolution timer):**

```c
#define configGENERATE_RUN_TIME_STATS     1
#define configUSE_STATS_FORMATTING_FUNCTIONS 1
/* Must also provide: */
/* extern void vConfigureTimerForRunTimeStats(void); */
/* extern unsigned long ulGetRunTimeCounterValue(void); */
#define portCONFIGURE_TIMER_FOR_RUN_TIME_STATS() vConfigureTimerForRunTimeStats()
#define portGET_RUN_TIME_COUNTER_VALUE()          ulGetRunTimeCounterValue()
```

**Hook function enables:**

```c
#define configUSE_IDLE_HOOK               1   /* void vApplicationIdleHook(void) */
#define configUSE_TICK_HOOK               0   /* void vApplicationTickHook(void) */
#define configUSE_MALLOC_FAILED_HOOK      1   /* void vApplicationMallocFailedHook(void) */
```

**Interrupt priority configuration (ARM Cortex-M critical):**

```c
/* Highest interrupt priority that can call FreeRTOS FromISR APIs.
   Lower numeric value = higher priority on Cortex-M.
   ISRs with priority ABOVE (numerically lower than) this value MUST NOT
   call any FreeRTOS API — they are truly hardware-level and not kernel-aware. */
#define configMAX_SYSCALL_INTERRUPT_PRIORITY    ( 5 << (8 - __NVIC_PRIO_BITS) )

/* Lowest priority that the kernel uses itself (SysTick, PendSV).
   Must be the lowest possible (highest numeric value). */
#define configKERNEL_INTERRUPT_PRIORITY         ( 15 << (8 - __NVIC_PRIO_BITS) )
```

---

### Key API Reference

**Task creation:**

```c
/* Dynamic allocation */
BaseType_t xTaskCreate(
    TaskFunction_t  pvTaskCode,       /* task function: void f(void *pvParameters) */
    const char     *pcName,           /* name for debugging, up to configMAX_TASK_NAME_LEN */
    uint16_t        usStackDepth,     /* stack size in WORDS (not bytes) */
    void           *pvParameters,     /* passed to task function */
    UBaseType_t     uxPriority,       /* 0 = idle, configMAX_PRIORITIES-1 = highest */
    TaskHandle_t   *pxCreatedTask);   /* out: handle, or NULL to discard */

/* Static allocation (no heap required) */
TaskHandle_t xTaskCreateStatic(
    TaskFunction_t  pvTaskCode,
    const char     *pcName,
    uint32_t        ulStackDepth,
    void           *pvParameters,
    UBaseType_t     uxPriority,
    StackType_t    *puxStackBuffer,   /* caller-provided stack buffer */
    StaticTask_t   *pxTaskBuffer);    /* caller-provided TCB buffer */
```

**Scheduler control:**

```c
void vTaskStartScheduler(void);        /* starts the RTOS — never returns on success */
void vTaskEndScheduler(void);          /* stops the RTOS (rare — mainly for testing) */
void vTaskSuspendAll(void);            /* suspends scheduler (NOT interrupts) */
BaseType_t xTaskResumeAll(void);       /* resumes scheduler, returns pdTRUE if switch needed */
```

**Task utilities:**

```c
TaskHandle_t xTaskGetCurrentTaskHandle(void);
UBaseType_t  uxTaskGetStackHighWaterMark(TaskHandle_t xTask);  /* min free stack words */
eTaskState   eTaskGetState(TaskHandle_t xTask);
void         vTaskGetRunTimeStats(char *pcWriteBuffer);         /* requires configGENERATE_RUN_TIME_STATS */
void         vTaskList(char *pcWriteBuffer);                    /* human-readable task list */
```

**Software timers:**

```c
TimerHandle_t xTimerCreate(
    const char     *pcTimerName,
    TickType_t      xTimerPeriodInTicks,
    BaseType_t      xAutoReload,        /* pdTRUE = periodic, pdFALSE = one-shot */
    void           *pvTimerID,          /* application identifier */
    TimerCallbackFunction_t pxCallbackFunction);

BaseType_t xTimerStart(TimerHandle_t xTimer, TickType_t xTicksToWait);
BaseType_t xTimerStop (TimerHandle_t xTimer, TickType_t xTicksToWait);
BaseType_t xTimerReset(TimerHandle_t xTimer, TickType_t xTicksToWait);
BaseType_t xTimerChangePeriod(TimerHandle_t xTimer, TickType_t xNewPeriod, TickType_t xTicksToWait);

/* Timer callbacks execute in the context of the timer daemon task.
   They must NOT block. Use them for short, quick operations.
   For longer work, give a semaphore to a dedicated task. */
```

---

### Static vs Dynamic Allocation

FreeRTOS supports fully static allocation (no heap required) using the `Static` variants of creation APIs.

```c
/* Statically allocated task */
static StaticTask_t  xTaskBuffer;
static StackType_t   xTaskStack[512];  /* 512 words = 2 KB */

void create_static_task(void)
{
    xTaskCreateStatic(
        my_task_function,
        "MY_TASK",
        512,            /* stack depth in words */
        NULL,
        3,              /* priority */
        xTaskStack,
        &xTaskBuffer);
}

/* Required when configSUPPORT_STATIC_ALLOCATION = 1:
   Provide the idle task's stack and TCB */
void vApplicationGetIdleTaskMemory(StaticTask_t **ppxIdleTaskTCBBuffer,
                                   StackType_t  **ppxIdleTaskStackBuffer,
                                   uint32_t      *pulIdleTaskStackSize)
{
    static StaticTask_t xIdleTaskTCB;
    static StackType_t  xIdleTaskStack[configMINIMAL_STACK_SIZE];
    *ppxIdleTaskTCBBuffer  = &xIdleTaskTCB;
    *ppxIdleTaskStackBuffer = xIdleTaskStack;
    *pulIdleTaskStackSize   = configMINIMAL_STACK_SIZE;
}
```

Static allocation is preferred in safety-critical systems (IEC 61508, ISO 26262) because:
1. All memory is allocated at compile time — no heap exhaustion at runtime.
2. Memory usage is visible in the linker map file.
3. Deterministic allocation (no `malloc` latency).
4. Easier to certify — no dynamic memory analysis required.

---

## Interview Questions

### Fundamentals Tier

---

**Q1. What are the five FreeRTOS heap schemes? Which would you choose for a safety-critical medical device that creates all tasks at startup?**

**Answer:**

- **heap_1**: simplest, allocation only (no free), O(1) deterministic. Memory pointer advances forward only.
- **heap_2**: adds free via best-fit free list, no coalescence. Can fragment if sizes vary.
- **heap_3**: wraps standard library `malloc`/`free`. Non-deterministic, depends on C library.
- **heap_4**: first-fit with coalescence of adjacent free blocks. General purpose, reduces fragmentation.
- **heap_5**: extends heap_4 to support multiple non-contiguous memory regions.

For a safety-critical medical device that creates all tasks at startup and never deletes them: **heap_1** is the correct choice.

Rationale:
- All allocations happen during startup (task/queue/semaphore creation). Once the scheduler starts, no further `pvPortMalloc` calls occur.
- heap_1 is deterministic (O(1) allocation), has no fragmentation, and its implementation is simple enough to audit line by line.
- `vPortFree` is a no-op — no risk of use-after-free, double-free, or heap corruption.
- Alternatively, use `configSUPPORT_STATIC_ALLOCATION = 1` with `xTaskCreateStatic` to eliminate the heap entirely, placing all buffers in known BSS sections visible in the linker map.

---

**Q2. What is `configMAX_SYSCALL_INTERRUPT_PRIORITY` and what happens if you call `xSemaphoreGiveFromISR` from an ISR with a priority above this value?**

**Answer:**

`configMAX_SYSCALL_INTERRUPT_PRIORITY` defines the highest interrupt priority (lowest numerical value on Cortex-M) from which FreeRTOS API functions (the `FromISR` variants) may be called safely.

FreeRTOS's critical sections on Cortex-M use `BASEPRI` to mask interrupts at or below the specified priority. Interrupts with priority strictly higher (numerically lower) than `configMAX_SYSCALL_INTERRUPT_PRIORITY` are not maskable by BASEPRI — they are "ultra-high priority" hardware-level ISRs that can preempt even the FreeRTOS critical section.

If you call a FreeRTOS API from an ISR configured with a priority above (numerically lower than) `configMAX_SYSCALL_INTERRUPT_PRIORITY`:

1. The API call can interrupt the FreeRTOS kernel in the middle of a critical section.
2. The kernel's internal data structures (ready lists, timer lists, semaphore counts) are partially updated and in an inconsistent state.
3. The API call corrupts the data structure.
4. The system behaves unpredictably — hangs, wrong task switching, or memory corruption.

**Correct configuration:**
```c
/* configMAX_SYSCALL_INTERRUPT_PRIORITY = 5 means:
   ISRs at priorities 5, 6, 7... (numerically higher) CAN call FromISR APIs.
   ISRs at priorities 1, 2, 3, 4 (numerically lower, higher urgency) MUST NOT. */
#define configMAX_SYSCALL_INTERRUPT_PRIORITY    ( 5 << (8 - __NVIC_PRIO_BITS) )
```

Set safety-critical ISRs (e.g., hard fault handlers, high-urgency hardware protection) above `configMAX_SYSCALL_INTERRUPT_PRIORITY`. Set all application ISRs that use FreeRTOS APIs at or below it.

---

**Q3. What is the FreeRTOS timer daemon task and what restrictions apply to timer callback functions?**

**Answer:**

The software timer daemon (timer service task) is a built-in FreeRTOS task created automatically when `configUSE_TIMERS = 1`. It runs at `configTIMER_TASK_PRIORITY` and processes timer commands from the timer command queue.

When an application calls `xTimerStart`, `xTimerStop`, `xTimerReset`, or `xTimerChangePeriod`, these commands are posted to the timer command queue. The timer daemon task reads from this queue and executes the actual timer operations, including calling expired timer callback functions.

**Restrictions on timer callbacks:**
1. **Must not block**: the callback executes inside the daemon task's context. If it blocks, no other timer callbacks or commands can be processed. Use `xTimerCallback` only for short, deterministic operations.
2. **Must not call blocking API with `portMAX_DELAY`**: even a finite long delay holds up all other timer processing.
3. **For longer work**: give a semaphore or send a task notification from the callback; let a dedicated task do the actual work.
4. **Must not modify the timer that called it** in most cases (use `xTimerChangePeriodFromISR` patterns if needed from callbacks).

```c
/* Wrong: callback does substantial work */
void timer_callback(TimerHandle_t xTimer)
{
    read_10KB_from_flash();   /* blocks for many ms — starves other timers */
    process_data();
}

/* Correct: callback posts to a task */
void timer_callback(TimerHandle_t xTimer)
{
    xSemaphoreGive(xProcessSemaphore);   /* signal the processing task */
}
```

---

### Intermediate Tier

---

**Q4. A system uses heap_4. After running for 24 hours, `pvPortMalloc` starts returning NULL even though `xPortGetMinimumEverFreeHeapSize` shows plenty of space was available at startup. Diagnose and fix.**

**Answer:**

`pvPortMalloc` returning NULL with apparently sufficient historical free space is a classic heap fragmentation symptom.

**Diagnosis:**

heap_4 uses a first-fit allocator with coalescence. Even with coalescence, if the allocation pattern creates many small non-adjacent free blocks, a large allocation request may fail because no single contiguous block is large enough — even if the total free space exceeds the request.

```
Free list after 24 hours of varying allocations/frees:
[32 bytes free] -> [128 bytes used] -> [48 bytes free] -> [64 bytes used] -> [32 bytes free]
Total free: 112 bytes. Largest contiguous block: 48 bytes.
A request for 64 bytes fails despite 112 total free bytes.
```

**Verification steps:**
1. Add instrumentation that logs `xPortGetFreeHeapSize()` and allocation block size at each `pvPortMalloc` failure. Compare total free vs requested size.
2. Use a heap analysis tool or add a custom allocator walk to count the number and sizes of free blocks.
3. Profile which tasks are allocating and freeing — find any patterns of mismatched sizes.

**Fixes:**

1. **Static allocation**: the most reliable fix. Convert dynamically allocated objects to statically allocated ones using `xTaskCreateStatic`, `xQueueCreateStatic`, etc. Eliminates fragmentation at its source.

2. **Fixed-size memory pools**: for frequently allocated/freed objects of the same size, implement a pool allocator (array of fixed-size blocks with a free list). Zero fragmentation because all blocks are identical.

3. **Increase heap size**: provides more total space, reducing the likelihood of fragmentation causing failures — but does not eliminate the root cause.

4. **Restructure allocation pattern**: allocate all long-lived objects first (at startup), then short-lived objects. Avoids the interleaving that creates fragmented free lists.

5. **Use heap_3** (stdlib malloc): some stdlib implementations use better fragmentation mitigation algorithms (e.g., dlmalloc). Trades determinism for fragmentation resistance.

---

**Q5. How do you implement a software watchdog using FreeRTOS tasks? What are the important design considerations?**

**Answer:**

A software watchdog monitors that all critical tasks are completing their work within their deadlines. If any task fails to check in within the watchdog timeout, the watchdog triggers a system reset or safe state entry.

**Design:**

```c
/* Watchdog task: highest priority in the system */

#define NUM_MONITORED_TASKS     3U
#define WATCHDOG_PERIOD_MS      100U
#define TASK_TIMEOUT_TICKS      pdMS_TO_TICKS(500U)   /* 5x the watchdog period */

typedef struct {
    TaskHandle_t      xHandle;
    const char       *pcName;
    TickType_t        xLastCheckin;
    TickType_t        xTimeoutTicks;
    BaseType_t        xAlive;
} WatchdogEntry_t;

static WatchdogEntry_t xWatchdogTable[NUM_MONITORED_TASKS];

/* Each monitored task calls this periodically */
void watchdog_checkin(uint8_t ucTaskIndex)
{
    configASSERT(ucTaskIndex < NUM_MONITORED_TASKS);
    /* Atomic update: single-word write, no mutex needed on 32-bit Cortex-M */
    xWatchdogTable[ucTaskIndex].xLastCheckin = xTaskGetTickCount();
    xWatchdogTable[ucTaskIndex].xAlive       = pdTRUE;
}

/* Watchdog monitor task — highest priority */
void watchdog_task(void *pvParameters)
{
    TickType_t xLastWakeTime = xTaskGetTickCount();

    for (;;) {
        vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(WATCHDOG_PERIOD_MS));

        TickType_t xNow = xTaskGetTickCount();

        for (uint8_t i = 0U; i < NUM_MONITORED_TASKS; i++) {
            TickType_t xElapsed = xNow - xWatchdogTable[i].xLastCheckin;

            if (xElapsed > xWatchdogTable[i].xTimeoutTicks) {
                /* Task i has not checked in — take action */
                enter_safe_state(i);   /* log, assert, or reset */
            }

            /* Clear the alive flag: task must set it again next period */
            xWatchdogTable[i].xAlive = pdFALSE;
        }

        /* Feed the hardware watchdog (if present) — only if all tasks are OK */
        HAL_IWDG_Refresh(&hiwdg);
    }
}
```

**Key design considerations:**

1. **Watchdog task priority**: must be the highest priority in the system. If another task starves the watchdog, the hardware watchdog times out — the desired behaviour for detecting starvation.

2. **Hardware watchdog backing**: the software watchdog is only meaningful if it feeds a hardware watchdog that resets the system when not fed. A software watchdog alone is defeated if the watchdog task itself hangs.

3. **Checkin frequency**: monitored tasks must check in at least once per watchdog timeout period. Adjust timeout to match each task's expected period — a slow task can have a longer timeout than a fast task.

4. **What to do on timeout**: options range from logging and asserting (development) to entering a defined safe state (production) to full system reset. For IEC 61508 SIL-2+ systems, the action must be documented and verified.

5. **Avoid false positives**: a task temporarily blocked on legitimate I/O must not trigger the watchdog. Design check-in points carefully: check in before blocking, not after.

---

### Advanced Tier

---

**Q6. Explain how FreeRTOS implements priority inheritance for mutexes. Trace through the kernel data structures.**

**Answer:**

When `xSemaphoreTake` is called on a mutex that is already held:

**Step 1: Mutex ownership check.**
FreeRTOS mutexes store the `TaskHandle_t` of the owning task in the queue structure's `pvOwner` field (the FreeRTOS mutex is implemented internally as a queue with special semantics).

```c
/* Internal mutex structure (simplified from queue.c) */
typedef struct QueueDefinition {
    /* ... queue fields ... */
    TaskHandle_t pxMutexHolder;   /* owner task, NULL if available */
    UBaseType_t  uxRecursiveCallCount;
} Queue_t;
```

**Step 2: Priority inheritance check.**
When a task (`xBlockedTask`) is about to block on the mutex, the kernel compares `xBlockedTask->uxPriority` with `pxMutexHolder->uxPriority`:

```c
/* prvInheritPriority (simplified) */
if (pxMutexHolder->uxPriority < xBlockedTask->uxPriority) {
    /* Inheritance needed */
    pxMutexHolder->uxPriority = xBlockedTask->uxPriority;
    /* Re-sort the holder's position in the ready list
       (it is now at a higher priority) */
    if (eTaskGetState(pxMutexHolder) == eReady) {
        /* Remove from old priority list, add to new priority list */
        uxListRemove(&pxMutexHolder->xStateListItem);
        vListInsertEnd(&pxReadyTasksLists[pxMutexHolder->uxPriority],
                       &pxMutexHolder->xStateListItem);
    }
}
```

**Step 3: Restoration when mutex is given.**
`xSemaphoreGive` calls `prvDisinheritPriority`:

```c
/* Restore original priority when mutex is given back */
if (pxCurrentTCB->uxPriority != pxCurrentTCB->uxBasePriority) {
    /* Restore base priority */
    pxCurrentTCB->uxPriority = pxCurrentTCB->uxBasePriority;
    /* Re-sort in ready list at restored priority */
    portYIELD_WITHIN_API();   /* trigger scheduler — woken task may be higher priority */
}
```

FreeRTOS stores both `uxPriority` (effective/current) and `uxBasePriority` (original). `uxBasePriority` is set at task creation and is restored after disinheritance.

**Limitation**: FreeRTOS implements one level of transitivity. If task L holds mutex A, and task M (elevated by H's block on mutex B held by M) blocks on mutex A, L should be elevated to H's priority. FreeRTOS v10+ handles this case but only for direct inheritance chains — deeply nested chains may not be fully resolved.

---

**Q7. Compare `xTaskNotify` with semaphores. When would you specifically choose one over the other, with quantitative justification?**

**Answer:**

**Task notifications:**
- Built into every task (no additional kernel object).
- 32-bit value per task (can carry data, not just a signal).
- Four modes: set bits, increment (counting), set value, or set value with overwrite.
- `xTaskNotify` / `xTaskNotifyGive`: ~25 instructions on Cortex-M.
- `ulTaskNotifyTake` / `xTaskNotifyWait`: unblock latency comparable to semaphore.

**Semaphores/mutexes:**
- Separate heap-allocated kernel object (minimum ~80 bytes for queue structure).
- Binary or counting, priority inheritance for mutexes.
- Multiple tasks can wait on one semaphore.
- `xSemaphoreGive`: ~40 instructions on Cortex-M (more due to queue bookkeeping).

**FreeRTOS benchmark numbers (Cortex-M4 at 168 MHz):**
- Notification give + take (unblocking): ~340 cycles (~2 µs).
- Binary semaphore give + take (unblocking): ~500 cycles (~3 µs).
- Mutex give + take (with inheritance check): ~600 cycles (~3.6 µs).

**Choose task notifications when:**
1. Exactly one task receives the signal (one-to-one). Notifications cannot be given to a pool of receivers.
2. Memory is very constrained — saving 80 bytes per synchronisation point matters.
3. Maximum speed is needed in a tight control loop or ISR path.
4. The signal carries a 32-bit value (avoids a separate queue).

```c
/* Fast ISR -> task notification */
void EXTI0_IRQHandler(void)
{
    BaseType_t xWoken = pdFALSE;
    vTaskNotifyGiveFromISR(xWorkerHandle, &xWoken);  /* ~15% faster than semaphore */
    portYIELD_FROM_ISR(xWoken);
}
```

**Choose semaphores/mutexes when:**
1. Multiple tasks may wait on the same event (many-to-one or broadcast via counting semaphore).
2. Mutual exclusion with priority inheritance is needed (use mutex).
3. The kernel object needs to be passed around or referenced by handle (e.g., from multiple modules).
4. A queue set is needed (task notifications cannot be added to queue sets).

---

**Q8. You are targeting a system with 4 KB of RAM total. Design a FreeRTOS configuration that creates two tasks with the minimum possible RAM footprint, explaining each trade-off.**

**Answer:**

With 4 KB total RAM, every byte matters. The strategy is: heap_1 for determinism and no overhead, minimum stack sizes, static allocation to avoid dynamic memory entirely, and disabling all unused features.

**Memory breakdown:**

| Item | Size (bytes) | Notes |
|---|---|---|
| Task 1 TCB (StaticTask_t) | 80 | FreeRTOS v10, Cortex-M4 |
| Task 2 TCB (StaticTask_t) | 80 | |
| Idle task TCB (StaticTask_t) | 80 | Required |
| Task 1 stack (128 words) | 512 | Minimum for non-trivial tasks |
| Task 2 stack (128 words) | 512 | |
| Idle task stack (128 words) | 512 | configMINIMAL_STACK_SIZE words |
| Kernel data (ready lists, etc.) | ~200 | Scales with configMAX_PRIORITIES |
| Global variables / BSS | ~200 | Application-dependent |
| **Total** | **~2176** | Leaving ~1920 bytes for code/data |

**FreeRTOSConfig.h for minimum footprint:**

```c
#define configUSE_PREEMPTION               1
#define configUSE_IDLE_HOOK                0
#define configUSE_TICK_HOOK                0
#define configCPU_CLOCK_HZ                 ( 48000000UL )
#define configTICK_RATE_HZ                 ( 100 )     /* 10 ms tick: lower rate = less overhead */
#define configMAX_PRIORITIES               ( 3 )       /* 3 levels: minimum practical */
#define configMINIMAL_STACK_SIZE           ( 128 )     /* 512 bytes — tune with watermark */
#define configTOTAL_HEAP_SIZE              ( 0 )       /* static only, no heap */
#define configMAX_TASK_NAME_LEN            ( 8 )       /* short names save TCB RAM */
#define configUSE_16_BIT_TICKS             0
#define configIDLE_SHOULD_YIELD            1
#define configUSE_TASK_NOTIFICATIONS       1           /* free — built into TCB */
#define configUSE_MUTEXES                  0           /* disable if not needed */
#define configUSE_RECURSIVE_MUTEXES        0
#define configUSE_COUNTING_SEMAPHORES      0
#define configUSE_TIMERS                   0           /* saves timer daemon task */
#define configUSE_EVENT_GROUPS             0
#define configUSE_QUEUE_SETS               0
#define configUSE_CO_ROUTINES              0
#define configSUPPORT_DYNAMIC_ALLOCATION   0
#define configSUPPORT_STATIC_ALLOCATION    1
#define configCHECK_FOR_STACK_OVERFLOW     1           /* minimal overhead check */
#define configGENERATE_RUN_TIME_STATS      0
#define configUSE_STATS_FORMATTING_FUNCTIONS 0
#define configUSE_TRACE_FACILITY           0
```

**Task creation with static allocation:**

```c
static StaticTask_t xTask1TCB, xTask2TCB, xIdleTCB;
static StackType_t  xTask1Stack[128], xTask2Stack[128], xIdleStack[128];

void create_tasks(void)
{
    xTaskCreateStatic(task1_func, "T1", 128, NULL, 2, xTask1Stack, &xTask1TCB);
    xTaskCreateStatic(task2_func, "T2", 128, NULL, 1, xTask2Stack, &xTask2TCB);
}

/* Mandatory when configSUPPORT_STATIC_ALLOCATION = 1 */
void vApplicationGetIdleTaskMemory(StaticTask_t **ppxTCB,
                                   StackType_t  **ppxStack,
                                   uint32_t      *pulSize)
{
    *ppxTCB   = &xIdleTCB;
    *ppxStack = xIdleStack;
    *pulSize  = 128;
}
```

**Trade-offs made:**
- `configMAX_PRIORITIES = 3`: each priority level uses one ready list (intrusive list, ~12 bytes). Fewer levels = less kernel overhead but less scheduling flexibility.
- No timers: saves one daemon task (~80 + 512 = 592 bytes minimum).
- No event groups or queue sets: these kernel objects are unused — disabling them removes the implementation from the build entirely.
- `configTICK_RATE_HZ = 100`: 10 ms resolution is sufficient for many IoT applications. The tick ISR runs 10x less frequently than at 1 kHz, saving CPU and reducing interrupt overhead.
- Static allocation: eliminates all heap overhead, makes memory usage statically verifiable.
