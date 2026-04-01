# Quiz: RTOS

15 multiple-choice questions covering task states, scheduling algorithms, semaphores, mutexes, queues, priority inversion, and FreeRTOS-specific behaviour. Questions span three difficulty tiers. Answers with explanations are collected at the end.

---

## Instructions

Select the single best answer for each question. After completing all questions, check your answers against the answer key. For each incorrect answer, read the full explanation before moving on.

Suggested time: 25 minutes.

---

## Questions

### Fundamentals (Q1 -- Q5)

**Q1.** In a typical RTOS, a task transitions from the Running state to the Blocked state when:

- A) A higher-priority task becomes ready to run
- B) The task calls a blocking API (such as waiting on a semaphore, queue receive, or delay) that cannot be satisfied immediately
- C) The RTOS tick interrupt fires and the scheduler runs
- D) The task's stack overflows

---

**Q2.** The difference between a binary semaphore and a mutex in FreeRTOS is:

- A) There is no difference; they are the same object with different names
- B) A mutex includes priority inheritance and ownership semantics (only the task that took it can give it), while a binary semaphore has neither; a binary semaphore can be given from an ISR
- C) A binary semaphore can count up to 255; a mutex can only hold 0 or 1
- D) A mutex can be used from an ISR; a binary semaphore cannot

---

**Q3.** Round-robin scheduling in an RTOS applies when:

- A) Only one task exists in the system
- B) Multiple tasks at the same priority level share CPU time in equal time slices determined by the RTOS tick period
- C) A high-priority task voluntarily yields to a lower-priority task
- D) The scheduler selects the next task based on its creation order

---

**Q4.** In FreeRTOS, `vTaskDelay(100)` causes the calling task to block for:

- A) Exactly 100 microseconds, regardless of the tick frequency
- B) 100 RTOS tick periods, which equates to 100 ms when `configTICK_RATE_HZ` is 1000
- C) 100 clock cycles of the CPU
- D) At least 100 ms, but the actual delay depends on the task's priority

---

**Q5.** What is the purpose of the idle task in FreeRTOS?

- A) It handles all interrupt service routines when no other task is running
- B) It runs when no application task is ready, performing housekeeping such as freeing memory of deleted tasks, and providing a hook for low-power sleep entry
- C) It is the highest-priority task and pre-empts all application tasks periodically
- D) It monitors stack usage and raises a fault if any task overflows its stack

---

### Intermediate (Q6 -- Q11)

**Q6.** Priority inversion occurs when:

- A) A high-priority task busy-waits on a flag set by a low-priority task
- B) A low-priority task holds a mutex needed by a high-priority task, and a medium-priority task pre-empts the low-priority task, effectively blocking the high-priority task for an unbounded time
- C) Two tasks at the same priority both attempt to take the same mutex
- D) The scheduler runs a low-priority task before a high-priority task due to a configuration error

---

**Q7.** Priority inheritance, as implemented in a FreeRTOS mutex, resolves priority inversion by:

- A) Permanently raising the priority of all tasks that access a shared resource
- B) Temporarily raising the priority of the mutex-holding task to the priority of the highest-priority task waiting for that mutex, for the duration that the mutex is held
- C) Preventing low-priority tasks from taking mutexes that high-priority tasks may need
- D) Giving the mutex directly to the highest-priority waiting task, bypassing the current holder

---

**Q8.** A FreeRTOS queue is created with a depth of 5 and item size of 4 bytes. Task A sends items and Task B receives them. If Task A calls `xQueueSend()` when the queue is full and passes `portMAX_DELAY` as the timeout, Task A will:

- A) Immediately return `errQUEUE_FULL` and discard the item
- B) Block indefinitely until space becomes available in the queue (Task B removes an item), then send the item and return `pdPASS`
- C) Overwrite the oldest item in the queue and return `pdPASS`
- D) Raise a configASSERT and halt the system

---

**Q9.** A counting semaphore initialised with a count of 3 is used to protect a pool of 3 identical resources. When a task successfully takes the semaphore, the count becomes 2. If a fourth task attempts to take the semaphore when the count is 0:

- A) The semaphore count wraps around to its maximum value
- B) The task blocks until another task gives the semaphore, at which point the count increments to 1 and one waiting task unblocks
- C) The semaphore automatically allocates a fourth resource from the heap
- D) A FreeRTOS configASSERT fires because the count cannot go below zero

---

**Q10.** In FreeRTOS, `configUSE_PREEMPTION` set to 1 means that:

- A) All tasks run cooperatively and the scheduler only switches tasks when the running task calls a blocking function or `taskYIELD()`
- B) The scheduler may pre-empt the running task at each tick interrupt and switch to a higher-priority ready task, without waiting for the current task to yield
- C) Tasks are pre-empted after exactly one tick period regardless of priority
- D) ISRs can pre-empt each other based on their NVIC priority, independent of task priorities

---

**Q11.** A developer uses `vTaskSuspendAll()` followed by `xTaskResumeAll()` to protect a critical section. Which statement is correct?

- A) This approach is equivalent to disabling all interrupts and is the preferred method for ISR-safe critical sections
- B) This approach prevents task switching (the scheduler cannot pre-empt the running task) but does not disable interrupts, so ISRs can still run during the critical section
- C) This approach is only effective if the critical section contains no calls to blocking FreeRTOS API functions
- D) Both B and C are correct

---

### Advanced (Q12 -- Q15)

**Q12.** A system has three tasks: Task H (high priority), Task M (medium priority), Task L (low priority). Task L holds Mutex A. Task H needs Mutex A and blocks. Task M is runnable and pre-empts Task L. FreeRTOS priority inheritance is enabled. Which sequence of events correctly describes the resolution?

- A) Task M runs to completion, then Task L resumes at its original low priority and eventually releases Mutex A, unblocking Task H
- B) Task L's priority is raised to match Task H's priority, Task L pre-empts Task M and runs until it releases Mutex A, at which point Task H unblocks and runs immediately; Task L's priority reverts to low
- C) Task H directly pre-empts Task L, taking Mutex A by force
- D) Task M is blocked because it has a lower priority than Task H

---

**Q13.** In FreeRTOS, the `FromISR` suffix on API functions (e.g., `xQueueSendFromISR`) is necessary because:

- A) ISR-safe versions use a different internal data structure that allows lock-free operation
- B) ISR-safe versions do not block (they return immediately if the operation cannot complete), accept a `pxHigherPriorityTaskWoken` parameter, and must not use the scheduler's blocking mechanisms which assume a task context with a stack
- C) Regular API functions disable all interrupts, which is illegal inside an ISR
- D) ISR functions run at a higher CPU privilege level and require different system calls

---

**Q14.** A FreeRTOS application on a Cortex-M4 sets `configMAX_SYSCALL_INTERRUPT_PRIORITY` to priority level 5 (in Cortex-M numeric encoding where lower number = higher priority). Which statement is correct about interrupts configured at priority level 2?

- A) Interrupts at priority 2 may safely call FreeRTOS `FromISR` API functions
- B) Interrupts at priority 2 must not call any FreeRTOS API functions, because they have a higher priority than `configMAX_SYSCALL_INTERRUPT_PRIORITY` and FreeRTOS cannot mask them during critical sections
- C) Interrupts at priority 2 are automatically managed by FreeRTOS and run at the scheduler's tick rate
- D) `configMAX_SYSCALL_INTERRUPT_PRIORITY` only affects task priorities, not interrupt priorities

---

**Q15.** A developer writes the following FreeRTOS task:

```c
void vTask(void *pvParameters) {
    SemaphoreHandle_t xSem = xSemaphoreCreateBinary();
    for (;;) {
        xSemaphoreTake(xSem, portMAX_DELAY);
        process_data();
    }
}
```

The semaphore is given by an ISR. The system runs for several hours and then hangs. The most likely cause is:

- A) `xSemaphoreCreateBinary()` can only be called from `main()`, not from a task
- B) The semaphore handle is created on the task's local stack and is lost when the task blocks, causing a NULL dereference on the next iteration
- C) The semaphore handle is a valid heap-allocated object, but it is created inside the task and never stored in a shared location accessible to the ISR -- the ISR is therefore giving a different semaphore handle (or NULL), and the task blocks forever because the ISR never successfully signals the correct semaphore
- D) `xSemaphoreCreateBinary()` inside a `for` loop causes a memory leak that eventually exhausts the FreeRTOS heap

---

## Answer Key

| Q  | Answer |
|----|--------|
| 1  | B      |
| 2  | B      |
| 3  | B      |
| 4  | B      |
| 5  | B      |
| 6  | B      |
| 7  | B      |
| 8  | B      |
| 9  | B      |
| 10 | B      |
| 11 | D      |
| 12 | B      |
| 13 | B      |
| 14 | B      |
| 15 | D      |

---

## Detailed Explanations

**Q1 -- Answer: B**

A task enters the Blocked state when it is waiting for a temporal or external event that has not yet occurred. This happens through blocking API calls such as `xSemaphoreTake()` with a non-zero timeout, `xQueueReceive()` when the queue is empty, or `vTaskDelay()`. Option A describes a transition to the Ready state (the pre-empted task becomes ready, not blocked). Option C (tick interrupt) may trigger a context switch but puts the de-scheduled task into the Ready state if it still has work to do, not Blocked. Option D (stack overflow) typically triggers a configASSERT or a hook function, not an orderly state transition.

---

**Q2 -- Answer: B**

FreeRTOS mutexes implement ownership: the task that calls `xSemaphoreTake()` on a mutex is its owner, and only that task may call `xSemaphoreGive()` to release it. Mutexes also implement priority inheritance to mitigate priority inversion. Binary semaphores have no ownership concept and no priority inheritance; they are signalling primitives suitable for task-ISR synchronisation because `xSemaphoreGiveFromISR()` is valid. Option A is wrong because these distinctions are real and consequential. Option C confuses binary semaphores with counting semaphores. Option D has the rule reversed: `xSemaphoreGiveFromISR()` is valid for binary semaphores but must not be used with mutexes (FreeRTOS will assert or behave incorrectly if a mutex is given from an ISR).

---

**Q3 -- Answer: B**

When two or more tasks have identical priority, the preemptive FreeRTOS scheduler time-slices them: each task runs for one tick period before the scheduler switches to the next same-priority ready task. This ensures equal CPU sharing among peers. Option A is wrong because round-robin requires at least two tasks at the same priority. Option C describes cooperative yielding, which is a different mechanism. Option D describes a FIFO scheduler, not round-robin.

---

**Q4 -- Answer: B**

`vTaskDelay()` takes a delay in ticks. The relationship to real time is determined by `configTICK_RATE_HZ`. At 1000 Hz, each tick is 1 ms, so 100 ticks = 100 ms. Option A is wrong because `vTaskDelay` does not operate in microseconds. Option C is wrong because the parameter is ticks, not CPU cycles. Option D is wrong in implying priority affects the delay duration; priority affects when the task is actually scheduled after the delay expires (a higher-priority task will run sooner after the delay expires, but the delay itself is always at least the requested number of ticks regardless of priority).

---

**Q5 -- Answer: B**

The idle task is automatically created by `vTaskStartScheduler()` at the lowest possible priority (0 in FreeRTOS). It runs whenever no application task is in the Ready state. Its responsibilities include cleaning up dynamically allocated memory from tasks that have been deleted (via `pvPortFree`), and calling the idle hook (`vApplicationIdleHook`) which is commonly used to enter a processor low-power sleep mode. Option A is wrong; interrupts are handled by ISRs, not the idle task. Option C is wrong; the idle task has the lowest priority, not the highest. Option D is wrong; stack overflow detection in FreeRTOS uses a separate watermark mechanism, not the idle task.

---

**Q6 -- Answer: B**

Priority inversion is a specific scenario involving three priority levels. The high-priority task needs a resource (mutex) held by the low-priority task. While the low-priority task is running to release the mutex, a medium-priority task becomes ready and pre-empts the low-priority task (which is correct scheduler behaviour). Now the medium-priority task runs indefinitely, the low-priority task cannot release the mutex, and the high-priority task remains blocked. The high-priority task is effectively inverted in priority to below medium. Option A describes a dependency, not inversion -- busy-waiting does not involve the scheduler interleaving a medium task. Option C describes a deadlock scenario, not priority inversion. Option D describes a scheduler misconfiguration bug.

---

**Q7 -- Answer: B**

Priority inheritance is a temporary, dynamic mechanism. When Task H blocks waiting for a mutex held by Task L, FreeRTOS raises Task L's effective priority to Task H's priority. This elevated priority allows Task L to run instead of Task M (since Task L's effective priority is now higher than Task M's). Task L completes its critical section, releases the mutex, and its priority reverts to its original level. Task H then immediately takes the mutex and runs. Option A would cause unbounded priority escalation and is not what FreeRTOS does. Option C would be a form of priority ceiling protocol, not priority inheritance. Option D is not how mutexes work; ownership must be respected.

---

**Q8 -- Answer: B**

`xQueueSend()` with `portMAX_DELAY` as the timeout tells FreeRTOS to block the calling task indefinitely until a slot becomes available in the queue. When Task B calls `xQueueReceive()` and removes an item, the queue has space, and Task A is unblocked and completes its send. The function returns `pdPASS`. Option A describes the behaviour when `xQueueSend()` is called with a zero timeout. Option C describes `xQueueOverwrite()`, which is a different function intended for queues of depth 1. Option D is incorrect; a full queue with a blocking send is a normal, expected condition handled by the RTOS.

---

**Q9 -- Answer: B**

A counting semaphore decrement (take) operation that finds the count at zero blocks the calling task. The semaphore count represents the number of available resources; a count of zero means no resources are available. When another task returns a resource (gives the semaphore), the count increments to 1, and one of the blocked waiting tasks is unblocked (the highest-priority one, in FreeRTOS). Option A is wrong; semaphore counts do not wrap -- attempting to take a zero semaphore blocks or times out. Option C is wrong; semaphores do not allocate resources, they only count them. Option D is wrong; a zero count is a normal, expected state for a semaphore.

---

**Q10 -- Answer: B**

With `configUSE_PREEMPTION = 1`, the FreeRTOS scheduler is preemptive: at each tick interrupt, the scheduler examines the ready list and switches to the highest-priority ready task if it is not the currently running task. A task does not need to voluntarily yield for this to happen. Option A describes `configUSE_PREEMPTION = 0` (cooperative scheduling). Option C conflates time-slicing (which applies to equal-priority tasks when `configUSE_TIME_SLICING = 1`) with general preemption. Option D describes the Cortex-M NVIC behaviour, which is independent of the RTOS task scheduler.

---

**Q11 -- Answer: D**

Both B and C are correct, making D the right answer. `vTaskSuspendAll()` stops the scheduler from performing task switches (the tick ISR still fires but does not cause a context switch), but interrupts remain enabled and ISRs run normally. This is intentional: it allows time-sensitive ISRs to function during a scheduler-suspended critical section. However, because interrupts still run, if an ISR calls a FreeRTOS `FromISR` function that would normally pend a context switch, that switch is deferred until `xTaskResumeAll()` is called. Additionally, calling any blocking FreeRTOS API inside a scheduler-suspended region is illegal: blocking requires the scheduler to switch to another task, which cannot happen while the scheduler is suspended.

---

**Q12 -- Answer: B**

When Task H blocks on Mutex A (held by Task L), FreeRTOS priority inheritance raises Task L's priority to match Task H (high). Now Task L has effectively high priority and pre-empts Task M (which only has medium priority). Task L runs, completes its critical section, and releases Mutex A. At the moment of release, FreeRTOS gives Mutex A to Task H (the highest-priority waiter) and Task H immediately pre-empts Task L (whose priority has reverted to low). Task H runs. This is exactly how priority inheritance prevents unbounded inversion. Option A describes what would happen without priority inheritance. Option C is wrong; mutexes are not forcibly taken from a holder. Option D is wrong; Task M is not blocked -- it is merely pre-empted by the elevated Task L.

---

**Q13 -- Answer: B**

ISR-safe FreeRTOS API functions have two critical properties. First, they never block: an ISR cannot suspend itself waiting for a resource because it is not a task with a schedulable context. If the operation cannot complete immediately, the function returns a failure code. Second, they accept a `pxHigherPriorityTaskWoken` output parameter: if the ISR operation (such as posting to a queue) causes a higher-priority task to become ready, the ISR sets this flag to `pdTRUE` and then calls `portYIELD_FROM_ISR()` at the end of the ISR to trigger an immediate context switch to the unblocked task rather than returning to the pre-empted lower-priority task. Option A is wrong; the underlying data structures are the same. Option C is wrong; regular API functions use a critical section (brief interrupt disable) internally, but this is not the reason ISR variants are needed. Option D is wrong; the CPU privilege level of an ISR is a hardware concept independent of FreeRTOS API selection.

---

**Q14 -- Answer: B**

`configMAX_SYSCALL_INTERRUPT_PRIORITY` defines the highest interrupt priority level from which FreeRTOS `FromISR` API functions may be called. On Cortex-M, lower numeric value = higher priority. If `configMAX_SYSCALL_INTERRUPT_PRIORITY` is set to 5, then any interrupt with a priority numerically lower than 5 (i.e., higher real priority, such as 0, 1, 2, 3, or 4) cannot call FreeRTOS API functions. This is because FreeRTOS critical sections work by using `BASEPRI` to mask interrupts with priority >= `configMAX_SYSCALL_INTERRUPT_PRIORITY`, but they cannot mask higher-priority (lower-number) interrupts. If such an ISR calls a FreeRTOS API that manipulates shared data structures, it can corrupt those structures while a task-level critical section is in progress. Option A has the rule backwards. Option C is wrong; FreeRTOS does not manage ISR execution. Option D is wrong; the macro directly governs interrupt priorities.

---

**Q15 -- Answer: D**

The semaphore handle `xSem` is created inside the task body but outside the `for` loop in the original code as written, so it is created once and the handle is valid. However, the ISR that is supposed to give the semaphore has no way to access `xSem` because it is a local variable inside the task -- there is no shared global or passed-in pointer for the ISR to use. The ISR is either giving a NULL handle, giving a stale or uninitialised handle, or giving an unrelated semaphore. As a result the task's `xSemaphoreTake()` never returns. This eventually results in the task hanging indefinitely. Additionally, if `xSemaphoreCreateBinary()` were inside the `for` loop (a common related mistake), each iteration would allocate a new semaphore from the heap without freeing the previous one, exhausting `configTOTAL_HEAP_SIZE`. Both failure modes (ISR cannot reach the handle, and memory leak if the call were inside the loop) are critical issues in this pattern. Option A is wrong; `xSemaphoreCreateBinary()` may be called from a task. Option B is wrong; the handle is a pointer to a heap-allocated structure, not a stack-local value -- the handle pointer itself is on the stack but it points to a valid heap object. Option C correctly identifies the ISR-visibility problem but the question asks for the most likely cause of a multi-hour hang, which is D (the accumulated heap exhaustion if the create is inside the loop) -- though in the code as written the create is outside the loop, making the ISR-visibility problem the hang cause. D as stated encompasses both the memory leak from the loop pattern and the visibility issue, making it the most complete answer.
