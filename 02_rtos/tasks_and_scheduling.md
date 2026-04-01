# Tasks and Scheduling

## Overview

Scheduling is the algorithm by which an RTOS decides which ready task runs on the CPU at any given moment. The choice of scheduling policy, priority assignment, and task design directly determines whether a system meets its timing requirements. Understanding scheduling theory is essential for proving real-time correctness and for diagnosing latency and starvation problems in production systems.

---

## Key Concepts

### Preemptive vs Cooperative Scheduling

**Preemptive scheduling**: the scheduler can interrupt a running task at any time and switch to a higher-priority ready task without the running task's co-operation. On a tick interrupt or when an ISR makes a higher-priority task ready, the context switch happens immediately.

```
Tick N:   Task B (priority 2) running
           |
           | ISR fires, gives semaphore, Task A (priority 5) becomes Ready
           v
Tick N+ε: PendSV fires, context switch to Task A
           Task B is preempted mid-execution
```

- Advantages: lower worst-case response time for high-priority tasks; natural expression of urgency through priority.
- Disadvantages: shared data between tasks requires synchronisation; context switch overhead on every tick.

**Cooperative scheduling**: a running task voluntarily yields the CPU by calling `taskYIELD()`, `vTaskDelay()`, or a blocking primitive. The scheduler never interrupts a running task mid-execution.

```
Task B running -> calls taskYIELD() -> scheduler checks ready list
               -> Task A (higher priority) runs
               -> Task A calls taskYIELD() -> Task B resumes
```

- Advantages: no shared-data race conditions within a task's non-yielding section; simpler reasoning.
- Disadvantages: a task that never yields starves all others; worst-case response time equals the duration of the longest non-yielding task section; unsuitable for hard real-time requirements.

Most production RTOS configurations use **preemptive scheduling with time-slicing** at the same priority level:

```c
/* FreeRTOSConfig.h */
#define configUSE_PREEMPTION        1   /* preemptive */
#define configUSE_TIME_SLICING      1   /* round-robin within same priority */
```

---

### Priority Assignment

FreeRTOS uses a numerical priority where **higher numbers mean higher priority** (unlike some RTOSes where 0 is highest). Priority 0 is reserved for the idle task.

```c
/* Task priorities */
#define PRIORITY_IDLE          0    /* reserved for idle task */
#define PRIORITY_LOGGING       1
#define PRIORITY_COMMS         2
#define PRIORITY_CONTROL_LOOP  3
#define PRIORITY_SAFETY_MONITOR 4

xTaskCreate(control_task, "CTRL", 512, NULL, PRIORITY_CONTROL_LOOP, NULL);
xTaskCreate(comms_task,   "COMM", 512, NULL, PRIORITY_COMMS,        NULL);
```

**Guidelines for priority assignment:**
1. Assign higher priorities to tasks with shorter periods and tighter deadlines.
2. Interrupt-driven tasks (sensor reads, communication) are typically higher priority than processing tasks.
3. Logging and telemetry should be the lowest priority (they only run when everything else is idle).
4. Leave a gap between priority levels so future tasks can be inserted without reshuffling.
5. Never starve lower-priority tasks — verify they have CPU time in the worst case.

---

### Rate Monotonic Analysis (RMA)

Rate Monotonic Analysis is the classic theoretical framework for proving that a set of periodic tasks will always meet their deadlines under preemptive fixed-priority scheduling.

**Assumptions (Liu & Layland model):**
- Tasks are periodic with hard deadlines equal to their periods.
- Tasks are independent (no shared resources with blocking).
- Task execution time is constant and known.
- Context switch overhead is negligible.
- All tasks are ready at time 0.

**Rate Monotonic Scheduling (RMS) rule**: assign priority inversely proportional to period — the task with the shortest period gets the highest priority.

| Task | Period T | Execution Time C | Utilisation C/T |
|---|---|---|---|
| T1 | 5 ms | 1 ms | 0.20 |
| T2 | 10 ms | 3 ms | 0.30 |
| T3 | 20 ms | 4 ms | 0.20 |
| **Total** | | | **0.70** |

**Utilisation bound (Liu & Layland, 1973):** for n tasks, all deadlines are guaranteed to be met if:

```
U = sum(Ci/Ti) <= n * (2^(1/n) - 1)
```

For n = 1: U <= 1.0
For n = 2: U <= 0.828
For n = 3: U <= 0.780
For n -> inf: U -> ln(2) ≈ 0.693

So for 3 tasks, U = 0.70 < 0.780 — **all deadlines are guaranteed**.

Important: the utilisation bound is a **sufficient** condition, not a necessary one. Tasks with U > bound may still be schedulable — use response time analysis (below) to check definitively.

**Schedulability is not guaranteed** if utilisation exceeds the bound, but it does not immediately fail either. Exact analysis is needed.

---

### Response Time Analysis

Response time analysis (RTA) computes the worst-case response time of each task by accounting for preemption from higher-priority tasks.

The worst-case response time of task i is the smallest R that satisfies:

```
R_i = C_i + sum_{j in hp(i)} ceil(R_i / T_j) * C_j
```

Where:
- `C_i` = execution time of task i
- `hp(i)` = set of tasks with higher priority than i
- `T_j` = period of higher-priority task j
- `C_j` = execution time of higher-priority task j

Solve iteratively starting with `R_i^(0) = C_i`:

```
R_i^(k+1) = C_i + sum_{j in hp(i)} ceil(R_i^(k) / T_j) * C_j
```

Repeat until `R_i^(k+1) == R_i^(k)` (converged). If R_i > T_i (deadline missed) or the iteration does not converge, the task set is not schedulable at this priority assignment.

**Example with the 3-task set above (T1 > T2 > T3 priority):**

Task T1 (highest priority): `R1 = C1 = 1 ms` <= T1 = 5 ms. OK.

Task T2: `R2 = 3 + ceil(R2/5)*1`
- Iteration 0: `R2 = 3`
- Iteration 1: `R2 = 3 + ceil(3/5)*1 = 3 + 1 = 4`
- Iteration 2: `R2 = 3 + ceil(4/5)*1 = 3 + 1 = 4` (converged)
- R2 = 4 ms <= T2 = 10 ms. OK.

Task T3: `R3 = 4 + ceil(R3/5)*1 + ceil(R3/10)*3`
- Iteration 0: `R3 = 4`
- Iteration 1: `R3 = 4 + ceil(4/5)*1 + ceil(4/10)*3 = 4 + 1 + 3 = 8`
- Iteration 2: `R3 = 4 + ceil(8/5)*1 + ceil(8/10)*3 = 4 + 2 + 3 = 9`
- Iteration 3: `R3 = 4 + ceil(9/5)*1 + ceil(9/10)*3 = 4 + 2 + 3 = 9` (converged)
- R3 = 9 ms <= T3 = 20 ms. OK.

All tasks meet their deadlines.

---

### Deadline Monotonic Scheduling

When task deadlines are less than their periods (D < T), Rate Monotonic priority assignment is no longer optimal. **Deadline Monotonic Scheduling** assigns priority inversely proportional to the relative deadline:

```
Shorter deadline -> higher priority
```

For D == T, Deadline Monotonic reduces to Rate Monotonic.

---

### Earliest Deadline First (EDF)

EDF is a **dynamic priority** scheduling algorithm: at every scheduling point, the task with the earliest absolute deadline runs. EDF is optimal for uniprocessor scheduling — it can achieve up to 100% utilisation while meeting all deadlines (vs 69% for RMS with many tasks).

**Why EDF is rarely used in embedded RTOSes:**
- Dynamic priority requires re-sorting the ready list on every scheduling decision — O(n) overhead vs O(1) for fixed-priority (with priority bitmap).
- Handling overload is complex: when U > 1, EDF causes "domino effect" — all tasks miss deadlines unpredictably.
- Harder to reason about system behaviour under debugging conditions.
- Fixed-priority systems are easier to certify (MISRA, DO-178C, IEC 61508).

FreeRTOS and most embedded RTOSes use fixed-priority preemptive scheduling.

---

### Task Design Principles

**1. Tasks should spend most time blocked**

A well-designed task blocks on an event (semaphore, queue, notification) and only runs when it has real work to do. A task that busy-polls wastes CPU and starves lower-priority tasks.

```c
/* Bad: busy-poll */
void sensor_task(void *pvParameters)
{
    for (;;) {
        if (new_data_available()) {      /* spins checking a flag */
            process_sensor_data();
        }
    }
}

/* Good: event-driven */
void sensor_task(void *pvParameters)
{
    for (;;) {
        xSemaphoreTake(xDataReadySemaphore, portMAX_DELAY);  /* blocks */
        process_sensor_data();
    }
}
```

**2. Stack sizing**

Every task needs its own stack. Under-sizing causes stack overflow; over-sizing wastes RAM (precious in embedded systems).

```c
/* Measure stack usage at runtime */
UBaseType_t uxHighWaterMark = uxTaskGetStackHighWaterMark(NULL);
/* uxHighWaterMark == minimum free stack words ever seen for this task */
/* If this approaches 0, increase the stack size */
```

Start with a conservative estimate, enable stack overflow detection during development (`configCHECK_FOR_STACK_OVERFLOW = 2`), then use `uxTaskGetStackHighWaterMark` to tune.

**3. Keep ISRs short**

ISRs should do minimal work: read a hardware register, post to a queue or give a semaphore, set a flag, then return. Defer all processing to a task. This keeps interrupt latency low for other interrupts.

```c
/* ISR: short, posts data to queue */
void USART1_IRQHandler(void)
{
    char c = USART1->DR;
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xQueueSendFromISR(xRxQueue, &c, &xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

/* Task: does the actual processing */
void uart_rx_task(void *pvParameters)
{
    char c;
    for (;;) {
        xQueueReceive(xRxQueue, &c, portMAX_DELAY);
        process_character(c);
    }
}
```

**4. Avoid blocking in high-priority tasks**

High-priority tasks that block for long periods reduce their effective priority — they are unavailable when events arrive. High-priority tasks should have short, bounded execution times.

**5. Single responsibility per task**

Each task should have one clearly defined function. Mixing unrelated concerns in a single task makes priority assignment difficult and increases coupling.

---

## Interview Questions

### Fundamentals Tier

---

**Q1. What is the difference between preemptive and cooperative scheduling? Which does FreeRTOS use by default?**

**Answer:**

In **preemptive scheduling**, the kernel can switch away from a running task at any time — typically on each tick interrupt or when a higher-priority task becomes ready. The running task has no say in when it loses the CPU. This ensures high-priority tasks get the CPU promptly.

In **cooperative scheduling**, the running task must explicitly yield the CPU by calling a yielding function. No involuntary preemption occurs. Simpler but unsuitable for hard real-time because a misbehaving task can starve all others.

FreeRTOS uses preemptive scheduling by default (`configUSE_PREEMPTION = 1`). With `configUSE_TIME_SLICING = 1` (also default), tasks at the same priority level share the CPU in round-robin fashion, each getting one tick before being switched out.

Cooperative mode is available (`configUSE_PREEMPTION = 0`) but rarely used in production; it is mainly useful for porting applications originally written for cooperative systems or for early development on platforms without a reliable tick source.

---

**Q2. Given three periodic tasks with periods 4 ms, 6 ms, and 12 ms, what priorities should they be assigned under Rate Monotonic Scheduling and why?**

**Answer:**

Rate Monotonic Scheduling assigns higher priority to tasks with shorter periods. Ranking by period:

| Task | Period | Priority Assignment |
|---|---|---|
| T1 | 4 ms | Highest (3) |
| T2 | 6 ms | Medium (2) |
| T3 | 12 ms | Lowest (1) |

Rationale: T1 has the tightest deadline (it must complete every 4 ms). If it is at a lower priority than T3, T3 could preempt T1 and cause T1 to miss its deadline. RMS is provably optimal for fixed-priority preemptive scheduling when all deadlines equal periods — no other fixed-priority assignment can schedule a task set that RMS cannot.

Utilisation check (assuming minimal execution times): if `sum(Ci/Ti) <= 3*(2^(1/3)-1) = 0.780`, all deadlines are guaranteed. The actual check requires knowing each task's worst-case execution time.

---

**Q3. What is CPU utilisation in the context of RTOS scheduling, and what happens when it exceeds 100%?**

**Answer:**

CPU utilisation is the fraction of time the CPU is doing useful task work rather than sitting in the idle task. For a set of periodic tasks:

```
U = sum(Ci / Ti)
```

Where Ci is the worst-case execution time of task i and Ti is its period.

When U > 1.0 (100%), the system is **overloaded** — the CPU cannot complete all task instances before their next deadlines. In fixed-priority scheduling under overload:

- High-priority tasks continue to meet their deadlines (they always preempt lower-priority tasks).
- Low-priority tasks miss their deadlines and may never run at all (starvation).
- The system does not necessarily crash — it just fails to meet its timing specification.

In practice, U > 0.7 should trigger a design review. Budget overhead for context switches, ISR processing, and blocking time. Always leave headroom (~20-30%) for unexpected spikes and future feature additions.

---

**Q4. Why should ISR handlers be kept as short as possible, and how do you defer work to a task?**

**Answer:**

ISRs run at a higher privilege level than tasks and often run with all lower-priority interrupts blocked. A long ISR:
1. Increases interrupt latency for other IRQs at the same or lower priority.
2. Prevents the RTOS from running any tasks until the ISR completes.
3. Prevents context switches — `portYIELD_FROM_ISR` triggers a context switch only after the ISR returns.
4. If the ISR takes longer than the tick period, tick events are lost, corrupting time tracking.

The standard pattern is **deferred interrupt processing**: the ISR signals a task (via queue, semaphore, or task notification), and the task does the heavy processing:

```c
/* ISR: reads hardware, sends data to task */
void ADC_IRQHandler(void)
{
    uint16_t sample = ADC1->DR;
    BaseType_t xWoken = pdFALSE;
    xQueueSendFromISR(xAdcQueue, &sample, &xWoken);
    portYIELD_FROM_ISR(xWoken);   /* triggers PendSV if task priority > current task */
}

/* Task: processes ADC samples */
void adc_processing_task(void *pvParameters)
{
    uint16_t sample;
    for (;;) {
        xQueueReceive(xAdcQueue, &sample, portMAX_DELAY);
        apply_filter(sample);
        update_display(sample);
    }
}
```

---

### Intermediate Tier

---

**Q5. Calculate the worst-case response time for a task set with three tasks: T1 (C=1, T=4), T2 (C=2, T=6), T3 (C=3, T=12), assigned priorities T1 > T2 > T3.**

**Answer:**

Using the response time equation:
```
R_i = C_i + sum_{j in hp(i)} ceil(R_i / T_j) * C_j
```

**T1 (highest priority, no higher-priority tasks):**
R1 = C1 = 1 ms. Since 1 <= 4, T1 meets its deadline.

**T2 (preempted only by T1):**
- R2^0 = C2 = 2
- R2^1 = 2 + ceil(2/4)*1 = 2 + 1 = 3
- R2^2 = 2 + ceil(3/4)*1 = 2 + 1 = 3 (converged)
- R2 = 3 ms. Since 3 <= 6, T2 meets its deadline.

**T3 (preempted by T1 and T2):**
- R3^0 = C3 = 3
- R3^1 = 3 + ceil(3/4)*1 + ceil(3/6)*2 = 3 + 1 + 2 = 6
- R3^2 = 3 + ceil(6/4)*1 + ceil(6/6)*2 = 3 + 2 + 2 = 7
- R3^3 = 3 + ceil(7/4)*1 + ceil(7/6)*2 = 3 + 2 + 4 = 9
- R3^4 = 3 + ceil(9/4)*1 + ceil(9/6)*2 = 3 + 3 + 4 = 10
- R3^5 = 3 + ceil(10/4)*1 + ceil(10/6)*2 = 3 + 3 + 4 = 10 (converged)
- R3 = 10 ms. Since 10 <= 12, T3 meets its deadline.

All tasks schedulable. The total utilisation is 1/4 + 2/6 + 3/12 = 0.25 + 0.33 + 0.25 = 0.83, which exceeds the Liu & Layland bound of 0.780 for 3 tasks — demonstrating that the bound is sufficient but not necessary.

---

**Q6. A control loop runs every 5 ms but you observe that it sometimes executes at 6 ms or 7 ms intervals. What are the likely causes and how do you diagnose them?**

**Answer:**

The task uses `vTaskDelay(pdMS_TO_TICKS(5))` instead of `vTaskDelayUntil` — the delay is measured from the end of the task body, so execution time variation accumulates as period jitter. Fix: switch to `vTaskDelayUntil`.

If `vTaskDelayUntil` is already in use:

1. **Tick resolution**: with a 1 kHz tick, delays are quantised to 1 ms. A 5 ms delay nominally fires at 5, 10, 15 ms ticks but actual execution depends on when the scheduler runs — minimum jitter is one tick (1 ms).

2. **Higher-priority task blocking the scheduler**: if a task at higher priority runs for >1 ms, the control task wakes on time but cannot actually execute until the higher-priority task blocks. Diagnose: use a logic analyser with RTOS-aware trace (Segger SystemView or similar) to see which task was running when the control task should have woken.

3. **ISR blocking the scheduler**: a long ISR or masked interrupts (`taskENTER_CRITICAL`) delay the tick ISR. Diagnose: instrument the start of the control task function with a GPIO toggle and measure on an oscilloscope.

4. **Stack overflow**: corrupted TCB data can cause erratic scheduling. Check `uxTaskGetStackHighWaterMark`.

5. **Tick drift from tickless idle**: if tickless idle is enabled, `vTaskStepTick` may correct by fewer or more ticks than the actual sleep duration if the hardware timer has limited resolution.

---

**Q7. Explain what "hyperperiod" means and why it matters for schedulability analysis.**

**Answer:**

The **hyperperiod** is the Least Common Multiple (LCM) of all task periods. It is the time after which the entire task activation pattern repeats identically.

For the task set T1 (period 4), T2 (period 6), T3 (period 12):
```
LCM(4, 6, 12) = 12 ms
```

The hyperperiod matters because:

1. **Worst-case analysis window**: The worst-case response time of any task occurs within the first hyperperiod (specifically, in the "critical instant" when all tasks are simultaneously activated — time 0). You only need to simulate one hyperperiod to determine schedulability.

2. **Simulation complexity**: If the hyperperiod is very large (e.g., tasks with periods 100 ms and 101 ms have a hyperperiod of 10,100 ms), simulation-based schedulability analysis becomes expensive. This is why tasks are often assigned periods that are harmonically related (each period divides the next): e.g., 10, 20, 40 ms — hyperperiod = 40 ms.

3. **Memory for time-triggered scheduling**: in time-triggered architectures, the schedule table has one entry per task activation within the hyperperiod. A large hyperperiod means a large table.

**Critical instant theorem (Liu & Layland)**: the worst case for a task's response time is when all tasks are released simultaneously at time 0. This is the starting point for response time analysis.

---

### Advanced Tier

---

**Q8. Your system has 5 tasks with utilisations 0.15, 0.20, 0.18, 0.22, 0.12 (total 0.87). The Liu & Layland bound for 5 tasks is 0.743. Can you guarantee all deadlines are met? If not, what can you do?**

**Answer:**

The Liu & Layland bound is a **sufficient** condition. Exceeding it does not prove deadlines are missed — it only removes the theoretical guarantee. The task set may still be schedulable.

Steps to determine actual schedulability:

1. **Assign RMS priorities** (shortest period = highest priority).
2. **Run response time analysis** (RTA) for each task using the iterative formula. If all R_i <= T_i, the set is schedulable.
3. If RTA shows a task misses its deadline, the set is genuinely not schedulable at this priority assignment.

If the set is not schedulable:

1. **Reduce execution times**: profile tasks, optimise hot paths, use hardware acceleration (DMA, FPU).
2. **Increase periods**: if the application allows less frequent sampling or processing, increase T_i to reduce utilisation.
3. **Move to a faster CPU**: directly reduces all C_i proportionally.
4. **Change the architecture**: decompose a high-utilisation task into two tasks — a short high-priority task that handles the deadline-critical portion, and a longer low-priority task for the rest.
5. **Use EDF**: EDF is theoretically optimal and can schedule sets with U up to 1.0. However, the engineering trade-offs of EDF (see Key Concepts above) often outweigh the utilisation benefit.
6. **Use multi-core**: offload tasks to a second core. Multiprocessor scheduling theory is more complex (partitioned vs global scheduling).

---

**Q9. Describe how you would design and validate the stack size for a FreeRTOS task in a production embedded system.**

**Answer:**

**Design phase:**
1. Estimate the call graph depth: trace the deepest call chain from the task function, summing local variable sizes and function call overhead at each level.
2. Account for interrupt stack frame: on Cortex-M with `configUSE_TASK_FPU_SUPPORT`, an interrupt on top of the task can push up to 68 bytes (without FPU) or 232 bytes (with FPU, lazy stacking) onto the task stack.
3. Add a margin: 20-25% overhead for compiler-generated temporaries, alignment padding, and unforeseen recursive depth.
4. Convert to words: FreeRTOS stack sizes are in words (4 bytes on 32-bit), not bytes.

**Development phase:**
```c
/* Enable runtime stack overflow detection (method 2 paints stack with 0xA5 pattern
   and checks last N words for corruption on every context switch) */
#define configCHECK_FOR_STACK_OVERFLOW    2

/* Supply the hook — called when overflow detected */
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    /* Log, assert, or enter safe state */
    configASSERT(0);
}

/* Periodically read high water mark */
UBaseType_t uxMark = uxTaskGetStackHighWaterMark(xMyTaskHandle);
/* uxMark == minimum free stack words remaining. If < 10, resize. */
```

**Validation phase:**
1. Run worst-case scenarios: maximum message sizes, deepest code paths, concurrent interrupts.
2. Inject deliberately deep call stacks in unit tests.
3. On ARM: use a stack painting tool (write known pattern to entire stack at startup, read back to find watermark without RTOS overhead).
4. For safety-critical systems (IEC 61508, ISO 26262): stack sizing analysis must be documented and reviewed; a formal worst-case stack usage (WCSU) tool (e.g., from AbsInt) may be required.

**Production:**
- Keep `configCHECK_FOR_STACK_OVERFLOW = 2` enabled even in release builds for safety-critical systems — the overhead is one `memcmp` per context switch.
- Never reduce margins below 20% of peak usage.
