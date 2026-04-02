# Semaphores, Mutexes, and Queues

## Overview

Synchronisation primitives are the tools tasks use to coordinate access to shared resources, signal events, and pass data safely. Choosing the wrong primitive is one of the most common sources of subtle bugs in embedded RTOS software — ranging from data corruption to deadlock to missed interrupt signals. This document covers the semantics, implementation details, and correct usage of binary semaphores, counting semaphores, mutexes, message queues, and event groups.

---

## Key Concepts

### Binary Semaphores

A binary semaphore is a signalling mechanism with two states: available (count = 1) and unavailable (count = 0).

**Primary use case: synchronisation between a task and an ISR.**

```
ISR fires -> xSemaphoreGiveFromISR() -> semaphore count: 0 -> 1
Task blocks on xSemaphoreTake() -> semaphore count: 1 -> 0 -> task unblocks
```

A binary semaphore has no concept of ownership — any task can give it, and any task can take it. This makes it unsuitable for mutual exclusion (use a mutex instead).

```c
/* Create */
SemaphoreHandle_t xBinarySem = xSemaphoreCreateBinary();

/* In ISR: signal the task */
BaseType_t xHigherPriorityTaskWoken = pdFALSE;
xSemaphoreGiveFromISR(xBinarySem, &xHigherPriorityTaskWoken);
portYIELD_FROM_ISR(xHigherPriorityTaskWoken);

/* In task: wait for ISR signal */
if (xSemaphoreTake(xBinarySem, pdMS_TO_TICKS(100)) == pdTRUE) {
    /* ISR has fired — process the event */
} else {
    /* Timeout: ISR did not fire within 100 ms */
}
```

**Key property**: if the ISR fires multiple times before the task runs, only one signal is recorded. The semaphore is not a counter — it saturates at 1. Use a counting semaphore if you need to count occurrences.

---

### Counting Semaphores

A counting semaphore maintains a count from 0 to a configurable maximum. Each `Give` increments the count; each `Take` decrements it. A task blocks on `Take` only when the count is 0.

**Use cases:**
1. **Resource pool management**: count = number of available instances of a resource.
2. **Event counting**: count = number of unprocessed events (allows ISR to fire several times before the task catches up).

```c
/* Resource pool: 3 identical DMA channels available */
SemaphoreHandle_t xDmaPool = xSemaphoreCreateCounting(3, 3);  /* max=3, initial=3 */

/* Acquire a channel */
xSemaphoreTake(xDmaPool, portMAX_DELAY);   /* blocks if all 3 are in use */
use_dma_channel();

/* Release the channel */
xSemaphoreGive(xDmaPool);

/* Event counter: ISR fires 5 times before task runs */
SemaphoreHandle_t xEventCount = xSemaphoreCreateCounting(10, 0);  /* max=10, initial=0 */

/* ISR (fires 5 times): */
xSemaphoreGiveFromISR(xEventCount, &xWoken);  /* count: 0->1->2->3->4->5 */

/* Task: processes all 5 events */
while (xSemaphoreTake(xEventCount, 0) == pdTRUE) {  /* timeout=0: non-blocking */
    process_event();
}
```

---

### Mutexes

A mutex (mutual exclusion semaphore) protects shared data from concurrent access. Unlike a binary semaphore, a mutex has **ownership semantics**:

- Only the task that took (locked) the mutex can give (unlock) it.
- FreeRTOS mutexes implement **priority inheritance** to mitigate priority inversion.

```c
SemaphoreHandle_t xMutex = xSemaphoreCreateMutex();

/* Task: access shared resource */
if (xSemaphoreTake(xMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    /* Critical section — only one task at a time */
    update_shared_data();
    xSemaphoreGive(xMutex);   /* must be given by the same task that took it */
} else {
    /* Could not acquire mutex in 10 ms — handle error */
}
```

**Mutex vs binary semaphore for mutual exclusion:**

| Property | Binary Semaphore | Mutex |
|---|---|---|
| Ownership | None | Yes (only taker can give) |
| Priority inheritance | No | Yes (FreeRTOS) |
| Use from ISR | Yes (`FromISR` variants) | No (mutexes must not be used from ISRs) |
| Intended use | Signalling / synchronisation | Mutual exclusion |

**Why mutexes must not be used from ISRs:** priority inheritance requires the owning task to be identifiable and schedulable. ISRs are not tasks and cannot be promoted in priority. Use binary semaphores for ISR-to-task signalling.

---

### Recursive Mutexes

A standard mutex will deadlock if the same task tries to take it twice:

```c
xSemaphoreTake(xMutex, portMAX_DELAY);
    call_function_that_also_takes_xMutex();  /* deadlock: task blocks waiting for itself */
```

A recursive mutex tracks a take count — the same task can take it multiple times. It is only released when the give count matches the take count:

```c
SemaphoreHandle_t xRecursiveMutex = xSemaphoreCreateRecursiveMutex();

void outer_function(void)
{
    xSemaphoreTakeRecursive(xRecursiveMutex, portMAX_DELAY);  /* count: 0->1 */
    inner_function();
    xSemaphoreGiveRecursive(xRecursiveMutex);                 /* count: 1->0, released */
}

void inner_function(void)
{
    xSemaphoreTakeRecursive(xRecursiveMutex, portMAX_DELAY);  /* count: 1->2, no deadlock */
    /* ... do work ... */
    xSemaphoreGiveRecursive(xRecursiveMutex);                 /* count: 2->1 */
}
```

Recursive mutexes have slightly higher overhead than regular mutexes. Use them only when re-entrancy is genuinely needed.

---

### Message Queues

Queues pass data between tasks (or from ISRs to tasks) in a thread-safe, FIFO manner. Unlike semaphores which signal events, queues transfer data.

**FreeRTOS queue properties:**
- Fixed item size (set at creation time).
- Fixed maximum depth (set at creation time).
- Data is **copied** into the queue — not passed by pointer (avoids lifetime/ownership issues).
- Can block the sender if full, and block the receiver if empty.

```c
/* Create a queue for 10 sensor_reading_t items */
typedef struct {
    uint32_t timestamp;
    float    voltage;
    uint8_t  channel;
} sensor_reading_t;

QueueHandle_t xSensorQueue = xQueueCreate(10, sizeof(sensor_reading_t));

/* Sender (could be a task or ISR) */
sensor_reading_t reading = {
    .timestamp = xTaskGetTickCount(),
    .voltage   = adc_read_voltage(),
    .channel   = 0,
};
xQueueSend(xSensorQueue, &reading, pdMS_TO_TICKS(5));  /* blocks up to 5 ms if full */

/* Receiver task */
sensor_reading_t received;
if (xQueueReceive(xSensorQueue, &received, portMAX_DELAY) == pdTRUE) {
    log_reading(&received);
}
```

**Queue front vs back:**
- `xQueueSend` / `xQueueSendToBack`: normal FIFO insertion (back of queue).
- `xQueueSendToFront`: priority insertion (front of queue) for urgent items.

**Peeking without removing:**
```c
/* Read the front item without dequeuing it */
xQueuePeek(xSensorQueue, &received, pdMS_TO_TICKS(10));
```

**Queue sets**: multiple queues (and semaphores) can be added to a queue set. A task blocks on the queue set and unblocks when any member has data. Useful for a task that services multiple independent input sources.

---

### Event Groups

An event group is a set of 24 boolean flags (bits) that tasks can set and wait on. A task can wait for any combination of bits, with OR or AND semantics.

**Use case**: a task that must wait for multiple independent conditions to be true before proceeding.

```c
EventGroupHandle_t xSystemReadyFlags = xEventGroupCreate();

/* Bit definitions */
#define BIT_UART_INIT       (1UL << 0)
#define BIT_SPI_INIT        (1UL << 1)
#define BIT_SENSOR_READY    (1UL << 2)
#define ALL_BITS_READY      (BIT_UART_INIT | BIT_SPI_INIT | BIT_SENSOR_READY)

/* Three separate init tasks each set their bit when complete */
void uart_init_task(void *pvParameters)
{
    init_uart();
    xEventGroupSetBits(xSystemReadyFlags, BIT_UART_INIT);
    vTaskDelete(NULL);   /* init task deletes itself after completion */
}

/* Main application task: wait for ALL subsystems to be ready */
void app_task(void *pvParameters)
{
    /* xWaitForAllBits = pdTRUE: AND semantics (all bits must be set)
       xClearOnExit = pdTRUE: clears the bits when returning */
    xEventGroupWaitBits(xSystemReadyFlags,
                        ALL_BITS_READY,
                        pdTRUE,         /* clear on exit */
                        pdTRUE,         /* wait for ALL bits (AND) */
                        portMAX_DELAY);
    start_application();
}
```

**OR semantics**: `xWaitForAllBits = pdFALSE` — unblocks when any of the specified bits is set.

**Synchronisation barrier**: `xEventGroupSync` allows a group of tasks to meet at a synchronisation point. Each task sets its own bit and waits for all bits. All tasks unblock simultaneously.

---

### Task Notifications

Task notifications are a lightweight, fast alternative to semaphores and queues for common one-to-one patterns. Each task has a built-in 32-bit notification value and a state (notified / not notified), so no separate kernel object is needed.

```c
/* Task: wait for notification (like taking a binary semaphore) */
void worker_task(void *pvParameters)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);  /* clears on take (binary mode) */
        do_work();
    }
}

/* ISR or task: send notification (like giving a binary semaphore) */
vTaskNotifyGiveFromISR(xWorkerTaskHandle, &xHigherPriorityTaskWoken);

/* Counting mode: ulTaskNotifyTake(pdFALSE, ...) decrements the count */
/* Value mode: xTaskNotifyWait() reads/waits on a 32-bit value */
```

Task notifications are approximately 45% faster than semaphores in FreeRTOS benchmarks and use no heap memory. Limitation: they only support one notifier-to-one receiver (no broadcast to multiple tasks).

---

### Choosing the Right Primitive

| Scenario | Recommended primitive |
|---|---|
| ISR signals one task | Binary semaphore or task notification |
| Count ISR occurrences for a task | Counting semaphore or notification (counting mode) |
| Protect shared data between tasks | Mutex (with priority inheritance) |
| Protect shared data, same task re-enters | Recursive mutex |
| Pass data items between tasks | Queue |
| Wait for multiple independent events | Event group |
| Fast one-to-one task/ISR signalling | Task notification |
| Broadcast one event to many tasks | Event group |

---

## Interview Questions

### Fundamentals Tier

---

**Q1. What is the difference between a binary semaphore and a mutex in FreeRTOS?**

**Answer:**

Both have a binary state (available / not available) and both cause a task to block when unavailable. The differences are:

**Ownership**: A mutex tracks which task owns it. Only the task that took the mutex can give it. A binary semaphore has no owner — any task (or ISR) can give it.

**Priority inheritance**: FreeRTOS mutexes implement priority inheritance. If a high-priority task blocks on a mutex held by a low-priority task, the low-priority task's priority is temporarily elevated to the high-priority task's level. Binary semaphores have no such mechanism.

**ISR usage**: Binary semaphores can be given from ISRs using `xSemaphoreGiveFromISR`. Mutexes cannot be used from ISRs — priority inheritance is meaningless in an ISR context and the mechanism would break.

**Intended use**:
- Binary semaphore: signalling between tasks/ISRs ("event happened, task can proceed").
- Mutex: mutual exclusion ("only one task may access this resource at a time").

A common mistake is using a binary semaphore for mutual exclusion. It works until priority inversion occurs — the missing priority inheritance can cause high-priority tasks to miss deadlines.

---

**Q2. A queue is full and a task calls `xQueueSend` with a timeout of 10 ms. Describe exactly what happens.**

**Answer:**

1. The task calls `xQueueSend(xQueue, &data, pdMS_TO_TICKS(10))`.
2. The kernel checks the queue — it is full. The task cannot place its item immediately.
3. The task is moved from the Ready state to the Blocked state. Its TCB is placed on the queue's "waiting to send" list with a timestamp recording when the timeout should expire.
4. The scheduler runs, selecting the next highest-priority ready task.
5. **Case A — queue space becomes available before 10 ms**: another task calls `xQueueReceive`, removing an item. The kernel checks whether any task is waiting to send. If so, the sending task is moved to Ready (the item is transferred into the now-available slot). When the sending task next runs, `xQueueSend` returns `pdPASS`.
6. **Case B — 10 ms elapses without queue space**: the tick ISR checks the delayed task list at each tick. When the timeout expires, the sending task is moved to Ready. When it runs, `xQueueSend` returns `errQUEUE_FULL`.

If multiple tasks are waiting to send, they are unblocked in priority order (highest priority first). Tasks of equal priority are unblocked in FIFO order.

---

**Q3. Why should you never call `xSemaphoreTake` or `xQueueReceive` from an ISR?**

**Answer:**

These functions can block — they put the calling context into the Blocked state if the resource is unavailable. ISRs are not tasks and cannot be placed in the Blocked state. The ISR runs on the processor's main stack (MSP on ARM), not on a task's process stack, and the RTOS scheduler has no TCB entry for it.

If an ISR called a blocking API, the RTOS would attempt to context-switch the ISR away, which is undefined. In practice, FreeRTOS performs a runtime check: calling a non-ISR API from within an interrupt triggers an assert or fault.

Use the `FromISR` variants from ISRs:
- `xQueueReceiveFromISR` — non-blocking receive (returns immediately if empty).
- `xSemaphoreGiveFromISR` — non-blocking give.
- `xQueueSendFromISR` — non-blocking send.

These variants never block. If they need to wake a waiting task, they set `*pxHigherPriorityTaskWoken = pdTRUE` and the ISR must call `portYIELD_FROM_ISR` at exit to trigger a context switch.

---

**Q4. What does `portYIELD_FROM_ISR(xHigherPriorityTaskWoken)` do and why is it needed?**

**Answer:**

When an ISR calls `xSemaphoreGiveFromISR` or `xQueueSendFromISR`, these functions may unblock a waiting task. If the unblocked task has a higher priority than the task that was running before the ISR, a context switch is needed immediately after the ISR returns — otherwise the high-priority task must wait until the next tick or next scheduling point.

`portYIELD_FROM_ISR(xHigherPriorityTaskWoken)` does:
- If `xHigherPriorityTaskWoken == pdTRUE`: sets the `PendSV` pending bit in the ARM SCB ICSR register. When the ISR returns and all higher-priority exceptions have completed, `PendSV` fires and performs the context switch.
- If `xHigherPriorityTaskWoken == pdFALSE`: does nothing. The task that was running before the ISR resumes.

Without this call, the context switch would not occur until the next tick interrupt, adding up to one full tick period of latency for the newly ready high-priority task — violating real-time guarantees.

```c
void GPIO_EXTI_IRQHandler(void)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xSemaphoreGiveFromISR(xButtonSem, &xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    /* ISR returns -> if PendSV set, context switch happens immediately */
}
```

---

### Intermediate Tier

---

**Q5. Explain why passing a pointer to a local variable through a queue is dangerous. How do you pass large data structures safely?**

**Answer:**

A queue copies the item by value at the time of `xQueueSend`. If you enqueue a pointer to a local (stack-allocated) variable, the pointer is copied correctly, but the pointed-to data lives on the sender's stack — it will be overwritten or freed when the sending function returns or the stack frame is recycled.

```c
void sensor_task(void *pvParameters)
{
    for (;;) {
        /* WRONG: local variable goes out of scope */
        sensor_data_t data = read_sensor();       /* on the stack */
        xQueueSend(xQueue, &data, portMAX_DELAY); /* copies the entire struct by value */
        /* data is destroyed when the loop iteration ends */
    }
}
```

**Note:** FreeRTOS queues copy by value. If the queue item type is `sensor_data_t` (the struct itself), the entire struct is deep-copied at enqueue time — the stack lifetime issue disappears, but large structs carry a proportionally high copy cost. If the queue item type is `sensor_data_t*` (a pointer), only the pointer address is copied, leaving the original data at risk of being overwritten when the sender's stack frame is recycled.

**Correct patterns for large data:**

1. **Static or global buffers with a semaphore**: keep a pool of `sensor_data_t` objects. The sender claims one, fills it, passes a pointer through the queue. The receiver frees it back to the pool.

2. **Double-buffering**: maintain two buffers; producer writes to one while consumer reads from the other. A flag or semaphore indicates which is current.

3. **Pass by pointer with clear ownership semantics**: if the data has a well-defined lifetime longer than the processing window (e.g., DMA buffer, global ring buffer), passing a pointer is safe. Document the ownership rule explicitly.

4. **Message buffer / stream buffer** (FreeRTOS v10+): designed for byte-stream data, more efficient than queues for variable-length messages.

---

**Q6. What is a "queue set" and when would you use one?**

**Answer:**

A queue set is a FreeRTOS construct that groups multiple queues and binary semaphores so that a single task can block waiting for data to arrive in any of them.

Without a queue set, a task that receives from two queues must either poll them in a loop (wasting CPU), use two tasks (memory overhead), or use a separate "ready" semaphore (extra synchronisation logic).

```c
/* Two input queues */
QueueHandle_t xUartQueue = xQueueCreate(10, sizeof(char));
QueueHandle_t xSpiQueue  = xQueueCreate(5, sizeof(spi_msg_t));

/* Create a queue set large enough for both queues' combined depths */
QueueSetHandle_t xQueueSet = xQueueCreateSet(10 + 5);
xQueueAddToSet(xUartQueue, xQueueSet);
xQueueAddToSet(xSpiQueue,  xQueueSet);

/* Task: blocks on the set, then reads from whichever queue has data */
void comms_task(void *pvParameters)
{
    for (;;) {
        QueueSetMemberHandle_t xActiveMember =
            xQueueSelectFromSet(xQueueSet, portMAX_DELAY);

        if (xActiveMember == (QueueSetMemberHandle_t)xUartQueue) {
            char c;
            xQueueReceive(xUartQueue, &c, 0);
            handle_uart_byte(c);
        } else if (xActiveMember == (QueueSetMemberHandle_t)xSpiQueue) {
            spi_msg_t msg;
            xQueueReceive(xSpiQueue, &msg, 0);
            handle_spi_message(&msg);
        }
    }
}
```

Limitation: once a queue is added to a set, items must only be removed via the set mechanism — direct `xQueueReceive` is not permitted while the queue is in a set.

---

**Q7. Describe the sequence of events when an ISR gives a semaphore and a higher-priority task is waiting. Trace from ISR entry to the high-priority task resuming.**

**Answer:**

Starting state: Task_LOW (priority 2) is running. Task_HIGH (priority 5) is blocked waiting on `xSem`. Semaphore count = 0.

1. Hardware interrupt fires. CPU switches to handler mode, automatically pushing Task_LOW's `{r0-r3, r12, lr, pc, xpsr}` to Task_LOW's process stack.
2. ISR body executes. Calls `xSemaphoreGiveFromISR(xSem, &xHigherPriorityTaskWoken)`.
3. Inside `GiveFromISR`: increments semaphore count to 1. Checks whether any task is blocked on the semaphore. Task_HIGH is waiting. Task_HIGH is moved to the Ready list. Since Task_HIGH priority (5) > currently running task priority (2), `*pxHigherPriorityTaskWoken = pdTRUE`.
4. ISR calls `portYIELD_FROM_ISR(pdTRUE)`. This writes to ARM's SCB ICSR register, setting the `PendSV` pending bit.
5. ISR returns. Hardware unstacks `{r0-r3, r12, lr, pc, xpsr}` from Task_LOW's stack (tail-chain check: `PendSV` is pending and is now the highest-priority pending exception).
6. `PendSV` fires immediately. PendSV handler (RTOS code): saves Task_LOW's `{r4-r11}` to Task_LOW's stack, saves Task_LOW's PSP to its TCB.
7. Scheduler selects Task_HIGH (priority 5, highest ready). Loads Task_HIGH's PSP from its TCB.
8. PendSV restores Task_HIGH's `{r4-r11}` from Task_HIGH's stack.
9. PendSV returns. Hardware unstacks Task_HIGH's `{r0-r3, r12, lr, pc, xpsr}` from Task_HIGH's stack.
10. Task_HIGH resumes execution from where it blocked inside `xSemaphoreTake`. The function returns `pdTRUE`.

Total latency from ISR give to Task_HIGH resuming: one PendSV context switch, approximately 100-300 cycles on Cortex-M4.

---

### Advanced Tier

---

**Q8. You have a shared ring buffer accessed by a producer task and a consumer task. The producer also runs from an ISR context. Design the correct synchronisation strategy.**

**Answer:**

The ring buffer has two access contexts: a task-level producer ISR (unusual — more commonly an ISR feeds a task, but let us assume both task and ISR write) and a task-level consumer. We need to protect the write pointer against concurrent access from both the task producer and the ISR producer, while allowing the consumer to read.

**Strategy:**

1. **ISR-to-consumer semaphore**: a counting semaphore tracks the number of items in the buffer. ISR and producer task each give the semaphore after writing; consumer task takes it before reading.

2. **Write-side critical section**: the ISR producer and task producer share the write pointer. To protect it:
   - Use `taskENTER_CRITICAL()` / `taskEXIT_CRITICAL()` in the task producer — this disables interrupts for the duration of the write-pointer update, preventing the ISR from preempting mid-update.
   - The ISR does not need critical section protection against itself (ISRs are not reentrant on single-core Cortex-M).

3. **Read-side**: the consumer only runs at task level, so a simple critical section around the read-pointer update suffices.

```c
/* Counting semaphore: counts items in ring buffer */
SemaphoreHandle_t xItemsAvailable;

/* ISR producer */
void UART_IRQHandler(void)
{
    BaseType_t xWoken = pdFALSE;

    /* Write to ring buffer (single writer from ISR — no critical section needed
       if no task-level writer, OR use atomic write to write index) */
    ring_buffer_write_from_isr(UART->DR);

    xSemaphoreGiveFromISR(xItemsAvailable, &xWoken);
    portYIELD_FROM_ISR(xWoken);
}

/* Task producer (e.g., fills buffer from a calculated source) */
void producer_task(void *pvParameters)
{
    for (;;) {
        uint8_t data = calculate_next_byte();

        /* Protect write pointer against ISR preemption */
        taskENTER_CRITICAL();
        ring_buffer_write(data);
        taskEXIT_CRITICAL();

        xSemaphoreGive(xItemsAvailable);
    }
}

/* Consumer task */
void consumer_task(void *pvParameters)
{
    for (;;) {
        xSemaphoreTake(xItemsAvailable, portMAX_DELAY);
        uint8_t data = ring_buffer_read();
        process(data);
    }
}
```

Note: if only ISR produces, an atomic write index and a counting semaphore suffice with no critical sections on the producer side.

---

**Q9. Explain the conditions for deadlock in an RTOS and how you prevent it.**

**Answer:**

Deadlock (the "deadly embrace") occurs when two or more tasks are each waiting for a resource held by the other, forming a circular dependency — none can proceed.

**Coffman conditions** (all four must hold simultaneously for deadlock):
1. **Mutual exclusion**: resources cannot be shared (held by one task at a time).
2. **Hold and wait**: a task holds a resource while waiting for another.
3. **No preemption**: resources cannot be forcibly taken from a task.
4. **Circular wait**: Task A waits for Task B's resource; Task B waits for Task A's resource.

**Example:**
```
Task A: takes MutexX, then tries to take MutexY (blocks — B holds Y)
Task B: takes MutexY, then tries to take MutexX (blocks — A holds X)
-> Deadlock
```

**Prevention strategies (break any one Coffman condition):**

1. **Lock ordering** (break circular wait): always acquire mutexes in a globally agreed fixed order. If all tasks always take MutexX before MutexY, circular wait is impossible.

   ```c
   /* ALL tasks: always take mutex_a before mutex_b */
   xSemaphoreTake(mutex_a, portMAX_DELAY);
   xSemaphoreTake(mutex_b, portMAX_DELAY);
   /* ... work ... */
   xSemaphoreGive(mutex_b);
   xSemaphoreGive(mutex_a);
   ```

2. **Bounded waiting with timeout and backoff** (detect and recover): use finite timeouts on all mutex takes. If a task cannot acquire all needed resources within a timeout, release all held resources, wait a random/backoff period, and retry.

3. **Priority ceiling protocol** (structural prevention): assign each mutex a priority ceiling equal to the highest priority of any task that uses it. A task that takes any mutex is elevated to the ceiling, preventing any other mutex-holding task from running. Prevents priority inversion and deadlock simultaneously.

4. **Resource allocation graphs**: in design, draw which tasks use which resources. If the graph has no cycles, deadlock is impossible.

5. **Eliminate multiple mutex holds**: redesign so tasks never hold more than one mutex simultaneously. This is the simplest and most reliable fix.
