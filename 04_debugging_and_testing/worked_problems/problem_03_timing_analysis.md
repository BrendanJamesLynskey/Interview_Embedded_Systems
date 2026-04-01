# Worked Problem 03: Timing Analysis in a Real-Time System

## Problem Statement

A motor controller runs on a Cortex-M4 at 168 MHz under FreeRTOS. The control loop runs in a high-priority task at a nominal 1 kHz rate (1 ms period). The motor behaves erratically: it occasionally stutters, overcorrects, and sometimes trips a hardware overcurrent fault. The overcurrent occurs roughly every 30-60 seconds and is non-deterministic.

System task summary:

```
Task          Priority   Period / Trigger       Function
-----------   --------   --------------------   -----------------------------------
MotorCtrl     5          1 kHz timer            PID control loop, sends PWM setpoints
CommTask      4          Event-driven           Receives CAN frames, updates setpoints
SensorTask    3          5 ms timer             Reads ADC (current, voltage)
LogTask       2          100 ms timer           Logs data to SD card via SPI
IdleTask      0          Continuous             FreeRTOS idle hook
```

The motor engineer says: "The PID algorithm is correct when tested in simulation. Something is wrong with timing." You have been asked to investigate.

**What would you do?**

---

## Background: Real-Time Timing Concepts

### Jitter, Latency, and Deadline

```
Ideal periodic task (1 kHz, period = 1 ms):

  |----|----|----|----|----|----|----|
  t0   t1   t2   t3   t4   t5   t6     (ideal: exactly 1 ms apart)

Real execution:

  |----|----|----|----|----|----|----|
  t0   t1  t2    t3   t4 t5    t6      (actual: varies by jitter)

Jitter:       Max deviation from expected period = t_actual - t_expected
Latency:      Time from event (timer fires) to task actually running
Deadline:     Latest acceptable time for task completion
              Motor control: if control law not applied within 1.1 ms, overcurrent risk

Worst-case latency contributors:
  1. FreeRTOS tick interrupt latency      (typically < 5 µs)
  2. Higher-priority interrupt blocking   (depends on ISR duration)
  3. Critical sections (taskENTER_CRITICAL)
  4. Interrupt-disabled periods
  5. Higher-priority task preemption      (CommTask at priority 4 can delay MotorCtrl at 5)
     Wait -- actually MotorCtrl is priority 5 (higher), so lower-priority tasks
     cannot delay it. Let us re-examine...
```

**Priority confusion is the first clue:** In FreeRTOS, higher priority number means higher priority. MotorCtrl at priority 5 will preempt CommTask at priority 4. So CommTask cannot delay MotorCtrl.

What CAN delay MotorCtrl:
- Interrupt service routines (interrupts preempt all tasks)
- Critical sections that disable the scheduler
- The FreeRTOS tick handler (the scheduler itself)
- Any ISR that calls `xQueueSendFromISR()` or similar and causes a context switch overhead

---

## Solution: Step-by-Step

### Step 1 — Add GPIO Timing Markers

The cheapest, highest-precision timing measurement method: toggle a dedicated GPIO at the start and end of the critical section, then measure with an oscilloscope.

```c
/* In motor_ctrl_task.c */

/* Use a GPIO dedicated to timing measurement (not connected to anything else) */
#define TIMING_GPIO_PORT   GPIOD
#define TIMING_PIN_CTRL    GPIO_PIN_0   /* MotorCtrl task active */
#define TIMING_PIN_SENSOR  GPIO_PIN_1   /* SensorTask active */
#define TIMING_PIN_LOG     GPIO_PIN_2   /* LogTask active */
#define TIMING_PIN_COMM    GPIO_PIN_3   /* CommTask active */

/* Call these in the task function itself */
#define TIMING_SET(pin)    (TIMING_GPIO_PORT->BSRR = (pin))
#define TIMING_CLEAR(pin)  (TIMING_GPIO_PORT->BRR  = (pin))

void vMotorCtrlTask(void *params) {
    TickType_t xLastWakeTime = xTaskGetTickCount();
    const TickType_t xFrequency = pdMS_TO_TICKS(1);  /* 1 ms */

    for (;;) {
        vTaskDelayUntil(&xLastWakeTime, xFrequency);

        TIMING_SET(TIMING_PIN_CTRL);    /* GPIO high: task running */

        /* Read latest sensor values (shared with SensorTask) */
        float current = sensor_get_current();
        float velocity = sensor_get_velocity();

        /* Execute PID control law */
        float pwm_duty = pid_compute(&motor_pid, velocity, g_setpoint);

        /* Apply to PWM hardware */
        pwm_set_duty(pwm_duty);

        TIMING_CLEAR(TIMING_PIN_CTRL);  /* GPIO low: task complete */
    }
}
```

**Oscilloscope measurement setup:**

```
CH1: TIMING_PIN_CTRL (MotorCtrl active indicator)
CH2: TIMING_PIN_LOG  (LogTask active indicator)

Trigger: rising edge on CH1
Timebase: 1 ms/div

Expected:
  CH1 pulses every 1 ms, each pulse lasting ~10-50 µs (PID computation time).
  CH2 pulses occasionally (100 ms period), not overlapping CH1.

Observed (with the problem):
  CH1 pulses are NOT exactly 1 ms apart. Some are 1.2-1.5 ms.
  The delayed pulses correlate exactly with CH2 going high.
```

**Root cause identified: LogTask is delaying MotorCtrl.**

Wait -- LogTask is priority 2 and MotorCtrl is priority 5. How can a lower-priority task delay a higher-priority task?

---

### Step 2 — Investigate the Delay Mechanism

A lower-priority task can delay a higher-priority task through **shared resource contention**. If MotorCtrl waits for a mutex that LogTask holds, MotorCtrl is blocked until LogTask releases the mutex.

```c
/* Examine the sensor reading code */
float sensor_get_current(void) {
    /* Acquire mutex protecting the shared sensor data structure */
    xSemaphoreTake(sensor_data_mutex, portMAX_DELAY);
    float val = g_sensor_data.current;
    xSemaphoreGive(sensor_data_mutex);
    return val;
}

/* Examine LogTask */
void vLogTask(void *params) {
    for (;;) {
        /* Acquire the same mutex to get a consistent snapshot for logging */
        xSemaphoreTake(sensor_data_mutex, portMAX_DELAY);

        /* Build a log entry -- this takes time (string formatting) */
        char log_line[128];
        snprintf(log_line, sizeof(log_line),
                 "t=%lu I=%.3f V=%.3f spd=%.3f\r\n",
                 xTaskGetTickCount(),
                 g_sensor_data.current,
                 g_sensor_data.voltage,
                 g_sensor_data.velocity);

        /* Write to SD card via SPI -- can take 1-5 ms! */
        sd_write_line(log_line);

        xSemaphoreGive(sensor_data_mutex);  /* release only AFTER SD write */

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
```

**The priority inversion scenario:**

```
Timeline:

t = 0.000 ms  LogTask (priority 2) acquires sensor_data_mutex.
t = 0.100 ms  LogTask begins sd_write_line() (slow SPI, 2 ms typical).
t = 1.000 ms  MotorCtrl (priority 5) wakes from vTaskDelayUntil().
              MotorCtrl calls sensor_get_current() -> xSemaphoreTake().
              sensor_data_mutex is held by LogTask -- MotorCtrl BLOCKS.
t = 2.100 ms  LogTask completes sd_write_line(), releases mutex.
              MotorCtrl unblocks and continues.
              MotorCtrl has been delayed by 1.1 ms -- past its deadline.

Intermittent aspect: the crash only happens when LogTask starts its SPI
write just before MotorCtrl's 1 ms wake tick. If LogTask's 100 ms period
happens to align with MotorCtrl's 1 ms period (once every 100 cycles =
every 100 ms), the overlap is possible. Hence the ~30-60 second interval
between overcurrent faults.
```

---

### Step 3 — Confirm with Logic Analyser

```
Logic analyser channels:
  CH1: MotorCtrl GPIO  (TIMING_PIN_CTRL)
  CH2: LogTask GPIO    (TIMING_PIN_LOG)
  CH3: SPI SCK         (SD card SPI clock, to see when SPI transaction occurs)
  CH4: SPI CS#         (SD card chip select)

Capture: 200 ms window, triggered on anomalous MotorCtrl pulse interval

Observation:
  Normal 1 ms period on CH1.
  Anomalous 2.1 ms gap on CH1 -- MotorCtrl did not run for an extra 1.1 ms.
  During that 1.1 ms: CH2 (LogTask) is high, CH3/CH4 show SPI activity.
  Proof: MotorCtrl was blocked waiting for the mutex held by LogTask during SPI.
```

```
Cursor measurements:
  A: falling edge of CH1 (MotorCtrl completed previous iteration)
  B: rising edge of CH1 (MotorCtrl resumed after being blocked)
  B - A = 2.143 ms   (should be 1.000 ms)
  Excess delay = 1.143 ms

  C: rising edge of CH4 (SPI transaction start, within LogTask)
  D: falling edge of CH4 (SPI transaction end)
  D - C = 1.150 ms   (matches the excess delay exactly)

Conclusion: MotorCtrl was blocked for exactly the duration of the SPI transaction.
```

---

### Step 4 — Fix: Restructure the Locking Strategy

The root problem is that `LogTask` holds a mutex across a slow I/O operation. The fix is to copy the shared data under a short mutex hold, release the mutex, then do the slow I/O without the mutex.

```c
/* BEFORE (bad): holds mutex across SPI write */
void vLogTask_bad(void *params) {
    for (;;) {
        xSemaphoreTake(sensor_data_mutex, portMAX_DELAY);
        /* CRITICAL SECTION (mutex held throughout): */
        snprintf(log_line, ...);
        sd_write_line(log_line);        /* slow! */
        /* END CRITICAL SECTION */
        xSemaphoreGive(sensor_data_mutex);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/* AFTER (correct): hold mutex only for the data copy */
void vLogTask_fixed(void *params) {
    SensorData_t local_snapshot;
    char log_line[128];

    for (;;) {
        /* Hold mutex only long enough to copy the data -- microseconds */
        xSemaphoreTake(sensor_data_mutex, portMAX_DELAY);
        local_snapshot = g_sensor_data;   /* struct copy -- very fast */
        xSemaphoreGive(sensor_data_mutex);

        /* Build log line WITHOUT holding the mutex */
        snprintf(log_line, sizeof(log_line),
                 "t=%lu I=%.3f V=%.3f spd=%.3f\r\n",
                 xTaskGetTickCount(),
                 local_snapshot.current,
                 local_snapshot.voltage,
                 local_snapshot.velocity);

        /* Write to SD WITHOUT holding the mutex */
        sd_write_line(log_line);

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
```

**After fix:** The mutex is held for only ~1-2 µs (the struct copy). MotorCtrl is blocked for at most 2 µs waiting for the mutex, well within its 1 ms deadline.

---

### Step 5 — Measure Maximum PID Execution Time

Now that the priority inversion is fixed, measure the worst-case execution time of the MotorCtrl task itself using the GPIO marker:

```c
/* Add cycle count measurement using DWT cycle counter */
void vMotorCtrlTask(void *params) {
    TickType_t xLastWakeTime = xTaskGetTickCount();
    const TickType_t xFrequency = pdMS_TO_TICKS(1);

    uint32_t max_cycles = 0;

    for (;;) {
        vTaskDelayUntil(&xLastWakeTime, xFrequency);

        TIMING_SET(TIMING_PIN_CTRL);
        uint32_t t_start = DWT->CYCCNT;    /* read hardware cycle counter */

        float current  = sensor_get_current();
        float velocity = sensor_get_velocity();
        float pwm_duty = pid_compute(&motor_pid, velocity, g_setpoint);
        pwm_set_duty(pwm_duty);

        uint32_t t_end    = DWT->CYCCNT;
        uint32_t cycles   = t_end - t_start;
        uint32_t us_taken = cycles / 168;  /* 168 MHz => 168 cycles per µs */

        TIMING_CLEAR(TIMING_PIN_CTRL);

        /* Track maximum execution time */
        if (cycles > max_cycles) {
            max_cycles = cycles;
            debug_printf("MotorCtrl new max: %u cycles = %u µs\r\n",
                         max_cycles, us_taken);
        }

        /* Assert we meet deadline (100 µs budget for 10% margin on 1 ms period) */
        if (us_taken > 100) {
            debug_printf("MotorCtrl DEADLINE MISS: %u µs\r\n", us_taken);
        }
    }
}

/* Enable DWT cycle counter (must be done in initialisation): */
void dwt_init(void) {
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;  /* enable DWT */
    DWT->CYCCNT       = 0;
    DWT->CTRL        |= DWT_CTRL_CYCCNTENA_Msk;       /* start counting */
}
```

**Typical output after fix:**

```
MotorCtrl new max: 3024 cycles = 18 µs
MotorCtrl new max: 3108 cycles = 18 µs
```

18 µs execution time with 1 ms period = 1.8% CPU utilisation. No deadline misses.

---

### Step 6 — Verify Jitter is Acceptable

Jitter is the variation in task start time relative to the expected 1 ms period. Even with correct priority and no blocking, some jitter exists due to FreeRTOS tick overhead and ISR timing:

```c
/* Measure inter-arrival time using DWT */
void vMotorCtrlTask(void *params) {
    TickType_t xLastWakeTime = xTaskGetTickCount();
    const TickType_t xFrequency = pdMS_TO_TICKS(1);

    uint32_t last_start = DWT->CYCCNT;
    uint32_t max_period_cycles = 0;
    uint32_t min_period_cycles = UINT32_MAX;

    for (;;) {
        vTaskDelayUntil(&xLastWakeTime, xFrequency);

        uint32_t now    = DWT->CYCCNT;
        uint32_t period = now - last_start;
        last_start = now;

        /* Expected: 168000 cycles (1 ms at 168 MHz) */
        if (period > max_period_cycles) max_period_cycles = period;
        if (period < min_period_cycles) min_period_cycles = period;

        /* Log jitter statistics every 1000 iterations (= 1 second) */
        static uint32_t count = 0;
        if (++count >= 1000) {
            uint32_t jitter_us = (max_period_cycles - 168000) / 168;
            debug_printf("1s jitter: max=%u min=%u jitter=%u µs\r\n",
                         max_period_cycles, min_period_cycles, jitter_us);
            max_period_cycles = 0;
            min_period_cycles = UINT32_MAX;
            count = 0;
        }

        /* ... rest of control loop ... */
    }
}
```

**Example output after fix:**

```
1s jitter: max=168643 min=167891 jitter=4 µs
1s jitter: max=168521 min=167943 jitter=3 µs
```

Maximum jitter of 4 µs on a 1 ms period. This is well within the motor control tolerance.

---

### Step 7 — Identify Remaining Interrupt Latency Sources

Even without priority inversion, interrupt service routines preempt all tasks. If a long ISR runs during MotorCtrl's window, it adds latency. Profile ISR durations:

```c
/* Measure ISR duration using GPIO (on oscilloscope, not logic analyser -- need µs resolution) */

/* In the CAN receive ISR: */
void CAN1_RX0_IRQHandler(void) {
    TIMING_SET(TIMING_PIN_CAN_ISR);   /* CH4 on oscilloscope */

    /* Process received CAN frames -- how long does this take? */
    can_process_rx_fifo();

    TIMING_CLEAR(TIMING_PIN_CAN_ISR);
}
```

If `can_process_rx_fifo()` takes more than 50 µs (due to, say, deserialising many messages in a burst), this will add latency to the MotorCtrl task start. Solutions:

1. **Minimise ISR work:** The ISR only copies the raw CAN frame to a queue. Processing happens in CommTask.
2. **Set ISR priority appropriately:** CAN ISR should be at a lower priority than the FreeRTOS tick interrupt if possible, so FreeRTOS can still switch tasks during the ISR.
3. **Use `taskENTER_CRITICAL_FROM_ISR()` only when necessary:** Critical sections disable interrupts entirely, preventing the FreeRTOS tick from running.

---

## Summary: Timing Analysis Methodology

```
Step  Tool                   What to measure               What to look for
----  ---------------------  ----------------------------  ------------------------------------
1     GPIO + oscilloscope    Task execution time           > deadline? Longer than expected?
2     GPIO + oscilloscope    Inter-task correlation        Does LogTask activity correlate
                                                           with MotorCtrl delays?
3     Logic analyser         SPI/I2C timing during         Long SPI transactions while
                             MotorCtrl delay               MotorCtrl is blocked?
4     DWT cycle counter      Worst-case execution time     Does worst case fit in deadline?
5     DWT cycle counter      Jitter measurement            Jitter < 10% of period?
6     GPIO + oscilloscope    ISR duration                  Any ISR > 50 µs?
7     FreeRTOS trace         Full task timeline            vTaskGetRunTimeStats() for CPU %
```

**FreeRTOS run-time statistics** (enabled with `configGENERATE_RUN_TIME_STATS = 1`):

```c
void print_task_stats(void) {
    char stats_buffer[512];
    vTaskGetRunTimeStats(stats_buffer);
    debug_printf("Task Name       Abs Time    %% Time\r\n");
    debug_printf("%s\r\n", stats_buffer);
}

/* Example output:
   Task Name       Abs Time    % Time
   MotorCtrl       18420       1%
   CommTask        5210        0%
   SensorTask      12300       1%
   LogTask         45800       4%
   IDLE            1618270     94%

   If LogTask showed 30%+ CPU: excessive SPI write time is confirmed.
   If MotorCtrl shows 40%+: PID computation is too slow for 1 kHz. */
```

---

## Interview Discussion Points

**Q: What is priority inversion and how does FreeRTOS address it?**

Priority inversion is when a high-priority task is indirectly blocked by a low-priority task through a shared resource. In this problem, MotorCtrl (priority 5) is blocked by LogTask (priority 2) via the mutex.

FreeRTOS mutexes (created with `xSemaphoreCreateMutex()`) implement **priority inheritance**: when a high-priority task blocks waiting for a mutex held by a lower-priority task, the low-priority task's priority is temporarily raised to match the high-priority task. This prevents medium-priority tasks (CommTask at priority 4) from preempting the mutex holder and extending the block time.

In this problem: with priority inheritance, LogTask's priority would be raised to 5 while it holds the mutex MotorCtrl is waiting for. LogTask would complete its SPI write without CommTask preempting it. However, the block time is still the SPI write duration. The real fix is still to shorten the critical section.

**Q: Why does `vTaskDelayUntil()` provide better period accuracy than `vTaskDelay()`?**

`vTaskDelay(100)` delays 100 ms from the moment the call is executed. If the task body took 2 ms to execute, the effective period is 102 ms.

`vTaskDelayUntil(&xLastWakeTime, 100)` delays until the absolute tick count `xLastWakeTime + 100`, then updates `xLastWakeTime`. The body execution time is automatically compensated. Periods remain accurate as long as the body finishes before the next deadline.

**Q: The motor engineer says "the algorithm is correct in simulation." What does simulation miss?**

Simulation typically models the algorithm at ideal sample intervals and ignores:
- Jitter in actual sampling time (sensor values read at wrong time)
- Execution time of the control law (assumed zero in simulation)
- Quantisation of PWM duty cycle (hardware resolution limit)
- ADC sampling time and conversion latency
- Communication latency for setpoint updates (CAN frame reception time)
- Mutual exclusion overhead (blocked reading sensor values)

A real control system must account for all of these. A 1.1 ms delay in applying the control output to a fast-response motor is equivalent to running the control loop at 0.9 kHz instead of 1 kHz, which changes the closed-loop stability margins. If the PID was tuned for exactly 1 kHz sampling, intermittent 2 kHz or 0.5 kHz effective rates (during priority inversion) can cause the controller to go unstable and overshoot current limits.
