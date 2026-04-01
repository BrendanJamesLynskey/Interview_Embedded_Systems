# Problem 03: Watchdog Strategy for a Multi-Task RTOS System

## Problem Statement

You are designing the watchdog strategy for an industrial motor control unit running FreeRTOS on an STM32H7 (Cortex-M7 at 400 MHz). The system specification is:

| Parameter | Value |
|---|---|
| RTOS | FreeRTOS 10.5 |
| Tasks | 8 tasks of varying priority (see below) |
| Safety standard | IEC 62061 SIL 1 (machinery safety) |
| Watchdog hardware | STM32H7 IWDG (independent watchdog) + WWDG (window watchdog) |
| Recovery requirement | On watchdog reset, system must enter safe state within 100 ms |
| Availability requirement | Spurious watchdog resets must not exceed 1 per year of operation |

**Task inventory:**

| Task Name | Priority | Period | WCET | Function |
|---|---|---|---|---|
| MotorControl | Highest (6) | 1 ms | 300 µs | PID loop, PWM output |
| SafetyMonitor | High (5) | 10 ms | 500 µs | Sensor range checks, fault detection |
| Comms | Medium-high (4) | 20 ms | 2 ms | CAN message handling |
| OTA | Medium (3) | 100 ms | 50 ms | Firmware update download |
| Logger | Medium (2) | 100 ms | 10 ms | Write log entries to flash |
| SensorRead | Low (1) | 50 ms | 5 ms | Read I2C/SPI sensors |
| Diagnostics | Low (1) | 1000 ms | 100 ms | Self-test routines |
| Idle | Lowest (0) | — | — | FreeRTOS idle task |

**Problem:** Design a complete watchdog strategy that:
1. Detects any task that stops executing or exceeds its WCET by 3x.
2. Prevents a high-priority task from starving lower-priority tasks from resetting the watchdog.
3. Handles the case where the OTA task is deliberately blocked for up to 10 minutes during a firmware download.
4. Specifies IWDG and WWDG timeout values with justification.
5. Defines the safe state entry sequence triggered by a watchdog reset.

---

## Design Requirements

- A single hanging task must trigger a watchdog reset within 5 seconds.
- The watchdog task must not add more than 50 µs of CPU time per second.
- The strategy must work correctly when the OTA task is running.

---

## Solution

### Step 1: Watchdog Architecture Overview

A naive approach — kick the IWDG from the highest-priority task — fails because:
1. The MotorControl task (priority 6) runs every 1 ms and will kick the IWDG even when lower-priority tasks are completely starved. A deadlocked Logger task would never be detected.
2. A hung RTOS scheduler that prevents all task switching would also prevent the IWDG kick, but this failure mode requires additional consideration.

The correct architecture uses a **dedicated watchdog task** that collects health tokens from all monitored tasks and only kicks the hardware watchdog when all tokens have been received.

```
Architecture:

  Hardware layer:
    IWDG (independent of CPU clock): safety net for total CPU lockup
    WWDG (AHB clock-dependent):      monitors the watchdog task itself

  RTOS layer:
    WatchdogTask (runs at medium priority 3.5 — between Comms and OTA):
      - Checks that all monitored tasks have checked in within their deadline.
      - Kicks IWDG only if all tasks healthy.
      - Kicks WWDG on its own schedule to prove it is running.

  Application tasks:
    Each task calls wdog_checkin(TASK_ID) at a defined point in its main loop.
    This sets a bit in a shared check-in bitmap.
    A missed check-in causes the WatchdogTask to NOT kick the IWDG,
    which expires and resets the system.
```

### Step 2: Hardware Watchdog Configuration

**IWDG (Independent Watchdog):**

The IWDG runs on the LSI (low-speed internal oscillator, ~32 kHz) and is independent of the main clock, the APB bus, and the RTOS. It is the last-resort protection against total system failure.

```
IWDG timeout selection:

  The WatchdogTask runs and kicks the IWDG every WDT_TASK_PERIOD_MS.
  We choose WDT_TASK_PERIOD_MS = 1000 ms (1 second check interval).

  IWDG must be kicked before it expires.
  Set IWDG timeout = 2 * WDT_TASK_PERIOD_MS = 2000 ms.
  Rationale: 2x the expected kick interval provides headroom for one missed kick
  due to temporary scheduling delays, without creating spurious resets.

  IWDG configuration on STM32H7:
    LSI clock: 32 kHz (typical)
    Prescaler: /256  -> timer clock = 32000/256 = 125 Hz (8 ms per tick)
    Reload value: 2000 ms / 8 ms = 250 ticks

  IWDG->PR   = IWDG_PR_PR_DIV256;   /* prescaler = 256 */
  IWDG->RLR  = 250;                 /* reload = 250 ticks = 2000 ms */
  IWDG->KR   = 0x5555;             /* enable prescaler/reload write */
  IWDG->KR   = 0xCCCC;             /* start IWDG */
```

**WWDG (Window Watchdog):**

The WWDG is driven by the APB1 clock and monitors the WatchdogTask's execution timing. The WWDG requires the kick to occur within a timing window — not too early and not too late.

```
WWDG window configuration:

  WatchdogTask runs at FreeRTOS priority 3.5 (fractional priority can be
  achieved by using priority 4 when not under load; see Step 3).
  Target period: 100 ms.

  WWDG counter clock = APB1_CLK / (4096 * prescaler)
  APB1 on STM32H7 = 100 MHz (typical at 400 MHz CPU).
  Prescaler = /8 -> WWDG clock = 100 MHz / (4096 * 8) = 3051 Hz -> 0.328 ms per tick.

  WWDG counter value: 7-bit (0x40 to 0x7F), effective range: 0x3F counts.
    Full range timeout: 63 * 0.328 ms = 20.6 ms. Too short for 100 ms task.

  Increase prescaler: /8 is the maximum hardware prescaler for WWDG.
  Alternative: use WWDG to protect a faster inner loop within the WatchdogTask.

  Revised approach: WWDG protects the MotorControl task (1 ms period),
  providing a tight hardware check on the most critical real-time task.
  IWDG (2 s) provides the system-level protection.

  WWDG for MotorControl (1 ms period, 3x tolerance = 3 ms window):
    Window = 3 ms. With 0.328 ms per tick: window = 9 ticks.
    WWDG_CFR->W (window value) = 0x7F - 9 = 0x76.
    WWDG_CR->T  (counter)      = 0x7F (start at maximum, count down).

    Kick must occur: when counter is between 0x76 and 0x40 (9-tick window).
    Too early (counter > 0x76): immediate reset.
    Too late (counter reaches 0x3F): reset.
```

### Step 3: Check-In Bitmap and Watchdog Task Design

```c
/* watchdog_task.h */

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include <stdint.h>
#include <stdbool.h>

/* Task IDs for check-in bitmap */
typedef enum {
    WDT_TASK_MOTOR_CTRL  = 0,
    WDT_TASK_SAFETY_MON  = 1,
    WDT_TASK_COMMS       = 2,
    WDT_TASK_SENSOR_READ = 3,
    WDT_TASK_LOGGER      = 4,
    WDT_TASK_DIAGNOSTICS = 5,
    /* OTA task is NOT in this bitmap — see Step 4 */
    WDT_TASK_COUNT       = 6
} wdt_task_id_t;

/* Bitmap: bit N is set when task N has checked in this period */
#define WDT_ALL_TASKS_MASK   ((1U << WDT_TASK_COUNT) - 1U)   /* 0x3F */

static volatile uint32_t  wdt_checkin_bitmap;
static SemaphoreHandle_t  wdt_mutex;   /* protects bitmap updates */

/* Called by each monitored task to register health */
void wdog_checkin(wdt_task_id_t task_id)
{
    BaseType_t rc = xSemaphoreTake(wdt_mutex, pdMS_TO_TICKS(10));
    if (rc == pdPASS) {
        wdt_checkin_bitmap |= (1U << task_id);
        xSemaphoreGive(wdt_mutex);
    }
    /* If mutex cannot be taken within 10 ms: do not check in.
     * The WatchdogTask will detect the missed check-in on its next cycle. */
}
```

```c
/* watchdog_task.c */

#define WDT_TASK_PERIOD_MS     1000U    /* WatchdogTask evaluation period */
#define IWDG_KICK_VALUE        0xAAAAUL /* STM32 IWDG refresh key */

static void watchdog_task(void *params)
{
    (void)params;

    TickType_t last_wake_time = xTaskGetTickCount();

    for (;;) {
        /* Sleep for exactly WDT_TASK_PERIOD_MS.
         * vTaskDelayUntil is used (not vTaskDelay) to prevent drift. */
        vTaskDelayUntil(&last_wake_time, pdMS_TO_TICKS(WDT_TASK_PERIOD_MS));

        /* Read and atomically clear the check-in bitmap */
        taskENTER_CRITICAL();
        uint32_t snapshot = wdt_checkin_bitmap;
        wdt_checkin_bitmap = 0U;
        taskEXIT_CRITICAL();

        /* Verify that all monitored tasks checked in during the last period */
        if ((snapshot & WDT_ALL_TASKS_MASK) == WDT_ALL_TASKS_MASK) {
            /* All tasks are healthy: kick IWDG */
            IWDG->KR = IWDG_KICK_VALUE;
        } else {
            /* One or more tasks missed their check-in.
             * Log which tasks failed (for post-mortem analysis after reset). */
            uint32_t missed = (~snapshot) & WDT_ALL_TASKS_MASK;
            wdt_log_missed_tasks(missed);

            /* Do NOT kick the IWDG. It will expire in (2s - time_since_last_kick)
             * and trigger a system reset. */
        }

        /* Kick WWDG unconditionally: this proves the WatchdogTask itself is running.
         * If WatchdogTask hangs, WWDG fires before IWDG. */
        WWDG->CR = WWDG_CR_WDGA | 0x7FU;   /* reload counter to 0x7F */
    }
}

/* Create the watchdog task */
void watchdog_task_init(void)
{
    wdt_mutex = xSemaphoreCreateMutex();
    configASSERT(wdt_mutex != NULL);

    wdt_checkin_bitmap = 0;

    /* Priority 4: higher than OTA and Logger, lower than Comms.
     * This ensures the WatchdogTask is not starved by high-priority tasks
     * during normal operation, and can run within its 1-second deadline. */
    BaseType_t rc = xTaskCreate(watchdog_task, "WDT",
                                 configMINIMAL_STACK_SIZE * 2,
                                 NULL,
                                 4,   /* priority */
                                 NULL);
    configASSERT(rc == pdPASS);
}
```

### Step 4: Handling the OTA Task

The OTA task is deliberately excluded from the check-in bitmap. During a firmware download, the OTA task may block on network I/O for up to 10 minutes waiting for data. Requiring it to check in every second would force either:
1. A 10-minute IWDG timeout — which defeats the purpose of a watchdog.
2. The OTA task checking in even when blocked — which hides real hangs.

**Solution: OTA watchdog suspension with timeout.**

```c
/* ota_watchdog.c */

#define OTA_MAX_SUSPENSION_MS  (10U * 60U * 1000U)   /* 10 minutes */

static bool     ota_suspended       = false;
static uint32_t ota_suspend_start_ms;

/* Called by OTA task before beginning a download */
void ota_wdog_suspend(void)
{
    taskENTER_CRITICAL();
    ota_suspended         = true;
    ota_suspend_start_ms  = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    taskEXIT_CRITICAL();
}

/* Called by OTA task after download completes (success or failure) */
void ota_wdog_resume(void)
{
    taskENTER_CRITICAL();
    ota_suspended = false;
    taskEXIT_CRITICAL();
}

/* Called by WatchdogTask as part of its health evaluation.
 * Returns true if OTA suspension is valid (within time limit).
 * Returns false if OTA has been suspended for too long. */
bool ota_wdog_check(void)
{
    if (!ota_suspended) {
        return true;   /* OTA not suspended: no action needed */
    }

    uint32_t now_ms   = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    uint32_t elapsed  = now_ms - ota_suspend_start_ms;

    if (elapsed > OTA_MAX_SUSPENSION_MS) {
        /* OTA has been suspended for over 10 minutes.
         * This is abnormal: the download should have completed or timed out. */
        return false;   /* trigger watchdog: OTA appears hung */
    }

    return true;   /* OTA suspension within allowed window */
}
```

**Revised WatchdogTask health check:**

```c
/* In watchdog_task(), replace the simple bitmap check: */

bool all_healthy = ((snapshot & WDT_ALL_TASKS_MASK) == WDT_ALL_TASKS_MASK)
                   && ota_wdog_check();

if (all_healthy) {
    IWDG->KR = IWDG_KICK_VALUE;
} else {
    wdt_log_missed_tasks((~snapshot) & WDT_ALL_TASKS_MASK);
    /* Do not kick: IWDG will fire */
}
```

**OTA task integration:**

```c
void ota_task(void *params)
{
    for (;;) {
        /* Wait for OTA trigger from Comms task */
        xTaskNotifyWait(0, 0, NULL, portMAX_DELAY);

        ota_wdog_suspend();              /* inform watchdog: download starting */

        ota_result_t result = ota_download_and_validate();

        ota_wdog_resume();               /* inform watchdog: download complete */
        ota_handle_result(result);
    }
}
```

### Step 5: Check-In Placement in Each Task

Check-in placement matters: it must occur at a point that proves the task completed its main functional work, not at an arbitrary point that the task can reach even when malfunctioning.

```c
/* MotorControl task: check in after completing the PID calculation and PWM update */
void motor_control_task(void *params)
{
    TickType_t last_wake_time = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&last_wake_time, pdMS_TO_TICKS(1));

        /* Read encoder, compute PID, write PWM */
        float position  = encoder_read();
        float error     = setpoint - position;
        float output    = pid_compute(&pid_state, error, 0.001f);
        pwm_set_duty(output);

        /* Check in: proves this entire execution path completed */
        wdog_checkin(WDT_TASK_MOTOR_CTRL);

        /* Also kick WWDG: MotorControl runs every 1 ms, within WWDG window */
        WWDG->CR = WWDG_CR_WDGA | 0x7FU;
    }
}

/* SafetyMonitor task: check in after completing all safety checks */
void safety_monitor_task(void *params)
{
    TickType_t last_wake_time = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&last_wake_time, pdMS_TO_TICKS(10));

        bool all_safe = true;
        all_safe &= check_temperature_sensor();
        all_safe &= check_current_sensor();
        all_safe &= check_voltage_sensor();
        all_safe &= validate_motor_state();

        if (!all_safe) {
            trigger_safe_state(REASON_SENSOR_FAULT);
        }

        wdog_checkin(WDT_TASK_SAFETY_MON);
    }
}

/* Logger task: check in after each log write attempt.
 * Note: Logger calls wdog_checkin even if the log write fails.
 * The checkin proves the task ran, not that the write succeeded.
 * Log write failures are handled separately via error counters. */
void logger_task(void *params)
{
    TickType_t last_wake_time = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&last_wake_time, pdMS_TO_TICKS(100));

        log_entry_t *entry = log_queue_dequeue();
        if (entry != NULL) {
            flash_write_log(entry);
            log_entry_free(entry);
        }

        wdog_checkin(WDT_TASK_LOGGER);
    }
}
```

### Step 6: Safe State Entry on Watchdog Reset

On reset, the firmware must quickly establish whether the reset was caused by the watchdog and enter a safe state before re-initialising normal operations.

```c
/* safe_state.c — executed at the start of main(), before RTOS starts */

void safe_state_check_on_boot(void)
{
    uint32_t reset_flags = RCC->RSR;   /* Reset source register on STM32H7 */

    bool iwdg_reset = (reset_flags & RCC_RSR_IWDGRSTF) != 0U;
    bool wwdg_reset = (reset_flags & RCC_RSR_WWDGRSTF) != 0U;

    if (iwdg_reset || wwdg_reset) {
        /* Step 1: Enter hardware safe state immediately.
         * This must happen within 100 ms of reset (SIL 1 requirement). */
        motor_emergency_stop();    /* disable PWM outputs, engage brake relay */
        alarm_set_active(true);    /* activate external alarm indicator */

        /* Step 2: Log the reset event with pre-reset diagnostic data.
         * The backup RAM (BKPSRAM) retains data through a watchdog reset.
         * Before reset, the WatchdogTask wrote the missed-task bitmap to BKPSRAM. */
        uint32_t missed_tasks = BKPSRAM->missed_task_bitmap;
        uint32_t reset_count  = BKPSRAM->wdog_reset_count + 1;
        BKPSRAM->wdog_reset_count = reset_count;

        persistent_log_write(LOG_LEVEL_CRITICAL,
                              "Watchdog reset #%lu. Missed tasks: 0x%02lX. "
                              "Motor state at reset: speed=%d RPM, current=%d mA",
                              reset_count, missed_tasks,
                              BKPSRAM->last_speed_rpm,
                              BKPSRAM->last_current_ma);

        /* Step 3: If reset count exceeds threshold (3 consecutive watchdog resets),
         * do not attempt to restart normal operation. Enter permanent safe state
         * and wait for operator intervention. */
        if (reset_count >= 3) {
            enter_permanent_safe_state();   /* never returns */
        }

        /* Step 4: Clear reset flags for next boot */
        RCC->RSR |= RCC_RSR_RMVF;   /* clear all reset flags */

        /* Step 5: Start RTOS with reduced task set for diagnostic boot.
         * Do not start OTA, Logger, or Diagnostics until system is verified. */
        boot_mode = BOOT_MODE_SAFE_RECOVERY;
    } else {
        boot_mode = BOOT_MODE_NORMAL;
        RCC->RSR |= RCC_RSR_RMVF;
    }
}

/* Permanent safe state: motor off, alarm on, CAN heartbeat with fault code.
 * Requires operator to power-cycle the unit to clear. */
static void enter_permanent_safe_state(void)
{
    motor_emergency_stop();
    alarm_set_active(true);
    can_send_fault_frame(CAN_FAULT_CONSECUTIVE_WDT_RESETS);

    /* Disable IWDG kick from this point: the system stays reset-free
     * only because no task is running the main loop. We must ensure the
     * IWDG is NOT kicked by anything, so it will not expire (we reload it
     * once here to give the CAN message time to transmit). */
    IWDG->KR = 0xAAAAUL;   /* one final reload — then stop kicking */

    /* Infinite loop: CAN heartbeat only */
    while (1) {
        can_send_heartbeat(HEARTBEAT_SAFE_STATE_ACTIVE);
        HAL_Delay(1000);
        /* IWDG will fire every 2 seconds from here, causing resets.
         * Each reset: motor brake engaged by safe state check above.
         * CAN message informs supervisory system. */
    }
}
```

### Step 7: Pre-Reset State Logging (BKPSRAM Usage)

To enable root cause analysis after a watchdog reset, critical state is saved to Backup SRAM (BKPSRAM) which survives a watchdog reset on the STM32H7:

```c
/* Updated WatchdogTask: save state to BKPSRAM before not kicking IWDG */

/* In watchdog_task(), on detection of missed check-in: */
if (!all_healthy) {
    /* Log missed tasks to BKPSRAM before the impending reset */
    BKPSRAM->missed_task_bitmap  = (~snapshot) & WDT_ALL_TASKS_MASK;
    BKPSRAM->motor_speed_rpm     = motor_get_speed_rpm();
    BKPSRAM->motor_current_ma    = motor_get_current_ma();
    BKPSRAM->last_wdog_fail_tick = xTaskGetTickCount();

    /* DSB: ensure writes reach BKPSRAM before reset */
    __DSB();

    /* Do NOT kick IWDG */
}
```

---

## Watchdog Configuration Summary

| Parameter | Value | Justification |
|---|---|---|
| IWDG timeout | 2000 ms | 2x the WatchdogTask period (1000 ms); allows one missed kick without spurious reset |
| WWDG window | ~3 ms (9 ticks at 0.328 ms/tick) | Protects MotorControl task; 3x the task period (1 ms); detects both runaway (too fast) and hang (too slow) |
| WatchdogTask period | 1000 ms | Balances detection latency (< 2 s with IWDG) vs CPU overhead (<1 µs/s) |
| OTA suspension limit | 10 minutes | Matches maximum expected download time on constrained link; beyond this, OTA is considered hung |
| Max watchdog resets before lockout | 3 | Prevents infinite reset loop on systematic faults; requires operator intervention |
| Check-in granularity | Once per task period | Proves the complete execution path ran, not just that the task started |

---

## Key Takeaways

1. **Never kick the watchdog from the highest-priority task alone.** The entire point of a multi-task watchdog strategy is to detect individual task failures. A high-priority task that always kicks the watchdog masks all lower-priority task failures.

2. **The watchdog task itself is a single point of failure.** The WWDG protects the watchdog task. If the watchdog task hangs, the WWDG fires. This creates a two-layer hardware protection: WWDG for the watchdog mechanism, IWDG for the application tasks.

3. **BKPSRAM is invaluable for post-mortem analysis.** Watchdog resets in the field are notoriously difficult to diagnose without pre-reset state. Investing in structured BKPSRAM logging pays significant dividends in field support time.

4. **OTA and maintenance tasks must be explicitly excluded from the normal health model.** Design the exclusion before implementing OTA, not as a workaround after it causes spurious resets.

5. **Define what "checked in" means carefully.** A check-in at the start of the task loop proves the task started. A check-in at the end proves it completed. For safety, check in at the end — after all safety-critical work has been done.

6. **Test the watchdog.** Deliberately inject failures (block a task with a semaphore, inject an infinite loop via debug JTAG) and verify the watchdog fires within the expected time and the safe state is correctly entered. Watchdog strategies that are never tested frequently fail silently due to configuration bugs (wrong prescaler, accidental background IWDG kick from an interrupt handler).
