# Low Power Design

## Prerequisites
- ARM Cortex-M sleep mode architecture (WFI, WFE, SLEEPDEEP)
- Basic CMOS power consumption: dynamic and static (leakage)
- RTOS tickless idle mode concepts
- Peripheral clock gating and power domain concepts

---

## Concept Reference

### Sources of Power Consumption in an Embedded System

```
Total system power = P_dynamic + P_static + P_peripheral

P_dynamic  = alpha * C * V^2 * f
  alpha = activity factor (fraction of gates switching per cycle)
  C     = total capacitance (gate, wire, I/O)
  V     = supply voltage
  f     = clock frequency

  Key insight: dynamic power scales with V^2 and linearly with f.
  Halving the voltage reduces dynamic power by 4x.
  Halving the clock reduces dynamic power by 2x (but increases latency).

P_static = I_leak * V
  I_leak = sum of all subthreshold leakage currents across the die.
  Dominant at high temperature and in advanced process nodes.
  Does not scale with activity — leakage flows even when the CPU is idle.

P_peripheral = sum of quiescent currents of all active peripherals.
  Examples: LDO regulator quiescent current (~50–200 µA),
            crystal oscillator (~200–500 µA),
            ADC continuous conversion (~1–5 mA),
            BLE radio during advertisement (~3–15 mA peak).
```

### ARM Cortex-M Sleep Modes

```
Mode          Depth    Clock state                Wake latency    Typical MCU current
------------  -------  -------------------------  --------------  --------------------
Run           Awake    CPU clock running          N/A             5–50 mA (active)
Sleep (WFI)   Shallow  CPU clock gated, AHB/APB   1–2 cycles      1–10 mA
                       peripherals optional on   
STOP          Deep     All clocks off except RTC  100–300 µs      5–100 µA
              sleep    or LPTIMER; core regs      (re-lock PLL)
                       retained; SRAM retained
STANDBY       Deep     All clocks off; most       1–4 ms          1–15 µA
              sleep    SRAM off; only BKP SRAM;   (full re-init)
                       wakeup pin or RTC only
SHUTDOWN      Off      Everything off except       50–100 ms       0.1–1 µA
                       wakeup pin; SRAM lost;      (full cold boot)
                       registers lost
```

The exact mode names and register bits vary by vendor. On STM32, these modes are controlled by:
- `PWR_CR1`: LPMS (Low Power Mode Selection) bits.
- `SCB->SCR`: SLEEPDEEP bit selects between Sleep and STOP/STANDBY/SHUTDOWN.
- `__WFI()` / `__WFE()`: ARM instructions that trigger the sleep entry.

### Wake Sources

Each sleep mode restricts which events can wake the device:

```
Sleep (WFI/WFE):
  - Any interrupt request (IRQ) from any enabled peripheral.
  - An event signal on the WFE input pin (SEV instruction from another core).

STOP mode:
  - Externalinterrupt on EXTI line (GPIO edge trigger).
  - RTC alarm, RTC wakeup timer.
  - Low-power timer (LPTIM) — runs on LSE/LSI clock even in STOP.
  - USART/LPUART wakeup (start bit detection on RX pin).
  - I2C address match wakeup.
  - USB wakeup detect (resume signalling on D+ line).
  - Comparator output trigger (analog voltage threshold wakeup).

STANDBY mode:
  - Wakeup pins (WKUPx, edge-triggered).
  - RTC alarm or periodic wakeup counter.
  - IWDG (triggers a RESET rather than a controlled wakeup).

SHUTDOWN mode:
  - Wakeup pins only.
  - No RTC in most implementations (or limited battery-backed RTC).
```

### Power Budget Analysis

A power budget quantifies every current source and computes average current, which determines battery life:

```
Average current (duty-cycle model):

  I_avg = sum_i (I_i * t_i) / T_total

  Where:
    I_i   = current drawn in state i (mA)
    t_i   = time spent in state i per cycle (ms)
    T_total = total period of one full duty cycle (ms)

Battery life:

  Life (hours) = Battery_capacity_mAh / I_avg_mA

  Example: 500 mAh battery at I_avg = 50 µA:
    Life = 500 / 0.050 = 10,000 hours = 416 days
```

### Peripheral Power Gating

Power gating turns off supply voltage to a peripheral block entirely, eliminating both dynamic and static (leakage) power:

```
Software-controlled peripheral power gating (generic sequence):

  1. Disable the peripheral (clear its enable bit).
  2. Wait for the peripheral to complete any in-progress transaction.
  3. Disable the peripheral's clock (clear its clock enable bit in RCC/CCM).
     This stops dynamic power consumption.
  4. Assert the peripheral's reset (set its reset bit in RCC).
     Required so the peripheral initialises cleanly on power-up.
  5. (Optional) Disable the power supply to the peripheral's power domain
     by opening the domain's power switch (if the SoC supports independent
     power domains per peripheral group).

Example on STM32F4 (clock gating only — no independent power domains):
  /* Disable UART2 clock to gate its power */
  __HAL_RCC_USART2_CLK_DISABLE();

  /* Re-enable later */
  __HAL_RCC_USART2_CLK_ENABLE();
  /* Re-initialise UART register configuration — clocking was off */
  UART_Init(&huart2);
```

### RTOS Tickless Idle Mode

Standard RTOS operation fires a periodic tick interrupt (e.g., 1 kHz = every 1 ms). This wakes the CPU 1000 times per second even when no task is runnable, imposing a minimum average power floor.

**Tickless idle** suppresses the tick interrupt when no task needs to run for an extended period. A hardware timer is programmed to fire at the next task's wake time, allowing the CPU to enter deep sleep until needed.

```
FreeRTOS tickless idle mechanism:

  portSUPPRESS_TICKS_AND_SLEEP(expectedIdleTicks):
    1. Calculate sleep duration: expectedIdleTicks * portTICK_PERIOD_MS.
    2. Program LPTIM (low-power timer) to fire after sleep_duration.
    3. Enter STOP mode (WFI with SLEEPDEEP set).
    4. CPU sleeps...
    5. LPTIM fires -> wakeup from STOP mode.
    6. Re-lock PLL (300 µs typical).
    7. Correct FreeRTOS tick count: xTaskIncrementTick(actual_elapsed_ticks).
    8. Resume scheduler.

  Caveat: during sleep, vTaskDelay() timers continue advancing because
  the LPTIM elapsed time is accounted for on wakeup. Tasks see correct
  time even though the CPU was asleep.
```

---

## Tier 1 — Fundamentals

### Question F1
**What is the difference between clock gating and power gating? Which reduces leakage current, and which reduces dynamic power?**

**Answer:**

**Clock gating** disables the clock signal to a logic block by inserting a gated clock cell (a clock enable AND gate) in the clock tree. When the clock is gated:
- Flip-flops no longer toggle — dynamic power (C * V^2 * f * alpha) drops to near zero for that block.
- The supply voltage remains present — leakage current continues to flow through all transistors.

**Power gating** removes the supply voltage from a block by opening a power switch (PMOS header or NMOS footer) between the supply rail and the block. When power is gated:
- Both dynamic power and leakage power drop to near zero.
- All register state is lost — the block must be reinitialised on power-up.
- Wakeup requires time for the supply to ramp, potentially hundreds of microseconds.

**Summary:**

| Mechanism | Reduces dynamic power | Reduces leakage | State preserved | Wakeup latency |
|---|---|---|---|---|
| Clock gating | Yes | No | Yes | Zero (1 clock cycle) |
| Power gating | Yes | Yes | No | Tens to hundreds of µs |

**Design implication:** Clock gating is used for blocks that are idle frequently but need to wake instantly (e.g., a timer that is temporarily disabled). Power gating is used for blocks that can tolerate reinitialisation and are off for extended periods (e.g., a cellular modem that is powered down between transmit windows).

**Common mistake:** Candidates say "turning off the peripheral clock saves both dynamic and static power". Clock gating saves only dynamic power. On advanced process nodes (28 nm and below), leakage dominates the idle power budget and clock gating alone is insufficient.

---

### Question F2
**Explain the difference between `__WFI()` and `__WFE()` on ARM Cortex-M. When would you choose one over the other?**

**Answer:**

`__WFI()` (Wait For Interrupt) puts the CPU into sleep mode and waits until an interrupt request is pending. The CPU wakes when any enabled interrupt fires (including the SysTick interrupt if it is enabled).

`__WFE()` (Wait For Event) puts the CPU into sleep mode and wakes on either an interrupt or an **event**, which includes:
- A `SEV` (Send Event) instruction executed on another core (relevant in multi-core systems like Cortex-M33 with Cortex-M0 companion core).
- An external event pin.
- In uniprocessor systems, WFE also wakes on any exception, similar to WFI.

**Practical difference in uniprocessor embedded systems:**

On most single-core Cortex-M MCUs (M4, M33, M7), WFI and WFE behave identically in terms of power — both enter the same sleep state. The distinction matters in:

1. **Multiprocessor spin-wait (SEV/WFE locking):** In a dual-core system (e.g., nRF5340 with Cortex-M33 + Cortex-M33), one core can execute `SEV` to wake the other core from `WFE`. This is the ARM recommended mechanism for inter-processor notification without polling.

2. **Event register:** ARM Cortex-M has an internal event register. The first `WFE` after an event clears the register and returns immediately without sleeping. The second `WFE` (with no new event) sleeps. This is useful for lock-free synchronisation: if a mutex unlock sends `SEV` before the waiting core executes `WFE`, the waiting core will not miss the event.

**Recommendation:** Use `__WFI()` for straightforward single-core low-power idle. Use `__WFE()` only when implementing multi-core synchronisation or when the SEV event mechanism is specifically needed.

---

### Question F3
**A microcontroller is battery-powered and spends most of its time in STOP mode. What must the firmware do before entering STOP mode to minimise power consumption?**

**Answer:**

Entering STOP mode blindly without preparing the system leads to wasted power from peripherals that continue running unnecessarily. The preparation sequence:

**1. Disable or suspend all peripherals that are not needed during sleep.**
```c
/* Example: Disable UART if not configured as a wakeup source */
HAL_UART_DeInit(&huart2);
__HAL_RCC_USART2_CLK_DISABLE();

/* Disable SPI */
HAL_SPI_DeInit(&hspi1);
__HAL_RCC_SPI1_CLK_DISABLE();
```

**2. Configure GPIO pins to minimise leakage.**

Floating input pins are a major source of unexpected current draw. A floating input sits at an intermediate voltage, causing both the N and P transistors of the input buffer to partially conduct — this can add 50–200 µA per floating pin.

```c
/* Set unused GPIO pins to analog input mode.
 * In analog mode, the Schmitt trigger buffer is disconnected,
 * eliminating the leakage path entirely. */
GPIO_InitTypeDef gpio = {
    .Pin  = GPIO_PIN_All,
    .Mode = GPIO_MODE_ANALOG,
    .Pull = GPIO_NOPULL,
};
HAL_GPIO_Init(GPIOB, &gpio);   /* GPIOB pins not used during sleep */
```

**3. Disable the high-frequency oscillator and PLL.**

In STOP mode, the HSI and HSE clocks are automatically stopped by most MCUs. However, verify that the PLL is not being held on by a peripheral that still has PLL as its clock source. If a peripheral holds the PLL on, power consumption in "STOP" will be closer to run-mode.

**4. Configure the wakeup source before sleeping.**

At least one wakeup source must be enabled, or the device will sleep forever:
```c
/* Enable RTC periodic wakeup every 30 seconds */
HAL_RTCEx_SetWakeUpTimer_IT(&hrtc, 30, RTC_WAKEUPCLOCK_CK_SPRE_16BITS);

/* Enable GPIO interrupt on PA0 (button) as secondary wakeup */
HAL_NVIC_EnableIRQ(EXTI0_IRQn);
```

**5. Set the SLEEPDEEP bit and enter STOP.**
```c
/* Request STOP2 mode (lowest current STOP mode on STM32L4) */
HAL_PWREx_EnterSTOP2Mode(PWR_STOPENTRY_WFI);

/* ---- CPU is asleep here ---- */

/* After wakeup: restore system clock (PLL was off during STOP) */
SystemClock_Config();   /* re-locks PLL to configured frequency */
```

**Common mistake:** Forgetting to call `SystemClock_Config()` after wakeup from STOP mode. After STOP, the MCU resumes on the MSI (low-speed internal oscillator) rather than the PLL. Code running at 4 MHz instead of 80 MHz may work but at 20x lower performance, and timing-dependent drivers (UART baud rate, SPI clock) will be wrong.

---

## Tier 2 — Intermediate

### Question I1
**Perform a power budget analysis for a sensor node that samples an accelerometer once per second, transmits data over BLE every 10 seconds, and sleeps the rest of the time. Determine the average current and estimate battery life from a 240 mAh coin cell.**

**Answer:**

**System parameters:**

| State | Current | Duration per 10-second cycle |
|---|---|---|
| MCU active (accelerometer sample + processing) | 8 mA | 10 ms |
| Accelerometer operating (during MCU active) | 0.2 mA | 10 ms |
| BLE advertisement + data TX (once per 10 s) | 12 mA peak average | 15 ms |
| MCU in STOP mode (LPTIM running for wakeup) | 8 µA | ~9,975 ms |
| Accelerometer standby (during MCU sleep) | 2 µA | 9,975 ms |
| LDO regulator quiescent (always-on) | 30 µA | 10,000 ms |

**Charge consumed per 10-second cycle:**

```
State              Current   Duration    Charge (µAh)
-----------------  --------  ----------  ------------------
MCU active         8 mA      10 ms       8,000 µA * (10/3,600,000) h  = 0.0222 µAh
Accelerometer ON   0.2 mA    10 ms       200 µA   * (10/3,600,000) h  = 0.000556 µAh
BLE TX             12 mA     15 ms       12,000 µA * (15/3,600,000) h = 0.0500 µAh
MCU STOP           8 µA      9,975 ms    8 µA     * (9,975/3,600,000) = 0.02217 µAh
Accelerometer stby 2 µA      9,975 ms    2 µA     * (9,975/3,600,000) = 0.005542 µAh
LDO quiescent      30 µA     10,000 ms   30 µA    * (10,000/3,600,000)= 0.08333 µAh

Total per 10 s:                                    = 0.1837 µAh
```

**Average current:**

```
I_avg = Total_charge_per_cycle / cycle_period
      = 0.1837 µAh / (10 / 3600 h)
      = 0.1837 / 0.002778
      = 66.1 µA
```

**Battery life:**

```
Life = Battery_capacity / I_avg
     = 240 mAh / 0.0661 mA
     = 3,631 hours
     = 151 days
     ≈ 5 months
```

**Applying a 0.8 derating factor** (accounts for self-discharge, temperature effects, and end-of-life capacity reduction on CR2032):

```
Realistic life = 151 * 0.8 = 121 days ≈ 4 months
```

**Where to optimise for longer battery life:**

The three dominant contributors ranked by charge:
1. LDO quiescent: 0.083 µAh (45% of total). Replace with ultra-low-quiescent LDO (e.g., 0.8 µA IQ). Savings: ~0.074 µAh per cycle.
2. MCU active + BLE TX: 0.072 µAh combined (39%). Reduce BLE TX interval from every 10 s to every 60 s for a 6x reduction in BLE contribution.
3. MCU STOP leakage: 0.022 µAh (12%). MCU choice: nRF52840 in RAM retention mode draws ~2.5 µA vs 8 µA for a typical STM32. Savings: ~0.015 µAh.

Implementing all three improvements:
- New I_avg estimate: ~10–15 µA
- New life estimate: 240 / 0.012 = 20,000 h = 833 days = 2.3 years

---

### Question I2
**What is "tickless idle" in FreeRTOS and how does it interact with STOP mode on an STM32 microcontroller? Describe the implementation steps and the pitfalls.**

**Answer:**

**Standard vs. tickless operation:**

In standard FreeRTOS, `SysTick` fires every `configTICK_RATE_HZ` (e.g., 1000 Hz = 1 ms). Even with all tasks blocked, the scheduler wakes the CPU every millisecond to check for runnable tasks. With `__WFI()` in the idle hook, the CPU enters Sleep mode (not STOP mode) for at most 1 ms at a time, because the SysTick interrupt wakes it.

In **tickless idle**, FreeRTOS suppresses the SysTick when the next task wake time is more than one tick in the future:

```c
/* FreeRTOS tickless idle hook — called from the idle task */
/* portSUPPRESS_TICKS_AND_SLEEP is the hook FreeRTOS calls */

void vPortSuppressTicksAndSleep(TickType_t xExpectedIdleTime)
{
    uint32_t ulTimerCountsForOneTick;
    uint32_t ulReloadValue;
    uint32_t ulCompleteTickPeriods;
    TickType_t xModifiableIdleTime;

    /* Convert expected idle time from ticks to LPTIM counts.
     * LPTIM runs at 32768 Hz (LSE crystal). */
    ulTimerCountsForOneTick = (32768UL / configTICK_RATE_HZ);
    ulReloadValue = (ulTimerCountsForOneTick * xExpectedIdleTime) - 1;

    /* Clamp to LPTIM's 16-bit counter maximum */
    if (ulReloadValue > 0xFFFFUL) {
        ulReloadValue = 0xFFFFUL;
    }

    /* Stop SysTick to prevent it from waking us during STOP */
    HAL_SuspendTick();

    /* Program LPTIM to fire after xExpectedIdleTime ticks */
    HAL_LPTIM_Counter_Stop_IT(&hlptim1);
    HAL_LPTIM_Counter_Start_IT(&hlptim1, ulReloadValue);

    /* Enter STOP2 mode */
    HAL_PWREx_EnterSTOP2Mode(PWR_STOPENTRY_WFI);

    /* ---- CPU sleeps here ---- */

    /* Woke up: restore system clock (STOP2 switched to MSI) */
    SystemClock_Config();

    /* Calculate how long we actually slept */
    uint32_t ulCounts = HAL_LPTIM_ReadCounter(&hlptim1);
    ulCompleteTickPeriods = ulCounts / ulTimerCountsForOneTick;

    /* Correct the RTOS tick count */
    vTaskStepTick(ulCompleteTickPeriods);

    /* Re-enable SysTick */
    HAL_ResumeTick();
}
```

**Implementation pitfalls:**

1. **STOP mode disables the PLL and HSE.** The MCU resumes on the MSI oscillator (~4 MHz). `SystemClock_Config()` must be called immediately on wakeup, before any peripheral that relies on the configured system clock (UART baud rate, SPI clock) executes.

2. **SysTick must be stopped before entering STOP.** SysTick runs from the CPU clock, which stops in STOP mode. However, if SysTick fires between the `HAL_SuspendTick()` call and `WFI`, it will wake the device immediately. Disable interrupts briefly around the sleep entry to make this atomic.

3. **The LPTIM source must remain active in STOP.** The LPTIM must be clocked from LSE or LSI, not from the PLL. Verify that the LPTIM clock source is configured to LSE before enabling tickless idle.

4. **Tick count correction must account for partial ticks.** If the device wakes from an external interrupt before the LPTIM fires, `ulCounts` will be less than the full expected count. `vTaskStepTick()` must receive the number of complete ticks elapsed, not the total expected ticks.

5. **`configUSE_TICKLESS_IDLE` must be set to 2** (use custom `portSUPPRESS_TICKS_AND_SLEEP`) in `FreeRTOSConfig.h`, not 1 (which uses the built-in SysTick-based low-power mode that only enters Sleep, not STOP).

---

### Question I3
**A product specification requires a device to operate for 10 years on a single AA battery (2500 mAh). What maximum average current does this allow, and what design techniques achieve it?**

**Answer:**

**Maximum average current calculation:**

```
Battery life target: 10 years = 10 * 365.25 * 24 = 87,660 hours
Battery capacity:    2500 mAh
Derating factor:     0.75 (self-discharge ~2% per year = ~18% total over 10 years;
                     temperature derating for -20°C to +60°C operation: ~10%;
                     combined: keep 0.75 as conservative factor)
Effective capacity:  2500 * 0.75 = 1875 mAh

Maximum I_avg = 1875 mAh / 87,660 h = 21.4 µA
```

**This is an extremely tight budget.** Compare: a standard LED draws 10 mA — leaving it on for just 3 minutes per year would consume the entire budget.

**Design techniques to achieve sub-25 µA average current:**

**1. Ultra-low-leakage MCU selection.**

| MCU | Deep sleep current | RAM retention | Notes |
|---|---|---|---|
| nRF52840 (Nordic) | 2.5 µA | 256 KB | Includes BLE radio, system-off mode |
| ATSAML21 (Microchip) | 0.9 µA | 32 KB | Picosleep mode with I2C wakeup |
| STM32L4+ (STM32) | 8 µA | 128 KB | STOP2 with RTC |
| EFM32PG22 (Silicon Labs) | 1.1 µA | 32 KB | Energy Mode 2 with RTC |

Choose MCUs specifically rated for the target sleep current. A generic STM32F4 draws ~1 mA in its lowest stop mode — completely unsuitable.

**2. Eliminate always-on regulators or choose ultra-low-IQ regulators.**

Traditional LDOs have IQ of 50–200 µA. Ultra-low-IQ alternatives:
- TPS62840 buck converter: IQ = 60 nA.
- TLV713 LDO: IQ = 0.7 µA.
- MAX8881 LDO: IQ = 0.8 µA.

At 21.4 µA budget, a 50 µA LDO alone exceeds the entire budget.

**3. Power gating all peripherals during sleep.**

Use load switches (e.g., TPS22919) controlled by a GPIO to physically disconnect sensors, displays, and communication modules from power during sleep:

```c
/* Sensor power rail: controlled by GPIO PC5 (active-high enable) */
HAL_GPIO_WritePin(SENSOR_PWR_EN_GPIO, SENSOR_PWR_EN_PIN, GPIO_PIN_RESET);
/* Sensor now draws 0 µA from the MCU supply */
```

**4. Measure real current before optimising.**

Power profilers (Nordic PPK2, Otii Arc, Keysight N6705C) measure current at microsecond resolution. Optimise based on measurements, not estimates — real systems consistently have surprises (e.g., a pull-up resistor on an I2C line drawing 50 µA through a sensor whose address pin is being pulled low).

**5. Duty cycle aggressively.**

```
Budget allocation example for a 10-year sensor node (I_avg budget: 20 µA):

Component                   Sleep current  Active current  Duty cycle  Average
--------------------------  -------------  --------------  ----------  -------
MCU (EFM32PG22, EM2)        1.1 µA         2.5 mA          0.1%        3.6 µA
Sensor (wakeup, measure)    0 µA           200 µA          0.1%        0.2 µA
Crystal oscillator (TCXO)   0 µA           400 µA          0.1%        0.4 µA
RF transmit (LoRa, 1/day)   0 µA           30 mA peak      0.001%      0.3 µA
Regulator (IQ)              0.8 µA         0.8 µA          100%        0.8 µA
Pull-up resistors            --             --              100%        2.0 µA (target)

Total                                                                   7.3 µA
Margin to 20 µA budget:                                                 12.7 µA
```

**6. Eliminate unnecessary pull-up resistors.**

Every pull-up resistor in a system connected to a low-impedance output draws current continuously. A 100 kΩ pull-up to 3.3 V draws 33 µA — more than the entire MCU sleep budget. Use 1 MΩ or 10 MΩ pull-ups on lines that switch infrequently, and disable them via GPIO output-low when not needed.

---

## Tier 3 — Advanced

### Question A1
**A system enters STOP mode correctly but measures 500 µA average current instead of the expected 15 µA. Describe a systematic debug methodology to identify the cause.**

**Answer:**

500 µA in a mode that should draw 15 µA indicates a current source that should be off is still active. Work through the following layers systematically:

**Step 1: Isolate the supply domain.**

Use a current-measuring probe or ammeter with µA resolution (Nordic PPK2, Otii Arc) to observe the current waveform. Key observations:
- Is the current constant at 500 µA, or does it vary? Constant current suggests a resistive or quiescent path. Varying current suggests intermittent activity (a peripheral still running).
- Does the current profile show periodic spikes? This indicates an interrupt is waking the CPU periodically (tick timer not suppressed, or an enabled IRQ source).

**Step 2: Check for unsuppressed interrupts.**

```c
/* Add to your sleep entry code — verify SysTick is suspended */
/* Read the SysTick Control Register: bit 0 = ENABLE, bit 1 = TICKINT */
uint32_t systick_ctrl = SysTick->CTRL;
assert((systick_ctrl & SysTick_CTRL_ENABLE_Msk) == 0);   /* must be 0 in STOP */

/* Check NVIC: are any unexpected IRQs enabled? */
/* ISER0 through ISER7 contain enabled interrupt bits */
for (int i = 0; i < 8; i++) {
    if (NVIC->ISER[i] != 0) {
        LOG_DEBUG("NVIC ISER[%d] = 0x%08X -- unexpected IRQ enabled", i, NVIC->ISER[i]);
    }
}
```

If an IRQ is enabled and its peripheral is still generating requests, the CPU will wake from STOP immediately and re-enter run mode, making average current approach run-mode current.

**Step 3: Check clock tree.**

Verify the PLL and HSE are actually stopped in STOP mode:

```c
/* Before entering STOP, log the clock state */
LOG_DEBUG("RCC_CR = 0x%08X", RCC->CR);
/* Expected in STOP mode (STM32L4):
   Bit 24 (PLLON) = 0: PLL off
   Bit 16 (HSEON) = 0: HSE off
   Bit  8 (HSION) = 0: HSI off (or kept on if configured) */
```

If PLLON remains 1 in STOP mode, a peripheral is requesting the PLL clock (e.g., USB peripheral, SDIO). The MCU will not enter STOP until all clock requests are cleared.

**Step 4: Audit GPIO states.**

```c
/* Scan all GPIO ports for pins in input mode with no pull.
 * A floating input in analog mode is correct (0 µA).
 * A floating digital input causes Schmitt trigger oscillation. */

/* On STM32: check MODER register. Mode 11 = analog (correct).
 * Mode 00 = input with possible floating. */
for (GPIO_TypeDef *port : {GPIOA, GPIOB, GPIOC, GPIOD}) {
    uint32_t moder = port->MODER;
    for (int pin = 0; pin < 16; pin++) {
        uint32_t mode = (moder >> (pin * 2)) & 0x3;
        if (mode == 0x0) {   /* input mode — check if floating */
            uint32_t pupdr = (port->PUPDR >> (pin * 2)) & 0x3;
            if (pupdr == 0x0) {
                LOG_WARN("Floating input: port %p pin %d", port, pin);
            }
        }
    }
}
```

A single floating GPIO pin through a Schmitt trigger with a 500 kΩ effective input resistance at mid-rail voltage draws V/(2*R) = 3.3/(2*500,000) = 3.3 µA. With 32+ pins, this accumulates. But a pin connected to an external oscillating signal (e.g., an SPI bus line left active) through a 10 kΩ internal pull-up can draw 100–300 µA.

**Step 5: Check external components.**

Use a thermal camera or current-injecting probe to identify which external component is consuming current:
- External pull-up resistors on I2C/SPI lines.
- LEDs left on (common bug: GPIO set to output-high before sleep).
- External sensor in active mode (power gating not enabled).
- External oscillator / crystal being driven even though MCU clock is off.

**Step 6: Regression-test with a known baseline.**

Flash a minimal "sleep-only" firmware that initialises no peripherals and immediately enters STOP mode. If current is still 500 µA, the hardware design (external components, PCB leakage paths) is the source, not the firmware. If current drops to expected levels, re-enable peripherals one by one until the current spikes — that peripheral's driver is the culprit.

---

### Question A2
**Explain voltage scaling (DVFS — Dynamic Voltage and Frequency Scaling) in embedded systems. When is it beneficial, and what are the risks of incorrect implementation?**

**Answer:**

**DVFS concept:**

Dynamic power scales as P = C * V^2 * f. By reducing both voltage and frequency together, power reduces cubically:
- Halving frequency: 2x power reduction (linear).
- Halving voltage at the same time: additional 4x reduction.
- Combined: 8x power reduction at half the performance.

DVFS exploits this by running the CPU at the minimum voltage and frequency sufficient for the current workload.

**Embedded implementation example (STM32L4+):**

```
Voltage range   VDD_CORE   Max frequency   Relative active power
Range 1         1.2 V      80 MHz          100% (baseline)
Range 2         1.0 V      26 MHz          ~40%
Low-power range 1.0 V (LDO2) 2 MHz         ~15%

Workload-based scaling:
  Heavy computation task (FFT, encryption): Range 1 at 80 MHz.
  Idle polling loop (waiting for sensor): Range 2 at 16 MHz.
  Deep idle between events: STOP mode.
```

**Code example for voltage range switching on STM32L4:**

```c
void set_power_range(uint32_t range)
{
    /* range: PWR_REGULATOR_VOLTAGE_SCALE1 or SCALE2 */

    /* When reducing voltage (going to lower VDD), reduce clock first. */
    /* When increasing voltage (going to higher VDD), change voltage first. */

    if (range == PWR_REGULATOR_VOLTAGE_SCALE2) {
        /* Step 1: Reduce clock before reducing voltage */
        RCC_ClkInitTypeDef clk = {
            .ClockType = RCC_CLOCKTYPE_HCLK,
            .AHBCLKDivider = RCC_SYSCLK_DIV4   /* 80 MHz / 4 = 20 MHz */
        };
        HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_3);

        /* Step 2: Switch to voltage Range 2 */
        HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE2);
        while (!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY)) { }   /* wait for stable */
    } else {
        /* Going to Range 1: increase voltage first, then increase clock */
        HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1);
        while (!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY)) { }

        RCC_ClkInitTypeDef clk = {
            .ClockType = RCC_CLOCKTYPE_HCLK,
            .AHBCLKDivider = RCC_SYSCLK_DIV1   /* 80 MHz */
        };
        HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_4);
    }
}
```

**Risks of incorrect DVFS implementation:**

1. **Clock increased before voltage is stable.** If the CPU is clocked at 80 MHz while VDD_CORE is still at 1.0 V (below the 1.2 V required for 80 MHz), setup-time violations occur in the register file and pipeline. The result is silent data corruption — the CPU executes incorrect instructions without triggering any fault. **Always change voltage before increasing clock.**

2. **Flash wait-states not updated.** Flash memory access time is fixed (not voltage-scaled). At lower clock frequencies, fewer wait states are needed for correct reads. At higher frequencies, more wait states are required. If the application increases the CPU clock without increasing flash wait states, the CPU will fetch incorrect data from flash.

3. **Peripheral clocks break communication protocols.** UART, SPI, and I2C baud rates are derived from the APB bus clock (APB1, APB2). If the system clock is halved by DVFS without recalculating baud rate dividers, all serial communication will run at half the configured baud rate, causing framing errors.

4. **DVFS transitions add latency overhead.** The voltage regulator settling time (typically 50–200 µs) plus PLL re-lock time (100–300 µs) must be included in the transition budget. If the system needs to respond to an interrupt within 1 ms and DVFS transition takes 400 µs, the response deadline is missed.

5. **Incorrect thermal modelling.** At lower voltage, the maximum safe junction temperature may be lower (leakage increases with temperature, potentially causing latch-up at borderline operating conditions). Verify DVFS operating points are within the safe operating area from the datasheet.
