/*
 * Challenge 03: Software Watchdog with Health Monitoring
 *
 * Problem:
 *   Implement a software watchdog system that monitors a set of application
 *   tasks and detects if any task has stopped executing (hung, deadlocked, or
 *   starved).  If a monitored task fails to "check in" within its timeout
 *   window, the watchdog takes a corrective action.
 *
 *   The system must also feed a hardware watchdog (simulated here) to reset
 *   the MCU if the software watchdog itself hangs.
 *
 * Architecture:
 *
 *   watchdog_task   (PRIORITY = configMAX_PRIORITIES - 1)
 *     |
 *     |-- inspects check-in table every WATCHDOG_PERIOD_MS
 *     |-- feeds hardware WDG only if ALL tasks are healthy
 *     |-- calls on_task_failure() if any task times out
 *
 *   monitored tasks (lower priorities):
 *     |-- call watchdog_checkin(TASK_ID) periodically
 *     |-- simulate_task_hang() triggers a hang after N seconds for testing
 *
 * Design considerations covered:
 *   1. Watchdog task at highest priority -- it must run even under CPU load.
 *   2. Per-task timeout: slow tasks get a longer timeout than fast tasks.
 *   3. Hardware watchdog fallback: software watchdog monitors tasks;
 *      hardware watchdog monitors the software watchdog itself.
 *   4. Thread-safe check-in: single 32-bit write on Cortex-M is atomic;
 *      no mutex needed for the timestamp update.
 *   5. Graceful recovery: log the failure, attempt task restart, escalate
 *      to system reset if restart fails.
 *
 * Compile:
 *   FreeRTOS POSIX or Windows Simulator port.
 *   gcc -std=c11 -Wall -Wextra -DSIMULATE_HANG_TASK_ID=0 \
 *       challenge_03_watchdog_task.c <FreeRTOS sources> -lpthread -o watchdog
 *
 *   SIMULATE_HANG_TASK_ID: if defined, causes that task ID to stop checking
 *   in after HANG_TRIGGER_MS milliseconds.  Used to demonstrate detection.
 *
 * Expected output (with SIMULATE_HANG_TASK_ID=0):
 *   [WDG] monitoring 3 tasks, period=100ms
 *   [TASK0] checkin (period=50ms)
 *   [TASK1] checkin (period=200ms)
 *   [TASK2] checkin (period=100ms)
 *   ... (normal operation for HANG_TRIGGER_MS ms) ...
 *   [TASK0] HANG SIMULATED -- stopping checkins
 *   [WDG] TIMEOUT: task TASK0 (id=0) -- last checkin 350ms ago (limit=200ms)
 *   [WDG] attempting recovery for task TASK0
 *   [WDG] SYSTEM RESET (hardware watchdog allowed to expire)
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

/* -------------------------------------------------------------------------
 * Configuration
 * -------------------------------------------------------------------------*/

#define NUM_MONITORED_TASKS     3U

/* Watchdog monitoring period in ms.  All task timeouts must be a multiple
   of this period so detection is timely.                                    */
#define WATCHDOG_PERIOD_MS      100U

/* After this many ms, the simulated hang task stops checking in */
#define HANG_TRIGGER_MS         1500U

/* Task priorities */
#define PRIORITY_WATCHDOG       ( configMAX_PRIORITIES - 1U )  /* highest */
#define PRIORITY_TASK0          2U
#define PRIORITY_TASK1          2U
#define PRIORITY_TASK2          2U

/* Stack depths in words */
#define WATCHDOG_STACK_WORDS    256U
#define TASK_STACK_WORDS        256U

/* -------------------------------------------------------------------------
 * Watchdog entry table
 *
 * Each monitored task has one entry.  The watchdog task reads this table
 * periodically.  Monitored tasks write xLastCheckin.
 *
 * Thread-safety note: xLastCheckin is written by one task and read by the
 * watchdog task.  On 32-bit Cortex-M, a 32-bit write is atomic (single bus
 * cycle).  The watchdog may occasionally read a value that is one update
 * behind, but this only affects the timeout detection by one period -- an
 * acceptable inaccuracy for a watchdog.  If strict atomicity is required,
 * use a mutex or C11 _Atomic TickType_t.
 * -------------------------------------------------------------------------*/

typedef struct {
    const char   *pcName;           /* task name for diagnostics             */
    TaskHandle_t  xHandle;          /* handle set at creation time           */
    TickType_t    xLastCheckin;     /* tick of last successful check-in      */
    TickType_t    xTimeoutTicks;    /* max allowed interval between checkins */
    uint32_t      uCheckinCount;    /* total checkins (for diagnostics)      */
    BaseType_t    xEnabled;         /* pdFALSE = skip this entry             */
} WatchdogEntry_t;

/* Table is written by monitored tasks (xLastCheckin, uCheckinCount) and
   read by the watchdog task.  The table itself is not protected by a mutex
   because the writes are to separate words (no write-write conflict) and
   are atomic on 32-bit targets.                                              */
static WatchdogEntry_t xWatchdogTable[NUM_MONITORED_TASKS] = {
    { "TASK0", NULL, 0, pdMS_TO_TICKS(200U), 0, pdTRUE },   /* 50ms period, 200ms timeout */
    { "TASK1", NULL, 0, pdMS_TO_TICKS(600U), 0, pdTRUE },   /* 200ms period, 600ms timeout */
    { "TASK2", NULL, 0, pdMS_TO_TICKS(300U), 0, pdTRUE },   /* 100ms period, 300ms timeout */
};

/* -------------------------------------------------------------------------
 * Watchdog check-in API
 *
 * Called by each monitored task to record that it is still alive.
 * -------------------------------------------------------------------------*/
void watchdog_checkin(uint8_t ucTaskId)
{
    configASSERT(ucTaskId < NUM_MONITORED_TASKS);

    /* Atomic on 32-bit Cortex-M -- safe without a mutex */
    xWatchdogTable[ucTaskId].xLastCheckin  = xTaskGetTickCount();
    xWatchdogTable[ucTaskId].uCheckinCount++;
}

/* -------------------------------------------------------------------------
 * Hardware watchdog (simulated)
 *
 * On a real MCU this would call the IWDG/WDT refresh register.
 * Here we just track whether it was fed.  The hardware watchdog has its
 * own timeout; if not fed, it resets the MCU independently of the software.
 * -------------------------------------------------------------------------*/
static volatile uint8_t ucHwWdgFed = 0U;

static void hardware_wdg_feed(void)
{
    /* On real hardware: HAL_IWDG_Refresh(&hiwdg);  */
    ucHwWdgFed = 1U;
}

static void hardware_wdg_start(void)
{
    /* On real hardware: start the IWDG with a timeout > WATCHDOG_PERIOD_MS */
    printf("[HW_WDG] hardware watchdog started\n");
}

/* -------------------------------------------------------------------------
 * Failure handler
 *
 * Called when a task times out.  In a real system this would:
 *   1. Log the event to non-volatile memory.
 *   2. Attempt to restart the failed task.
 *   3. If restart fails or the task has timed out repeatedly, reset.
 *
 * The return value indicates whether the failure was recovered (pdTRUE) or
 * requires a system reset (pdFALSE).
 * -------------------------------------------------------------------------*/
static BaseType_t on_task_failure(uint8_t ucTaskId, TickType_t xMissedByTicks)
{
    printf("[WDG] TIMEOUT: task %s (id=%u) -- last checkin %lums ago (limit=%lums)\n",
           xWatchdogTable[ucTaskId].pcName,
           (unsigned)ucTaskId,
           (unsigned long)pdTICKS_TO_MS(xMissedByTicks),
           (unsigned long)pdTICKS_TO_MS(xWatchdogTable[ucTaskId].xTimeoutTicks));

    printf("[WDG] attempting recovery for task %s\n",
           xWatchdogTable[ucTaskId].pcName);

    /* Recovery attempt: delete and restart the failed task.
       In a minimal embedded system this may not be feasible.
       Here we demonstrate the pattern. */
    if (xWatchdogTable[ucTaskId].xHandle != NULL) {
        eTaskState eState = eTaskGetState(xWatchdogTable[ucTaskId].xHandle);
        (void)eState;   /* would be checked in a real implementation */

        /* In a full implementation: recreate the task here.
           For simplicity, we return pdFALSE to indicate escalation. */
    }

    return pdFALSE;   /* escalate: require system reset */
}

/* -------------------------------------------------------------------------
 * Watchdog task
 *
 * Runs at the highest priority in the system.  Periodically checks the
 * check-in table and feeds the hardware watchdog only if all tasks are
 * healthy.  If any task has timed out, calls on_task_failure() and stops
 * feeding the hardware watchdog (causing it to reset the MCU).
 * -------------------------------------------------------------------------*/
static void watchdog_task(void *pvParameters)
{
    (void)pvParameters;

    TickType_t xLastWakeTime = xTaskGetTickCount();

    printf("[WDG] monitoring %u tasks, period=%ums\n",
           (unsigned)NUM_MONITORED_TASKS, (unsigned)WATCHDOG_PERIOD_MS);

    /* Initialise all check-in timestamps to now so the first period does not
       immediately trigger a false alarm                                       */
    TickType_t xNow = xTaskGetTickCount();
    for (uint8_t i = 0U; i < NUM_MONITORED_TASKS; i++) {
        xWatchdogTable[i].xLastCheckin = xNow;
    }

    for (;;) {
        /* Use vTaskDelayUntil so the watchdog period is precise */
        vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(WATCHDOG_PERIOD_MS));

        xNow = xTaskGetTickCount();
        BaseType_t xAllHealthy = pdTRUE;

        for (uint8_t i = 0U; i < NUM_MONITORED_TASKS; i++) {
            if (xWatchdogTable[i].xEnabled == pdFALSE) {
                continue;
            }

            /* Tick subtraction handles 32-bit wrap-around correctly */
            TickType_t xElapsed = xNow - xWatchdogTable[i].xLastCheckin;

            if (xElapsed > xWatchdogTable[i].xTimeoutTicks) {
                /* Task has exceeded its check-in deadline */
                xAllHealthy = pdFALSE;
                BaseType_t xRecovered = on_task_failure(i, xElapsed);

                if (xRecovered != pdTRUE) {
                    /* Cannot recover -- stop feeding HW watchdog.
                       The hardware watchdog will reset the MCU. */
                    printf("[WDG] SYSTEM RESET (hardware watchdog allowed to expire)\n");
                    /* Do NOT call hardware_wdg_feed() -- enter a spin to let HW WDG fire */
                    taskDISABLE_INTERRUPTS();
                    for (;;) { /* spin: hardware WDG will reset the system */ }
                }
            }
        }

        if (xAllHealthy == pdTRUE) {
            /* All tasks healthy -- feed the hardware watchdog */
            hardware_wdg_feed();
        }
    }
}

/* -------------------------------------------------------------------------
 * Generic monitored task
 *
 * Each instance periodically does work and checks in with the watchdog.
 * The check-in period should be significantly shorter than the watchdog
 * timeout to account for scheduling jitter and execution time variance.
 * -------------------------------------------------------------------------*/
typedef struct {
    uint8_t    ucId;               /* index into xWatchdogTable           */
    uint32_t   uCheckinPeriodMs;   /* how often this task checks in       */
    uint32_t   uWorkDurationMs;    /* simulated work per iteration        */
} TaskParams_t;

static void monitored_task(void *pvParameters)
{
    TaskParams_t  *pxParams = (TaskParams_t *)pvParameters;
    TickType_t     xLastWakeTime = xTaskGetTickCount();
    BaseType_t     xHangSimulated = pdFALSE;
    TickType_t     xStartTick = xTaskGetTickCount();

    for (;;) {
        vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(pxParams->uCheckinPeriodMs));

#if defined(SIMULATE_HANG_TASK_ID)
        /* After HANG_TRIGGER_MS, the specified task stops checking in */
        if ((pxParams->ucId == (uint8_t)SIMULATE_HANG_TASK_ID) &&
            (xHangSimulated == pdFALSE) &&
            (pdTICKS_TO_MS(xTaskGetTickCount() - xStartTick) >= HANG_TRIGGER_MS)) {
            printf("[TASK%u] HANG SIMULATED -- stopping checkins\n",
                   (unsigned)pxParams->ucId);
            xHangSimulated = pdTRUE;
        }
#else
        (void)xStartTick;
        (void)xHangSimulated;
#endif

        if (xHangSimulated == pdFALSE) {
            /* Normal operation: check in with the watchdog */
            watchdog_checkin(pxParams->ucId);
            printf("[TASK%u] checkin #%lu (period=%lums)\n",
                   (unsigned)pxParams->ucId,
                   (unsigned long)xWatchdogTable[pxParams->ucId].uCheckinCount,
                   (unsigned long)pxParams->uCheckinPeriodMs);
        }
        /* If hang is simulated, task continues to run (using CPU) but
           does not check in -- models a task stuck in a busy loop */
    }
}

/* -------------------------------------------------------------------------
 * Application entry point
 * -------------------------------------------------------------------------*/

/* Task parameter blocks: one per monitored task */
static TaskParams_t axTaskParams[NUM_MONITORED_TASKS] = {
    { 0U,  50U, 5U  },   /* TASK0: checks in every 50ms, works for 5ms  */
    { 1U, 200U, 10U },   /* TASK1: checks in every 200ms, works for 10ms */
    { 2U, 100U, 8U  },   /* TASK2: checks in every 100ms, works for 8ms  */
};

int main(void)
{
    printf("=== Software Watchdog with Health Monitoring ===\n");

#if defined(SIMULATE_HANG_TASK_ID)
    printf("NOTE: task %d will hang after %u ms\n\n",
           SIMULATE_HANG_TASK_ID, (unsigned)HANG_TRIGGER_MS);
#else
    printf("NOTE: no hang simulation (all tasks healthy)\n\n");
#endif

    hardware_wdg_start();

    /* Create monitored tasks and record their handles */
    for (uint8_t i = 0U; i < NUM_MONITORED_TASKS; i++) {
        char acName[12];
        snprintf(acName, sizeof(acName), "TASK%u", (unsigned)i);

        const UBaseType_t uxPriorities[NUM_MONITORED_TASKS] = {
            PRIORITY_TASK0, PRIORITY_TASK1, PRIORITY_TASK2
        };

        BaseType_t xResult = xTaskCreate(
            monitored_task,
            acName,
            TASK_STACK_WORDS,
            &axTaskParams[i],
            uxPriorities[i],
            &xWatchdogTable[i].xHandle);   /* store handle for recovery */
        configASSERT(xResult == pdPASS);
    }

    /* Create watchdog task -- highest priority */
    BaseType_t xResult = xTaskCreate(
        watchdog_task,
        "WDG",
        WATCHDOG_STACK_WORDS,
        NULL,
        PRIORITY_WATCHDOG,
        NULL);
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
    printf("[FATAL] malloc failed\n");
    taskDISABLE_INTERRUPTS();
    for (;;) {}
}

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    printf("[FATAL] stack overflow in task: %s\n", pcTaskName);
    taskDISABLE_INTERRUPTS();
    for (;;) {}
}

/* =========================================================================
 * INTERVIEW DISCUSSION NOTES
 *
 * Q: Why must the watchdog task run at the highest priority?
 * A: If a lower-priority task hangs in a busy loop (stuck in a spin or
 *    infinite computation), it will only affect tasks of equal or lower
 *    priority.  A watchdog at the highest priority is preempted only by
 *    ISRs -- it will always get its turn to check the health table, even
 *    if lower-priority tasks are hanging.  If the watchdog were at a lower
 *    priority than the hung task, it would never run and could not detect
 *    the problem.
 *
 * Q: Why is the hardware watchdog necessary alongside the software watchdog?
 * A: The software watchdog monitors application tasks.  But what if the
 *    software watchdog task itself hangs (e.g., due to a kernel bug,
 *    memory corruption, or deadlock in the watchdog's own code)?  The
 *    hardware watchdog monitors the software watchdog: if the software
 *    watchdog stops feeding the hardware watchdog (because it has stopped
 *    running), the hardware watchdog resets the MCU independently.  This
 *    two-level architecture is standard in IEC 61508 SIL-2+ systems.
 *
 * Q: Why use vTaskDelayUntil instead of vTaskDelay in the watchdog task?
 * A: vTaskDelay would introduce cumulative drift: if the watchdog body takes
 *    5 ms, it runs at 105 ms intervals instead of 100 ms.  Over time, this
 *    could allow a timed-out task to go undetected for longer than the
 *    configured timeout.  vTaskDelayUntil maintains a precise 100 ms period
 *    regardless of the watchdog body execution time.
 *
 * Q: Is the xLastCheckin access thread-safe without a mutex?
 * A: On 32-bit ARM Cortex-M, aligned 32-bit writes are single-bus-cycle
 *    atomic operations.  The watchdog task reads the value and the monitored
 *    task writes it.  There is no write-write conflict (only one writer per
 *    entry).  The worst case is the watchdog reads a stale value (one period
 *    old) -- this adds at most one WATCHDOG_PERIOD_MS to the detection
 *    latency, which is acceptable.  For strict atomicity on multi-core or
 *    64-bit targets, use C11 stdatomic.h: atomic_store / atomic_load.
 *
 * Q: What is the minimum hardware watchdog timeout that is safe?
 * A: It must be greater than the worst-case execution time of one watchdog
 *    task period (WATCHDOG_PERIOD_MS) plus any interrupt latency that could
 *    delay the watchdog task from running.  A rule of thumb: set the
 *    hardware watchdog timeout to at least 3x WATCHDOG_PERIOD_MS.  For
 *    example, with a 100 ms software period, set the hardware watchdog to
 *    300-500 ms.  This prevents false resets from scheduling jitter while
 *    still detecting genuine software watchdog failures within 500 ms.
 * =========================================================================*/
