# Priority Inversion

## Overview

Priority inversion is one of the most notorious failure modes in real-time systems. It occurs when a high-priority task is indirectly blocked by a low-priority task through a shared mutex, causing the high-priority task to be effectively starved while unrelated medium-priority tasks run. Priority inversion invalidates the assumptions of rate-monotonic scheduling and can cause high-priority tasks to miss their deadlines — sometimes indefinitely.

The problem became famous when it nearly caused the loss of the Mars Pathfinder mission in 1997. Understanding priority inversion, its variants, and the protocols designed to prevent it is essential for any RTOS-based embedded systems interview.

---

## Key Concepts

### The Priority Inversion Problem

Consider three tasks:
- **Task H** (high priority, e.g., priority 5): time-critical control task.
- **Task M** (medium priority, e.g., priority 3): background computation, no shared resources with H.
- **Task L** (low priority, e.g., priority 1): housekeeping, shares a mutex with H.

**Sequence leading to unbounded priority inversion:**

```
Time 0:  Task L runs, takes Mutex_A
Time 1:  Task H becomes ready (interrupt or timer), preempts L
Time 2:  Task H calls xSemaphoreTake(Mutex_A) — L holds it, H blocks
Time 3:  Since H is blocked, scheduler runs next highest-priority ready task: Task M
Time 4:  Task M runs to completion (or until it blocks)
Time 5:  Task L can now run (M has finished)
Time 6:  Task L finishes critical section, gives Mutex_A
Time 7:  Task H unblocks and resumes
```

At step 3-4, the high-priority Task H is waiting, but Task M — which shares no resources with H — runs ahead of H. This is the inversion: the effective priority ordering is M > H, the opposite of the declared order.

Worse: if there are many medium-priority tasks that continuously become ready, Task H may wait indefinitely. This is **unbounded priority inversion**.

```
Timeline without priority inheritance:
-----|-------|---------|---------|---------|------->
 L takes  H blocks  M1 runs  M2 runs  L gives  H runs
 mutex    (waits)            (waits for nothing)
    <- H is waiting during all of M1, M2 execution ->
```

---

### The Mars Pathfinder Case Study

In July 1997, the Mars Pathfinder rover began experiencing total system resets shortly after landing — the computer was rebooting itself several times per day. JPL engineers diagnosed the problem remotely by analysing the VxWorks RTOS task traces.

**The configuration:**
- A high-priority meteorological science task (Task H) collected sensor data and stored it via a shared information bus mutex.
- A low-priority communications task (Task L) also accessed the bus mutex for long periods.
- A medium-priority task (Task M) ran between H and L.

**What happened:**
1. Task L took the bus mutex for a long data transfer.
2. Task H preempted L, tried to take the bus mutex, blocked.
3. Task M preempted L (M had higher priority than L).
4. While M ran, L could not complete its critical section and release the mutex.
5. Task H remained blocked. The watchdog timer — which expected H to check in periodically — timed out.
6. The watchdog reset the entire system.

**The fix (applied via uplink from Earth):**
JPL engineers enabled the priority inheritance option for the VxWorks mutex being used. VxWorks had priority inheritance built in but it was disabled by default. A single configuration parameter change, uploaded to the spacecraft, fixed the problem.

The episode demonstrated that priority inversion is not a theoretical concern — it can and does occur in production systems with serious consequences. It also demonstrated the importance of understanding RTOS configuration options before deployment.

---

### Priority Inheritance Protocol (PIP)

Priority inheritance is the most common mitigation. The rule is simple:

> **When a high-priority task blocks on a mutex, the priority of the mutex holder is temporarily elevated to match the blocking task's priority.**

This ensures the low-priority holder can preempt medium-priority tasks to finish its critical section quickly, freeing the mutex.

```
Time 0:  Task L runs (priority 1), takes Mutex_A
Time 1:  Task H (priority 5) blocks on Mutex_A
         -> Kernel elevates L's priority to 5 (inherits H's priority)
Time 2:  Task L now runs at priority 5 — it PREEMPTS Task M (priority 3)
Time 3:  Task L completes critical section, gives Mutex_A
         -> L's priority restored to 1
Time 4:  Task H unblocks, runs (highest ready priority = 5)
```

Task M is now correctly delayed behind H. The inversion is bounded to the duration of L's critical section.

**FreeRTOS implementation:**
FreeRTOS implements priority inheritance automatically for mutexes created with `xSemaphoreCreateMutex()`. There is no additional configuration required. It does not implement priority inheritance for binary semaphores — one reason why semaphores and mutexes should not be used interchangeably.

```c
/* FreeRTOS mutex with automatic priority inheritance */
SemaphoreHandle_t xBusMutex = xSemaphoreCreateMutex();

void low_priority_task(void *pvParameters)
{
    xSemaphoreTake(xBusMutex, portMAX_DELAY);
    /* If H blocks here, L's priority is raised automatically */
    write_to_bus(large_data_block, sizeof(large_data_block));
    xSemaphoreGive(xBusMutex);
    /* Priority is restored to original level */
}
```

**Limitations of PIP:**
1. Priority inheritance is applied transitively — if L holds mutex Y, and M (elevated by H) blocks on Y, then L is elevated to H's priority. This chains through the dependency graph. FreeRTOS implements one level of transitivity.
2. Chained inheritance with many mutexes and tasks can be complex to analyse.
3. PIP does not prevent deadlock.
4. PIP does not give a tight upper bound on blocking — a task can still be blocked for the duration of the critical section.

---

### Priority Ceiling Protocol (PCP)

The Priority Ceiling Protocol provides stronger guarantees than PIP by preventing deadlock as well as priority inversion.

**Rules:**
1. Every mutex is assigned a **priority ceiling** equal to the highest priority of any task that will ever take it.
2. A task may only take a mutex if its current priority is strictly higher than the ceiling of all currently locked mutexes (by all other tasks).
3. While a task holds a mutex, it runs at the maximum of its own priority and the mutex's ceiling.

**Properties:**
- A task can be blocked for at most the duration of one critical section (the longest critical section of all lower-priority tasks that use shared mutexes).
- Deadlock is impossible — the locking condition (rule 2) prevents circular waits.

**Example:**

Mutex_A ceiling = 5 (H uses it, H has priority 5).

```
Task L (priority 1) tries to take Mutex_A:
  - Is L's priority (1) > max ceiling of all held mutexes? (no other mutexes held, so max = 0)
  - 1 > 0: Yes. L takes Mutex_A and is elevated to priority 5.
  - L now runs at priority 5 — M (priority 3) cannot preempt L.
```

Task H arrives and tries to take Mutex_A — L holds it and is already at priority 5. H blocks. L completes quickly and gives the mutex. H resumes. No unbounded inversion.

**FreeRTOS**: FreeRTOS does not natively implement PCP. Some safety-critical frameworks (AUTOSAR OS, OSEK) implement PCP. In FreeRTOS, use a mutex and minimise critical section duration as the primary mitigation.

---

### Immediate Priority Ceiling Protocol (IPCP)

A simpler variant of PCP: when a task takes any mutex, it is immediately elevated to the ceiling of that mutex — before blocking can occur. This avoids the complex check of rule 2 and is easier to implement.

Used in AUTOSAR OS as the standard mechanism.

---

### Designing to Minimise Priority Inversion Risk

1. **Keep critical sections short**: the maximum blocking time for a high-priority task equals the longest critical section holding the contended mutex. Profile and minimise it.

2. **Do not call blocking APIs from within a critical section**: a task that takes a mutex and then blocks on a queue (waiting for space) holds the mutex for an unbounded time.

3. **Avoid nested locks**: holding multiple mutexes simultaneously is the primary cause of deadlock and complex inversion chains.

4. **Use DMA and double-buffering**: move long data transfers out of critical sections. Write to a staging buffer (no mutex needed), then swap buffers in a short critical section.

5. **Separate configuration from run-time access**: read-only shared data needs no mutex. Structure data so that frequently written fields are not co-located with frequently read fields.

6. **Test with stress scenarios**: run all medium-priority tasks at full load while the high-priority task and low-priority task share a mutex. Measure H's response time against its deadline.

---

## Interview Questions

### Fundamentals Tier

---

**Q1. Explain priority inversion using a concrete three-task example.**

**Answer:**

Three tasks: H (priority 5), M (priority 3), L (priority 1). L and H share a mutex. M shares nothing.

```
T=0: L runs, takes Mutex
T=1: H becomes ready, preempts L
T=2: H tries to take Mutex — L holds it. H blocks.
T=3: No other high-priority tasks ready. Scheduler picks M (priority 3).
T=4: M runs until it blocks or completes.
T=5: L can run again. L releases Mutex.
T=6: H unblocks and runs.
```

During T=3 to T=4, H is waiting even though H has higher priority than M. H's effective priority has been inverted: M runs before H. If there are many medium-priority tasks, H could wait for arbitrarily long.

The inversion occurs because the scheduler does not know that M's execution is indirectly preventing H from completing — the scheduler only sees that H is blocked, so it runs the next highest ready task, which is M.

---

**Q2. What is priority inheritance and how does it solve priority inversion?**

**Answer:**

Priority inheritance solves priority inversion by ensuring the mutex holder runs at the priority of the highest-priority task blocked on its mutex, rather than at its own (possibly low) priority.

When H (priority 5) blocks on a mutex held by L (priority 1), the kernel immediately elevates L's effective priority to 5. L now preempts M (priority 3) and can complete its critical section promptly. When L gives the mutex, L's priority reverts to 1, and H runs immediately as the highest-priority ready task.

The inversion is **bounded**: H waits at most for the duration of L's critical section. Without inheritance, H could wait for M, M's successors, M's successors' successors... — unbounded.

FreeRTOS implements priority inheritance automatically on any mutex created with `xSemaphoreCreateMutex`. It is not available for binary semaphores, which is one reason you should use mutexes (not binary semaphores) for mutual exclusion.

---

**Q3. Describe the Mars Pathfinder priority inversion incident. What was the bug and how was it fixed?**

**Answer:**

In August 1997, the Mars Pathfinder spacecraft began resetting itself multiple times per day shortly after landing. The cause was a classic priority inversion on the VxWorks real-time operating system.

**Configuration**: A high-priority meteorological data task (H) and a low-priority ASI/MET communications task (L) shared an information bus mutex. A medium-priority task (M) ran between them.

**Sequence**: L took the bus mutex for a long data transfer. H became ready and blocked waiting for the mutex. With H blocked, the scheduler ran M (the next highest ready task). M ran for an extended period. L could not complete its transfer (and free the mutex) because it was repeatedly preempted by M. H remained blocked. The watchdog timer — which expected H to periodically check in — expired. The watchdog reset the entire system.

**Fix**: The VxWorks mutex had a priority inheritance option that was disabled by default. JPL engineers uploaded a configuration change to the spacecraft enabling priority inheritance on the specific mutex. With inheritance enabled, when H blocked on the mutex, L's priority was elevated to H's priority, allowing L to complete its transfer and free the mutex before M could preempt it.

The fix was a single boolean flag in a configuration structure, sent via radio uplink across 170 million miles. The spacecraft operated normally thereafter for the remainder of the mission.

---

### Intermediate Tier

---

**Q4. What is the difference between Priority Inheritance Protocol and Priority Ceiling Protocol? When would you prefer PCP?**

**Answer:**

**Priority Inheritance Protocol (PIP):**
- When a task takes a mutex, its priority is unchanged.
- When a higher-priority task blocks on the mutex, the holder's priority is elevated to match.
- Elevation is reactive: it happens after a blocking event.
- Does not prevent deadlock.
- Blocking time can be up to n critical sections long for n nested mutexes.

**Priority Ceiling Protocol (PCP):**
- Each mutex has a preassigned ceiling = the highest priority of any task that will use it.
- A task can only take a mutex if its priority exceeds all ceilings of all currently locked mutexes.
- The holder runs at the maximum of its priority and the mutex ceiling.
- Prevents deadlock (the locking condition creates a total order).
- Blocking is bounded by the duration of exactly one critical section.

**When to prefer PCP:**
1. **Safety-critical systems** (AUTOSAR, OSEK, IEC 61508): PCP gives a provably tighter blocking bound, simplifying worst-case response time analysis.
2. **Systems with complex mutex dependency graphs**: PCP's deadlock prevention eliminates a class of errors.
3. **Certification**: PCP's static analysis is simpler to document and review.
4. **Systems where deadlock is not acceptable**: PCP is structurally deadlock-free; PIP is not.

**When PIP is sufficient:**
- Single-mutex critical sections (no nesting, no deadlock risk).
- FreeRTOS-based systems where PCP is not built in.
- Systems where critical section duration is already short and well-understood.

---

**Q5. Can priority inheritance itself cause problems? Describe two scenarios where PIP behaves poorly.**

**Answer:**

**Scenario 1: Chained priority inheritance (transitive inversion)**

Task H (priority 5) waits on Mutex_A held by Task M (priority 3).
Task M (now elevated to 5) waits on Mutex_B held by Task L (priority 1).
L must be elevated to priority 5 transitively.

FreeRTOS supports one level of transitivity but not arbitrary chains. In complex systems with long chains of mutex dependencies, the inheritance graph may not be correctly resolved, leaving inversion in the chain. The fix: avoid nested mutex locking.

**Scenario 2: Priority inheritance masks a design flaw**

If L holds a mutex for a very long time (copying a 10 KB buffer), H will wait for the entire duration even with priority inheritance. PIP makes the wait bounded but does not make it short. Engineers who add PIP and assume the problem is solved may miss that L's critical section is far too long.

Worse: with L elevated to H's priority, other tasks at intermediate priorities that need to run may be starved during L's elevated execution — effectively the system now treats L as a high-priority task, potentially missing other deadlines.

**Scenario 3: Incorrect use of binary semaphores instead of mutexes**

If a developer uses a binary semaphore (not a mutex) for mutual exclusion, priority inheritance does not apply. Priority inversion occurs exactly as in the uninherited case. FreeRTOS will not warn about this misuse.

---

**Q6. A task holds a mutex and then calls `vTaskDelay(1000)`. What problems does this cause and how would you fix it?**

**Answer:**

**Problems:**

1. **Unbounded blocking for other tasks**: any task that tries to take the same mutex will block for up to 1000 ticks (1 second at 1 kHz). This completely defeats the purpose of the mutex and makes the system's timing analysis invalid.

2. **Priority inversion**: if a high-priority task is waiting for the mutex, it is blocked for 1000 ms regardless of priority inheritance, because the delay is the bottleneck, not the CPU competition.

3. **Resource starvation**: all waiters are serialised behind a 1-second delay.

4. **Deadlock risk**: if the sleeping task was supposed to release the mutex and then re-acquire it in a sequence, other logic depending on the resource is completely stalled.

**Fix:**

The mutex should only be held during the critical section that accesses the shared resource. Delays, I/O, and all non-critical work should happen outside the mutex:

```c
/* Wrong: hold mutex across a delay */
xSemaphoreTake(xMutex, portMAX_DELAY);
prepare_data();
vTaskDelay(pdMS_TO_TICKS(1000));   /* mutex held for 1 second! */
write_to_device(data);
xSemaphoreGive(xMutex);

/* Correct: only hold mutex during the actual shared-resource access */
prepare_data();
vTaskDelay(pdMS_TO_TICKS(1000));   /* delay outside the mutex */
xSemaphoreTake(xMutex, portMAX_DELAY);
write_to_device(data);             /* short critical section */
xSemaphoreGive(xMutex);
```

General rule: the duration of any critical section should be the minimum possible — ideally a single register read/write or data structure update. Long operations (DMA, UART transmit, calculations) belong outside the mutex.

---

### Advanced Tier

---

**Q7. Prove that the Priority Ceiling Protocol prevents deadlock.**

**Answer:**

**Claim**: under PCP, deadlock is impossible.

**Proof by contradiction:**

Assume deadlock occurs. This requires a circular wait: Task A waits for a resource held by Task B, which waits for a resource held by ... Task N, which waits for a resource held by Task A.

For Task A to be waiting for a mutex held by Task B, Task B must currently hold that mutex. For Task B to hold that mutex under PCP's rule, at the moment B took it, B's priority must have exceeded the ceiling of all mutexes held by any other task (other than B's own mutexes).

Specifically: when B took Mutex_BA (the mutex A needs), B's priority must exceed the ceiling of Mutex_AB (the mutex B will subsequently need and which A holds). But the ceiling of Mutex_AB equals the highest priority of any task that uses Mutex_AB — which includes A. So the ceiling of Mutex_AB >= priority(A).

For B to have taken Mutex_BA, B's priority > ceiling(Mutex_AB) >= priority(A). So priority(B) > priority(A).

By the same argument applied to every pair in the circular chain, each task in the chain has strictly higher priority than the previous one. But the chain is circular, so we require priority(A) > priority(B) > ... > priority(A), which is a contradiction.

Therefore, circular wait cannot occur, and deadlock is impossible under PCP.

---

**Q8. Design a complete priority inversion-safe shared bus driver for FreeRTOS. The bus is accessed by three tasks at different priorities. Include all synchronisation considerations.**

**Answer:**

The design uses a mutex for mutual exclusion (priority inheritance provided automatically), a double-stage API (prepare/commit) to minimise critical section duration, and a timeout-based API to prevent unbounded blocking.

```c
/*
 * Shared bus driver — priority-inversion safe design.
 *
 * Three tasks use the bus:
 *   SAFETY_TASK   (priority 5) — brief, deadline-critical reads
 *   CONTROL_TASK  (priority 3) — periodic sensor reads
 *   LOGGING_TASK  (priority 1) — bulk data transfers
 *
 * Design choices:
 *   - Mutex (not binary semaphore): priority inheritance prevents inversion.
 *   - All callers use finite timeouts: prevents indefinite blocking.
 *   - API accepts pre-prepared buffer: no allocation inside critical section.
 *   - Critical section covers only the bus transaction: minimise hold time.
 */

#include "FreeRTOS.h"
#include "semphr.h"

#define BUS_MUTEX_TIMEOUT_MS    20U   /* max acceptable wait for any task */

typedef struct {
    SemaphoreHandle_t xMutex;
    uint32_t          uTransactionCount;
    uint32_t          uTimeoutCount;
} BusDriver_t;

static BusDriver_t gBus;

/* Initialise — call once before scheduler starts */
BaseType_t bus_driver_init(void)
{
    gBus.xMutex = xSemaphoreCreateMutex();
    if (gBus.xMutex == NULL) return pdFALSE;
    gBus.uTransactionCount = 0U;
    gBus.uTimeoutCount     = 0U;
    return pdTRUE;
}

/*
 * bus_transact — perform a bus transaction.
 *
 * The caller prepares tx_buf and provides rx_buf before calling.
 * The mutex is only held during the actual hardware operation.
 * Returns pdTRUE on success, pdFALSE on timeout or hardware error.
 *
 * This function must NOT be called from an ISR.
 */
BaseType_t bus_transact(const uint8_t *tx_buf, uint8_t *rx_buf, size_t len)
{
    if (tx_buf == NULL || rx_buf == NULL || len == 0U) return pdFALSE;

    /* Try to acquire the bus mutex with a bounded timeout.
       Priority inheritance will elevate this task's priority if a
       higher-priority task is waiting.                                  */
    if (xSemaphoreTake(gBus.xMutex, pdMS_TO_TICKS(BUS_MUTEX_TIMEOUT_MS))
            != pdTRUE) {
        gBus.uTimeoutCount++;
        /* Log timeout here for diagnostics */
        return pdFALSE;
    }

    /* --- Critical section: mutex is held --- */
    BaseType_t xResult = pdTRUE;

    /* Perform the hardware transaction.
       Keep this as short as possible — no delays, no blocking calls. */
    if (hal_bus_write_read(tx_buf, rx_buf, len) != HAL_OK) {
        xResult = pdFALSE;
    }

    gBus.uTransactionCount++;
    /* --- End of critical section --- */

    xSemaphoreGive(gBus.xMutex);
    return xResult;
}

/* Diagnostic: retrieve statistics without a lock (volatile read — safe for counters) */
void bus_get_stats(uint32_t *transactions, uint32_t *timeouts)
{
    *transactions = gBus.uTransactionCount;
    *timeouts     = gBus.uTimeoutCount;
}
```

**Key design decisions explained:**

- Using `xSemaphoreCreateMutex`: FreeRTOS priority inheritance is automatic. If the logging task (priority 1) holds the mutex while the safety task (priority 5) tries to take it, the logging task is promoted to priority 5, completing the transaction before the control task (priority 3) can preempt it.
- Bounded timeout (`BUS_MUTEX_TIMEOUT_MS`): every caller knows the maximum wait time. Safety-critical callers can treat a `pdFALSE` return as a fault condition.
- No allocation inside the critical section: `tx_buf` and `rx_buf` are prepared by the caller before the mutex is taken. Heap allocation inside a critical section is forbidden.
- No blocking calls inside the critical section: `hal_bus_write_read` must be a synchronous, non-blocking hardware operation (polling or DMA with a spin-wait of bounded duration). If DMA with IRQ completion is needed, restructure using a semaphore for the DMA done signal — given from the IRQ while the bus mutex is still held.
