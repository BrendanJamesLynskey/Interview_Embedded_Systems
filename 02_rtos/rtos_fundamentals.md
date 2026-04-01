# RTOS Fundamentals

## Overview

A Real-Time Operating System (RTOS) is a specialised OS designed to process data and events within predictable, bounded time constraints. Unlike a general-purpose OS, which optimises for throughput, an RTOS prioritises **determinism** — the guarantee that a task will be scheduled and complete within a defined deadline.

Understanding RTOS internals is essential for embedded systems roles at companies building safety-critical, industrial, automotive, and consumer IoT products.

---

## Key Concepts

### What Makes an OS "Real-Time"

A real-time system is one where correctness depends not only on the logical result of a computation but also on the time at which that result is produced.

| Property | General-Purpose OS | RTOS |
|---|---|---|
| Scheduling goal | Throughput / fairness | Deterministic deadline adherence |
| Context-switch latency | Milliseconds (variable) | Microseconds (bounded) |
| Interrupt latency | Variable (tens of µs) | Bounded (single-digit µs typical) |
| Memory allocation | Dynamic, can fragment | Often static or deterministic allocators |
| Examples | Linux, Windows, macOS | FreeRTOS, Zephyr, ThreadX, VxWorks, RTEMS |

Two classes:
- **Hard real-time**: missing a deadline is a system failure (ABS brake control, pacemaker, airbag).
- **Soft real-time**: missing a deadline degrades quality but is not catastrophic (audio streaming, UI updates).

---

### Task States

An RTOS task (also called a thread) moves through a well-defined state machine managed by the kernel.

```
         xTaskCreate()
              |
              v
         [SUSPENDED] <------+-------> [BLOCKED]
              |             |              |
    vTaskResume()      vTaskSuspend()  timeout / event
              |             |              |
              v             |              v
           [READY] <--------+---------> [READY]
              |
      kernel selects highest-priority ready task
              |
              v
          [RUNNING]
              |
      preemption / yield / blocking call
              |
              v
           [READY] / [BLOCKED] / [SUSPENDED]
```

| State | Description | FreeRTOS equivalent |
|---|---|---|
| **Ready** | Task is eligible to run; waiting for CPU | `eReady` |
| **Running** | Task is executing on the CPU | `eRunning` |
| **Blocked** | Task is waiting for an event, semaphore, queue item, or timeout | `eBlocked` |
| **Suspended** | Task has been explicitly suspended; not schedulable until resumed | `eSuspended` |
| **Deleted** | Task has been deleted; its stack and TCB will be reclaimed | `eDeleted` |

**Key transitions:**
- `Ready -> Running`: scheduler selects this task (highest priority ready task).
- `Running -> Ready`: a higher-priority task becomes ready (preemption), or task voluntarily yields.
- `Running -> Blocked`: task calls a blocking API (queue receive with timeout, semaphore take, delay).
- `Blocked -> Ready`: event arrives, semaphore given, or timeout expires.
- `Running -> Suspended`: `vTaskSuspend()` called on the running task or from another task.
- `Suspended -> Ready`: `vTaskResume()` called on the task.

---

### The Tick Timer and Time Management

The RTOS tick is the heartbeat of the kernel. A hardware timer interrupt fires at a fixed rate (the **tick frequency**, commonly 1 kHz = 1 ms per tick), incrementing a global tick counter.

**Responsibilities of the tick ISR:**
1. Increment the tick count.
2. Unblock any tasks whose timeout has expired.
3. Trigger the scheduler to check whether a context switch is needed.

```c
/* FreeRTOS: tick frequency configuration in FreeRTOSConfig.h */
#define configTICK_RATE_HZ    ( ( TickType_t ) 1000 )   /* 1 ms per tick */

/* Converting time to ticks */
#define pdMS_TO_TICKS(xTimeInMs) \
    ( ( TickType_t ) ( ( ( TickType_t ) ( xTimeInMs ) * \
                         ( TickType_t ) configTICK_RATE_HZ ) / \
                       ( TickType_t ) 1000U ) )

vTaskDelay( pdMS_TO_TICKS(100) );   /* block for 100 ms */
```

**Tick overhead tradeoff:**
- Higher tick rate: finer time resolution, more frequent scheduler invocations, more CPU overhead.
- Lower tick rate: coarser resolution, lower overhead — suitable when tasks have loose timing requirements.
- Tickless idle: advanced RTOSes suppress the tick when only the idle task is running, saving power on battery-constrained devices.

**`vTaskDelay` vs `vTaskDelayUntil`:**

```c
/* vTaskDelay: delays for N ticks FROM NOW.
   If the task body takes variable time, the period drifts. */
void drifting_task(void *pvParameters)
{
    for (;;) {
        do_work();                     /* variable duration */
        vTaskDelay( pdMS_TO_TICKS(10) );  /* 10 ms from end of do_work() */
    }
}

/* vTaskDelayUntil: delays UNTIL a fixed future tick.
   The period is held precisely even if do_work() varies.
   Correct for periodic tasks. */
void periodic_task(void *pvParameters)
{
    TickType_t xLastWakeTime = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil( &xLastWakeTime, pdMS_TO_TICKS(10) );
        do_work();
    }
}
```

---

### The Task Control Block (TCB)

Every task is represented internally by a Task Control Block — a kernel data structure holding all information the scheduler needs to manage the task.

Typical TCB contents:
- **Stack pointer**: saved CPU register state when the task is not running.
- **Task priority**: numerical priority level.
- **Task state**: ready, blocked, suspended, etc.
- **Task name**: ASCII string for debugging.
- **Stack base and size**: for stack overflow detection.
- **List entries**: used to place the task on ready lists, delayed lists, or event lists.
- **Notification values**: task notification state and value (FreeRTOS).
- **Runtime statistics**: total CPU time used (optional, for profiling).

The TCB is allocated from the heap (or statically) when the task is created. The scheduler maintains separate lists of TCBs for each state and priority.

---

### Kernel Services

An RTOS kernel provides a set of primitive services that tasks use to coordinate and communicate.

| Service | Purpose | FreeRTOS API example |
|---|---|---|
| Task management | Create, delete, suspend, resume tasks | `xTaskCreate`, `vTaskDelete` |
| Delays | Yield CPU for a time period | `vTaskDelay`, `vTaskDelayUntil` |
| Semaphores | Signalling between tasks and ISRs | `xSemaphoreGive`, `xSemaphoreTake` |
| Mutexes | Mutual exclusion with priority inheritance | `xSemaphoreCreateMutex` |
| Queues | Pass data between tasks/ISRs | `xQueueSend`, `xQueueReceive` |
| Event groups | Synchronise on multiple boolean flags | `xEventGroupSetBits` |
| Software timers | Callback after a timeout | `xTimerCreate`, `xTimerStart` |
| Task notifications | Lightweight per-task signalling | `xTaskNotifyGive`, `ulTaskNotifyTake` |
| Memory management | Heap allocation | `pvPortMalloc`, `vPortFree` |

---

### Context Switching

A context switch saves the CPU state of the currently running task (all registers, program counter, stack pointer) into its TCB, then restores the CPU state of the next task to run from its TCB.

On ARM Cortex-M:
- The hardware automatically pushes `r0-r3`, `r12`, `lr`, `pc`, `xpsr` to the task stack on exception entry.
- The RTOS `PendSV` handler (lowest-priority exception) saves the remaining registers (`r4-r11`) and switches the stack pointer to the next task's stack.
- Using `PendSV` for context switching prevents context switches from delaying higher-priority ISRs.

```
Task A running
    |
    | Interrupt fires (higher priority task becomes ready)
    v
Hardware pushes { r0-r3, r12, lr, pc, xpsr } to Task A's stack
PendSV fires:
    software saves { r4-r11, potentially FPU regs } to Task A's stack
    PSP = Task_A_TCB.stack_pointer  (save A's stack pointer)
    Task_B_TCB = scheduler_select_next()
    PSP = Task_B_TCB.stack_pointer  (restore B's stack pointer)
    software restores { r4-r11 } from Task B's stack
Hardware restores { r0-r3, r12, lr, pc, xpsr } from Task B's stack (exception return)
Task B running
```

**Context switch cost** matters: on a Cortex-M4 at 168 MHz, a FreeRTOS context switch takes approximately 100-200 cycles (~1 µs). This must be budgeted against the tick period.

---

### Idle Task and the Idle Hook

The RTOS automatically creates an **idle task** at the lowest possible priority (priority 0 in FreeRTOS). It runs whenever no other task is ready. The idle task:
- Reclaims memory from deleted tasks (if heap is used).
- Calls `vApplicationIdleHook()` if defined.
- Optionally enters low-power sleep via `portSUPPRESS_TICKS_AND_SLEEP()` in tickless mode.

```c
/* FreeRTOSConfig.h */
#define configUSE_IDLE_HOOK    1

/* Application code */
void vApplicationIdleHook(void)
{
    /* Called repeatedly while idle. Must NEVER block or delay.
       Common use: enter low-power sleep mode. */
    __WFI();   /* ARM Wait For Interrupt: CPU halts until next IRQ */
}
```

The idle hook must never call blocking RTOS APIs. It runs at the lowest priority and must always be able to yield.

---

## Interview Questions

### Fundamentals Tier

---

**Q1. What is the difference between a hard real-time and a soft real-time system? Give one example of each.**

**Answer:**

A **hard real-time** system has absolute deadlines. Missing a deadline constitutes a complete system failure, potentially causing physical damage, injury, or death. The correctness of the system is defined both by the logical result and by the time at which it is produced. Examples: ABS braking control (must respond within 1 ms of wheel slip detection), airbag deployment, fly-by-wire flight controls, cardiac pacemakers.

A **soft real-time** system has preferred deadlines. Missing a deadline degrades performance or quality but does not constitute a failure. Examples: video streaming (a late frame causes a momentary stutter), smartphone UI rendering (a late frame causes visible jank but no harm), online gaming (a delayed packet increases latency but the game continues).

A common mistake is calling any embedded system "real-time" because it is fast. Speed is not the same as determinism. A slow system can be hard real-time if its worst-case response time always meets its deadline, and a fast system can be non-real-time if its response time is unbounded.

---

**Q2. Draw and explain the task state diagram for a typical RTOS.**

**Answer:**

The four primary states are Ready, Running, Blocked, and Suspended.

- **Ready**: The task is eligible to run. It has everything it needs except CPU time. The scheduler holds all ready tasks in priority-ordered ready lists.
- **Running**: The task has been given the CPU by the scheduler. On a single-core system, exactly one task is in the Running state at any time.
- **Blocked**: The task is waiting for an external condition — a timeout to expire, a semaphore to be given, a queue item to arrive, or an event group bit to be set. A blocked task consumes no CPU cycles.
- **Suspended**: The task has been explicitly suspended by itself or another task. Unlike Blocked, there is no automatic wake-up condition — the task only becomes Ready when another task calls `vTaskResume()`.

Key transitions:
- Running -> Blocked: task calls any blocking API (e.g., `xSemaphoreTake` with finite timeout).
- Blocked -> Ready: the awaited event occurs or the timeout expires.
- Running -> Ready: preemption by a higher-priority task, or voluntary yield.
- Running/Ready -> Suspended: `vTaskSuspend()` is called.

---

**Q3. What is the RTOS tick, and what are the consequences of setting `configTICK_RATE_HZ` very high (e.g., 10,000 Hz)?**

**Answer:**

The RTOS tick is a periodic hardware timer interrupt that drives all time-based kernel operations: task delays, timeout unblocking, and the scheduler's opportunity to trigger preemption.

Setting the tick rate to 10,000 Hz means the tick ISR fires every 100 µs. Consequences:

1. **Increased CPU overhead**: Every 100 µs the processor is interrupted, saves context, runs the tick ISR (increments tick count, checks timeout list, possibly runs the scheduler), then returns. On a 100 MHz Cortex-M, this ISR might consume 1,000 cycles, which is 1% overhead before any task work is done. At 10,000 Hz with a longer ISR this could reach 5-10%.
2. **Finer time resolution**: `vTaskDelay(1)` blocks for 100 µs instead of 1 ms, allowing more precise timing.
3. **More context switches**: The scheduler runs more frequently, increasing the chance of preemption and the total switching overhead.
4. **More cache/pipeline disruptions**: Each interrupt flushes pipeline state.

For most embedded applications, 100 Hz to 1,000 Hz is sufficient. Use hardware timers directly (not the RTOS tick) for sub-millisecond timing requirements.

---

**Q4. What is a Task Control Block and what information does it contain?**

**Answer:**

A Task Control Block (TCB) is the kernel's data structure representing a single task. It is the RTOS equivalent of a process control block in a general-purpose OS.

Contents typically include:
- **Saved stack pointer**: the value of the CPU stack pointer when the task last context-switched out. Points to the top of the saved register frame on the task's stack.
- **Task priority**: the current priority level. For mutexes with priority inheritance, this can temporarily be different from the base priority.
- **Task name**: a short ASCII string, used in debugging and trace views.
- **Stack limits**: the base address and size of the task's stack, used for overflow checking.
- **State information**: ready/blocked/suspended encoded in which kernel lists the TCB is linked into.
- **List items**: intrusive linked list nodes used to place the task on the ready list, delayed list, or an event object's waiting list without additional heap allocation.
- **Timeout value**: the tick count at which a blocked task should time out and become ready.
- **Notification state**: in FreeRTOS, each task has a built-in notification value used as a lightweight semaphore/event.

The TCB is allocated when the task is created and freed when the task is deleted.

---

**Q5. What is the difference between `vTaskDelay()` and `vTaskDelayUntil()`? When would you use each?**

**Answer:**

`vTaskDelay(N)` blocks the calling task for at least N ticks measured from the moment the call is made. If the task body takes variable time, the actual period of the task drifts:

```
Tick 0:  task wakes, runs for 3 ticks of work
Tick 3:  vTaskDelay(10) called -> unblocks at tick 13
Tick 10: task wakes, runs for 5 ticks of work
Tick 15: vTaskDelay(10) called -> unblocks at tick 25
```
Period varies: 13, 15 ticks. Not periodic.

`vTaskDelayUntil(&xLastWake, N)` blocks until the tick count reaches `xLastWake + N`, then sets `xLastWake = xLastWake + N`. The period is held constant regardless of task body duration:

```
Tick 0:  task wakes (xLastWake=0), runs for 3 ticks
Tick 3:  vTaskDelayUntil called -> unblocks at tick 10
Tick 10: task wakes (xLastWake=10), runs for 5 ticks
Tick 15: vTaskDelayUntil called -> unblocks at tick 20
```
Period is always 10 ticks.

Use `vTaskDelay` for non-periodic delays or retries. Use `vTaskDelayUntil` for any periodic task where jitter accumulation matters (sensor sampling, control loops, periodic communication).

Edge case: if the task body takes longer than the period, `vTaskDelayUntil` will return immediately (it will not block) and will increment `xLastWake` to catch up. This prevents runaway tick accumulation but does mean the task runs more frequently than normal to catch up.

---

### Intermediate Tier

---

**Q6. A task is stuck in the Blocked state indefinitely. List at least four distinct root causes and how you would diagnose each.**

**Answer:**

1. **Semaphore or mutex never given**: the producing task or ISR that should call `xSemaphoreGive()` has crashed, exited, or has a logic bug. Diagnosis: use a debugger or trace tool to inspect the semaphore's owner and the state of the task responsible for giving it.

2. **Queue never receives data**: the sending task is blocked waiting on another resource, creating a circular dependency. Diagnosis: build a dependency graph of which task is waiting for what. Check for deadlock (Task A waits for Task B's semaphore; Task B waits for Task A's queue item).

3. **Timeout set to `portMAX_DELAY`**: the task is waiting indefinitely and the event never comes. Diagnosis: use `xSemaphoreTake` with a finite timeout and add a timeout-path assertion or log message.

4. **Priority starvation**: the task is technically ready (not blocked) but is never scheduled because higher-priority tasks run continuously. This is not technically "blocked" state but produces the same symptom from the application's perspective. Diagnosis: use RTOS runtime stats (`vTaskGetRunTimeStats`) to verify the task's run-time counter is not incrementing.

5. **Event group bits never set**: the task is waiting for multiple bits with `xEventGroupWaitBits` and one of the required bits is set by a task that has died. Diagnosis: inspect the event group value at the time of the hang.

6. **ISR-based wake never fires**: the task is waiting on a semaphore given only from an ISR, and the hardware interrupt is disabled (wrong NVIC priority, `taskDISABLE_INTERRUPTS` left active, peripheral not started). Diagnosis: verify NVIC enable bits and interrupt priority vs `configMAX_SYSCALL_INTERRUPT_PRIORITY`.

---

**Q7. Explain the role of `PendSV` in ARM Cortex-M RTOS implementations.**

**Answer:**

`PendSV` (Pendable Service Call) is a special ARM Cortex-M exception designed specifically for RTOS context switching. It is intentionally configured at the **lowest possible priority** in the NVIC.

Why this matters:

When an ISR determines that a context switch is needed (e.g., an ISR gives a semaphore and a higher-priority task becomes ready), the RTOS sets the `PendSV` pending bit in the SCB ICSR register. The actual context switch does not happen inside the ISR — it is deferred until all higher-priority exceptions have completed and `PendSV` is the highest-priority pending exception.

This design guarantees:
1. ISRs are never delayed by context-switch overhead.
2. If multiple ISRs fire back-to-back, only one context switch happens after all of them complete (tail-chaining of `PendSV`).
3. The context switch only modifies task stacks — not the MSP (main stack) used by interrupt handlers.

```
ISR runs -> gives semaphore -> sets PendSV pending
ISR exits -> PendSV fires (if no higher-priority ISR pending)
PendSV handler: saves r4-r11 to current task stack
                updates PSP (Process Stack Pointer)
                restores r4-r11 from new task stack
PendSV exit: hardware restores r0-r3, r12, lr, pc, xpsr from new task stack
New task runs
```

`SVC` (SuperVisor Call) is used for the initial task start and for privileged kernel calls in systems with an MPU.

---

**Q8. What is priority starvation? How does round-robin scheduling within a priority level help?**

**Answer:**

Priority starvation occurs when a lower-priority task never gets CPU time because one or more higher-priority tasks are always ready (never block). The low-priority task is perpetually preempted.

Example: Task A (priority 5) runs in a tight loop with no blocking calls. Task B (priority 3) never executes.

Solutions:

1. **Fix the design**: well-designed RTOS tasks should block on events rather than busy-polling. A task that has no work to do should block. If Task A is doing genuine continuous work, it may need to yield periodically: `taskYIELD()`.

2. **Round-robin within a priority level**: when multiple tasks share the same priority and are all ready, the scheduler gives each a time slice (one tick in FreeRTOS with `configUSE_TIME_SLICING = 1`). This prevents one task from monopolising the CPU when peers are at the same priority. It does not help when starvation is caused by a *higher*-priority task.

3. **Priority ceiling/inheritance**: not a direct solution for starvation, but proper use of mutexes ensures that priority inversion does not cause inadvertent starvation.

4. **Rate limiting the high-priority task**: insert `vTaskDelay(1)` or `vTaskDelayUntil` in Task A to explicitly yield for at least one tick per iteration.

FreeRTOS with `configUSE_TIME_SLICING = 1` (default): at the end of each tick, if the highest-priority ready list contains more than one task, a round-robin switch occurs between them. Tasks at lower priorities still starve if any higher-priority task is always ready.

---

**Q9. What information is saved and restored during a context switch on a Cortex-M4 with FPU enabled?**

**Answer:**

The Cortex-M4 has lazy FPU context saving to avoid saving FPU registers on every context switch when floating-point is not used.

**Hardware automatically saves on exception entry (to the task stack):**
```
r0, r1, r2, r3, r12, lr (EXC_RETURN), pc, xpsr   — 8 registers (32 bytes)
s0-s15, fpscr                                      — FPU regs (if LSPACT set)
```

**RTOS (PendSV handler) additionally saves:**
```
r4, r5, r6, r7, r8, r9, r10, r11   — callee-saved registers (32 bytes)
s16-s31                             — upper FPU regs (if FPU used by task)
```

In lazy stacking mode (`FPCCR.LSPACT`), the hardware reserves stack space for FPU registers but does not save them until an FPU instruction actually executes in the exception handler. If the ISR uses no FPU instructions, the save never occurs.

FreeRTOS implements `configUSE_TASK_FPU_SUPPORT` which controls whether every task's context includes FPU registers. If enabled, the RTOS save/restore includes `s16-s31` and the hardware covers `s0-s15`.

Total context size with FPU: 17 registers hardware + 8 + 16 software = 33 registers on the stack during a switch.

**Practical implication**: context switches are more expensive when FPU is in use. On bare-metal interrupt handlers that use FPU, the increased latency must be budgeted.

---

### Advanced Tier

---

**Q10. Explain tickless idle mode. What hardware requirement does it impose, and what RTOS changes are needed to implement it?**

**Answer:**

Tickless idle mode suppresses the periodic tick interrupt when the only runnable task is the idle task, allowing the CPU to enter a deep sleep state (e.g., ARM `WFI` or a vendor-specific low-power mode) without being woken at every tick period.

**How it works:**
1. Before entering idle, the scheduler inspects the delay lists to find the time until the next task needs to wake up — the expected idle duration.
2. The tick timer is reprogrammed to fire after that duration (or the maximum achievable sleep period, whichever is shorter).
3. The CPU enters sleep.
4. On wake (either the timer fires or an external interrupt arrives), the scheduler corrects the tick count by the elapsed time and resumes normal operation.

**Hardware requirement:**
A timer that can be programmed to fire at arbitrary intervals and that can report how long it actually slept (in case an earlier interrupt woke the CPU). On Cortex-M, the SysTick timer is typically replaced with an RTC or LPTIM for tickless operation because SysTick stops in deep sleep modes.

**FreeRTOS implementation:**
```c
/* FreeRTOSConfig.h */
#define configUSE_TICKLESS_IDLE    1    /* enable tickless */

/* Application must supply: */
void vPortSuppressTicksAndSleep(TickType_t xExpectedIdleTime)
{
    /* 1. Reprogram hardware timer to fire in xExpectedIdleTime ticks */
    /* 2. Call __WFI() or vendor deep-sleep entry */
    /* 3. On wake, read elapsed time from hardware timer */
    /* 4. Call vTaskStepTick(elapsed) to correct the RTOS tick count */
}
```

**Trade-off**: tickless idle complicates timing accuracy and requires careful handling of the tick count correction. It is essential for battery-powered devices where idle power (CPU running with tick ISR) can be 10x-100x higher than deep sleep power.

---

**Q11. A team is considering using a single RTOS task to handle all application logic in a state machine, versus splitting into multiple tasks. Discuss the engineering trade-offs.**

**Answer:**

**Single-task state machine:**
- Advantages: no synchronisation primitives needed, no inter-task communication overhead, no stack space per task, simpler debugging (single execution context), deterministic execution order.
- Disadvantages: a blocking operation (e.g., waiting for a UART byte) stalls all state machine processing. Must use non-blocking patterns throughout (polling with timeouts, callbacks). Latency for low-priority states is determined by the longest state in the machine, not by priority. Cannot take advantage of multi-core CPUs.

**Multiple tasks:**
- Advantages: each task blocks independently — a UART task can block on a semaphore without affecting the control loop. Priority assignment allows the RTOS to guarantee response times. Easier to reason about one task's invariants in isolation.
- Disadvantages: each task needs a stack (stack sizing is non-trivial), inter-task communication through queues/semaphores adds latency and code complexity, shared data requires synchronisation (risk of priority inversion, deadlock), higher memory footprint.

**Guidelines from practice:**
- Use one task per independent blocking concern: one task per peripheral that can stall, one task per periodic control function.
- Avoid giving every small function its own task — task creation has memory cost (minimum ~500 bytes for TCB + minimal stack in FreeRTOS).
- Group non-blocking, low-frequency work into the idle task or a single low-priority task.
- Use task notifications rather than semaphores for simple one-to-one unblocking — they are faster and have zero extra memory cost.

The canonical embedded architecture: a high-priority control task, a medium-priority communications task, a low-priority logging/telemetry task, and an idle task.
