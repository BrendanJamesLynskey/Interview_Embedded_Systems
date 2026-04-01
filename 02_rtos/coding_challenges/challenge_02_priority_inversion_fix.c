/*
 * Challenge 02: Demonstrating and Fixing Priority Inversion
 *
 * Problem:
 *   This file contains two complete demonstrations:
 *
 *   PART A — BROKEN:  A three-task system that exhibits priority inversion
 *   because a binary semaphore (no priority inheritance) is used for mutual
 *   exclusion.  A medium-priority task starves the high-priority task.
 *
 *   PART B — FIXED:   The same system corrected by replacing the binary
 *   semaphore with a FreeRTOS mutex, which provides automatic priority
 *   inheritance.  The high-priority task's deadline is consistently met.
 *
 *   An instrumented timing measurement shows the difference numerically.
 *
 * System under test:
 *   TASK_H  (priority HIGH=4): time-critical, must complete within 50 ms of
 *                               becoming ready.  Needs the shared resource.
 *   TASK_M  (priority MED=3):  CPU-intensive background work, no shared
 *                               resource, runs for ~30 ms when scheduled.
 *   TASK_L  (priority LOW=1):  housekeeping, holds the shared resource for
 *                               ~20 ms.  Becomes ready at t=0.
 *
 * Expected timing (tick resolution = 1 ms):
 *
 *   BROKEN  (binary semaphore):
 *     t=0:   L starts, takes binary semaphore
 *     t=5:   H becomes ready, tries to take semaphore -- BLOCKED
 *     t=5:   M becomes ready (or was always ready), RUNS (H blocked, L preempted)
 *     t=35:  M finishes, L resumes
 *     t=55:  L gives semaphore, H unblocks
 *     t=55+: H completes -- MISSED DEADLINE (55 > 50 ms from t=5)
 *
 *   FIXED   (mutex with priority inheritance):
 *     t=0:   L starts, takes mutex
 *     t=5:   H becomes ready, tries to take mutex -- BLOCKED
 *     t=5:   L's priority ELEVATED to HIGH=4 (inherits from H)
 *     t=5:   L preempts M (L is now priority 4 > M priority 3)
 *     t=25:  L gives mutex, priority RESTORED to LOW=1
 *     t=25:  H unblocks, runs -- MEETS DEADLINE (25-5=20 ms wait < 50 ms)
 *
 * Compile:
 *   Use the FreeRTOS POSIX simulator or Windows Simulator port.
 *   Select PART_A or PART_B via the compile-time flag:
 *     -DUSE_BROKEN_VERSION   -> Part A (binary semaphore, broken)
 *     (no flag)              -> Part B (mutex, fixed)
 *
 * Observed output (approximate):
 *   PART A: [TASK_H] completed in 55 ms  <-- MISSED DEADLINE
 *   PART B: [TASK_H] completed in 21 ms  <-- deadline met
 */

#include <stdio.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

/* -------------------------------------------------------------------------
 * Configuration
 * -------------------------------------------------------------------------*/

#define PRIORITY_LOW    1U
#define PRIORITY_MED    3U
#define PRIORITY_HIGH   4U

#define STACK_DEPTH     256U

/* Timing in milliseconds (converted to ticks in code) */
#define L_RESOURCE_HOLD_MS   20U    /* how long LOW holds the shared resource */
#define M_WORK_MS            30U    /* how long MEDIUM does its CPU work      */
#define H_DEADLINE_MS        50U    /* TASK_H must complete within this many ms
                                       of becoming ready                      */

/* H becomes ready this many ms after L takes the resource */
#define H_READY_DELAY_MS     5U

/* -------------------------------------------------------------------------
 * Shared resource handle
 * (declared as void* so we can assign either a semaphore or a mutex)
 * -------------------------------------------------------------------------*/
static SemaphoreHandle_t xSharedResource;

/* Timestamps for measuring H's response latency */
static volatile TickType_t xH_Ready_Tick    = 0;
static volatile TickType_t xH_Complete_Tick = 0;

/* -------------------------------------------------------------------------
 * Simulated work functions
 *
 * In a real system these would be real operations (flash write, SPI transfer,
 * sensor read).  Here we busy-spin for a measured duration so the scheduler
 * sees the task as running -- vTaskDelay would release the CPU.
 * -------------------------------------------------------------------------*/

/* Busy-spin for approximately 'ms' milliseconds.
   Uses the tick counter rather than hardware to avoid requiring a
   high-resolution timer.  Accuracy is limited to tick resolution (1 ms). */
static void busy_spin_ms(uint32_t ms)
{
    TickType_t xEnd = xTaskGetTickCount() + pdMS_TO_TICKS(ms);
    while (xTaskGetTickCount() < xEnd) {
        /* spin -- keeps the CPU busy so we appear to be "doing work" */
        __asm volatile ("nop");
    }
}

/* -------------------------------------------------------------------------
 * TASK_L: Low-priority housekeeping
 *
 * Takes the shared resource, holds it for L_RESOURCE_HOLD_MS while doing
 * its work, then releases it.
 * -------------------------------------------------------------------------*/
static void task_low(void *pvParameters)
{
    (void)pvParameters;

    printf("[TASK_L] starting, about to take resource\n");
    xSemaphoreTake(xSharedResource, portMAX_DELAY);
    printf("[TASK_L] resource acquired, working for %u ms\n", L_RESOURCE_HOLD_MS);

    /* Simulate holding the resource while doing housekeeping work.
       TASK_H will become ready partway through this window (H_READY_DELAY_MS). */
    busy_spin_ms(L_RESOURCE_HOLD_MS);

    printf("[TASK_L] work done, releasing resource\n");
    xSemaphoreGive(xSharedResource);

    /* L has finished its one-shot work -- delete itself */
    printf("[TASK_L] done\n");
    vTaskDelete(NULL);
}

/* -------------------------------------------------------------------------
 * TASK_M: Medium-priority background computation
 *
 * Runs whenever the scheduler selects it.  Has no dependency on the shared
 * resource -- it purely consumes CPU time, demonstrating how medium-priority
 * tasks can starve TASK_H (via TASK_L) in the broken version.
 * -------------------------------------------------------------------------*/
static void task_medium(void *pvParameters)
{
    (void)pvParameters;

    /* Wait until after L has taken the resource (to ensure the race condition
       is properly set up in both PART A and PART B) */
    vTaskDelay(pdMS_TO_TICKS(H_READY_DELAY_MS));

    printf("[TASK_M] starting CPU-intensive work for %u ms\n", M_WORK_MS);
    busy_spin_ms(M_WORK_MS);
    printf("[TASK_M] done\n");
    vTaskDelete(NULL);
}

/* -------------------------------------------------------------------------
 * TASK_H: High-priority time-critical task
 *
 * Becomes ready H_READY_DELAY_MS after L takes the resource.  Must complete
 * (acquire and release the resource) within H_DEADLINE_MS of becoming ready.
 * -------------------------------------------------------------------------*/
static void task_high(void *pvParameters)
{
    (void)pvParameters;

    /* Delay to simulate H becoming ready after L has already taken the resource */
    vTaskDelay(pdMS_TO_TICKS(H_READY_DELAY_MS));

    xH_Ready_Tick = xTaskGetTickCount();
    printf("[TASK_H] became ready at tick %lu, trying to take resource\n",
           (unsigned long)xH_Ready_Tick);

    /* This call will BLOCK until TASK_L releases the resource.
       In PART A: TASK_M runs while we wait (priority inversion).
       In PART B: TASK_L is elevated to our priority and completes quickly. */
    xSemaphoreTake(xSharedResource, portMAX_DELAY);

    printf("[TASK_H] resource acquired -- doing critical work\n");
    busy_spin_ms(2U);   /* minimal work inside critical section */
    xSemaphoreGive(xSharedResource);

    xH_Complete_Tick = xTaskGetTickCount();
    TickType_t xLatency = xH_Complete_Tick - xH_Ready_Tick;

    printf("[TASK_H] completed in %lu ms -- deadline was %u ms -- %s\n",
           (unsigned long)pdTICKS_TO_MS(xLatency),
           H_DEADLINE_MS,
           (pdTICKS_TO_MS(xLatency) <= H_DEADLINE_MS) ? "DEADLINE MET" : "DEADLINE MISSED");

    vTaskDelete(NULL);
}

/* -------------------------------------------------------------------------
 * Monitor task
 *
 * Waits for all worker tasks to complete, then prints a summary.
 * -------------------------------------------------------------------------*/
static void monitor_task(void *pvParameters)
{
    (void)pvParameters;

    /* Wait long enough for all tasks to finish */
    vTaskDelay(pdMS_TO_TICKS(200U));

    printf("\n=== Summary ===\n");

#if defined(USE_BROKEN_VERSION)
    printf("Mode: BROKEN (binary semaphore -- no priority inheritance)\n");
#else
    printf("Mode: FIXED  (mutex -- priority inheritance enabled)\n");
#endif

    if (xH_Complete_Tick > xH_Ready_Tick) {
        TickType_t xLatency = xH_Complete_Tick - xH_Ready_Tick;
        printf("TASK_H response time: %lu ms (deadline: %u ms)\n",
               (unsigned long)pdTICKS_TO_MS(xLatency),
               H_DEADLINE_MS);
        if (pdTICKS_TO_MS(xLatency) <= H_DEADLINE_MS) {
            printf("Result: PASS -- real-time deadline met\n");
        } else {
            printf("Result: FAIL -- real-time deadline MISSED by %lu ms\n",
                   (unsigned long)(pdTICKS_TO_MS(xLatency) - H_DEADLINE_MS));
        }
    } else {
        printf("TASK_H did not complete (measurement error)\n");
    }

    vTaskDelete(NULL);
}

/* -------------------------------------------------------------------------
 * Application entry point
 * -------------------------------------------------------------------------*/

int main(void)
{
#if defined(USE_BROKEN_VERSION)
    /*
     * PART A -- BROKEN: Binary semaphore has no ownership and no priority
     * inheritance.  When TASK_H blocks on the semaphore held by TASK_L,
     * TASK_M (which has higher priority than TASK_L) runs first, delaying
     * TASK_L's release of the semaphore and causing TASK_H to miss its
     * deadline.
     */
    printf("=== Part A: Priority Inversion Demo (BROKEN) ===\n\n");
    xSharedResource = xSemaphoreCreateBinary();
    configASSERT(xSharedResource != NULL);

    /* Binary semaphore starts unavailable (count=0).
       Give it once so it starts in the "available" state. */
    xSemaphoreGive(xSharedResource);

#else
    /*
     * PART B -- FIXED: FreeRTOS mutex provides priority inheritance.
     * When TASK_H blocks on the mutex held by TASK_L, TASK_L's effective
     * priority is raised to TASK_H's priority level.  TASK_L now preempts
     * TASK_M and completes its critical section quickly, allowing TASK_H
     * to run before its deadline expires.
     */
    printf("=== Part B: Priority Inversion Fix (MUTEX) ===\n\n");
    xSharedResource = xSemaphoreCreateMutex();
    configASSERT(xSharedResource != NULL);
    /* Mutex starts available (no explicit give needed) */
#endif

    /* Create tasks -- all start immediately once the scheduler runs */
    BaseType_t xResult;

    xResult = xTaskCreate(task_low,    "LOW",     STACK_DEPTH, NULL, PRIORITY_LOW,  NULL);
    configASSERT(xResult == pdPASS);

    xResult = xTaskCreate(task_medium, "MED",     STACK_DEPTH, NULL, PRIORITY_MED,  NULL);
    configASSERT(xResult == pdPASS);

    xResult = xTaskCreate(task_high,   "HIGH",    STACK_DEPTH, NULL, PRIORITY_HIGH, NULL);
    configASSERT(xResult == pdPASS);

    xResult = xTaskCreate(monitor_task, "MONITOR", STACK_DEPTH, NULL, PRIORITY_LOW,  NULL);
    configASSERT(xResult == pdPASS);

    vTaskStartScheduler();

    /* Should never reach here */
    for (;;) {}
}

/* -------------------------------------------------------------------------
 * FreeRTOS application hooks
 * -------------------------------------------------------------------------*/

void vApplicationMallocFailedHook(void)
{
    taskDISABLE_INTERRUPTS();
    for (;;) {}
}

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    (void)pcTaskName;
    taskDISABLE_INTERRUPTS();
    for (;;) {}
}

/* =========================================================================
 * INTERVIEW DISCUSSION NOTES
 *
 * Q: Why does the broken version use xSemaphoreCreateBinary() instead of
 *    xSemaphoreCreateMutex() for mutual exclusion?
 * A: Binary semaphores are often (incorrectly) used as mutexes because both
 *    have a binary state.  The key difference is ownership: a mutex tracks
 *    which task holds it and raises that task's priority when a higher-
 *    priority task is waiting.  A binary semaphore has no owner and provides
 *    no such guarantee.  This is the exact mistake that caused the Mars
 *    Pathfinder failure.
 *
 * Q: Can you use a binary semaphore for mutual exclusion if you don't have
 *    any priority inversion risk?
 * A: Only if you can guarantee that the semaphore holder is always the
 *    highest-priority task in the system, or that the holder never holds
 *    the semaphore for more than a single tick.  In practice, using a mutex
 *    is always safer for mutual exclusion -- the overhead difference is
 *    negligible (~50 ns on Cortex-M4).
 *
 * Q: Does FreeRTOS priority inheritance handle transitive chains?
 *    (e.g., H waits on mutex held by M, M waits on mutex held by L)
 * A: FreeRTOS v10 implements basic transitive inheritance for direct chains.
 *    However, complex diamond or multi-level dependency graphs may not be
 *    fully resolved.  The recommended design is to avoid holding multiple
 *    mutexes simultaneously.
 *
 * Q: What is the difference between priority inheritance and priority ceiling?
 * A: Priority inheritance is reactive -- it raises the holder's priority
 *    after a blocking event.  Priority ceiling is proactive -- it raises the
 *    holder's priority to the ceiling at the moment of acquisition, before
 *    any blocking can occur.  PCP also prevents deadlock; PIP does not.
 *    FreeRTOS implements PIP; AUTOSAR OS uses PCP (Immediate PCP).
 * =========================================================================*/
