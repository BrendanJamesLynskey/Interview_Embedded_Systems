# Problem 02: Power Budget for a Battery-Powered IoT Device

## Problem Statement

You are the firmware architect for a wireless environmental sensor node. The product specification requires:

| Parameter | Value |
|---|---|
| Measurements | Temperature (±0.2°C), Humidity (±2% RH), CO2 (±50 ppm) |
| Sample interval | Every 60 seconds |
| Data transmission | BLE advertisement packet, once per sample |
| Firmware update | OTA over BLE, triggered by user (infrequent) |
| Battery | Single CR2032 coin cell (nominal 235 mAh at 2.7 V discharge) |
| Target battery life | Minimum 18 months continuous operation |
| Operating temperature | -20°C to +60°C |
| Enclosure | Sealed plastic; no user-accessible power switch |
| MCU | Nordic nRF52840 (Cortex-M4 at up to 64 MHz) |
| BLE stack | SoftDevice S140 |

**Tasks:**

1. Calculate the maximum average current allowed to meet the 18-month battery life target.
2. Construct a detailed power budget for one 60-second operating cycle, listing every active component and its contribution.
3. Identify the dominant current consumers and propose optimisations to extend battery life.
4. Analyse the impact of the -20°C operating temperature on battery capacity and revise the budget accordingly.
5. Specify the sleep mode configuration required to achieve the target current.

---

## Design Requirements

- The firmware must achieve the calculated average current target.
- All three sensors must be read every 60 seconds; no sensor may be skipped to save power.
- The BLE advertisement must be receivable by a standard smartphone at a distance of 10 metres.

---

## Solution

### Step 1: Maximum Average Current Calculation

**Battery parameters:**

```
Nominal capacity:    235 mAh (at 2.7 V, 0.2 mA discharge rate, 20°C)
Derating factors:
  Self-discharge:    CR2032 self-discharge ~1% per year.
                     Over 18 months: ~1.5% capacity lost.
  Temperature:       At -20°C, CR2032 capacity falls to ~70-75% of 20°C value.
                     Use worst-case: 70%.
  End-of-life:       BLE radio requires minimum 2.0 V to operate.
                     CR2032 delivers useful capacity down to ~2.5 V before
                     the voltage curve drops steeply. Use 80% utilisation.
  Combined derating: 0.985 * 0.70 * 0.80 = 0.551

Effective capacity at -20°C, 18 months: 235 * 0.551 = 129.4 mAh
```

**Maximum average current:**

```
Target life = 18 months = 18 * 30.4375 * 24 = 13,149 hours

I_avg_max = Effective_capacity / Target_life
          = 129.4 mAh / 13,149 h
          = 9.84 µA

Round down to allow margin: target I_avg = 9 µA
```

**This is an extremely tight budget of 9 µA.** For context:
- The nRF52840's system-off current (no RAM retention) is 0.4 µA.
- The nRF52840's system-on + RAM retention current is 1.5–3.0 µA.
- A single 100 kΩ pull-up resistor to 3.3 V draws 33 µA — three times the entire budget.

---

### Step 2: Power Budget for One 60-Second Cycle

**Sensors and their characteristics:**

| Sensor | Example part | Active current | Measurement time | Standby/off current |
|---|---|---|---|---|
| Temperature + Humidity | SHT4x (Sensirion) | 0.4 mA | 10 ms (high-precision mode) | ~0.1 µA (idle after measurement) |
| CO2 | SCD41 (Sensirion) | 13 mA average during measurement | 5000 ms (single-shot mode) | 0.1 µA |
| CO2 start-up time | SCD41 | 1 mA | 1000 ms (sensor warm-up) | — |

Note: The SCD41 in periodic measurement mode draws ~13 mA continuously. In single-shot mode, it wakes, measures, and powers down. The 5000 ms measurement window includes the on-chip NDIR optical sensing cycle.

**Operating sequence for one 60-second cycle:**

```
Timeline (not to scale):

t=0 ms:    MCU wakes from System-On low-power mode (RAM retention, RTC running)
           MCU transitions from 0.002 mA (sleep) to active (1.5-4 mA at 16 MHz)
t=0 ms:    Power gate to sensor VDD rail enabled (GPIO output high)
           Sensor power-on settling: 1 ms

t=1 ms:    I2C bus enabled. Send SHT4x measurement command.
t=11 ms:   Read SHT4x result (10 ms measurement complete).
           Compute temperature and humidity values.

t=12 ms:   Send SCD41 wake-up command (from periodic idle).
           SCD41 warm-up: 1000 ms

t=1012 ms: Send SCD41 single-shot measurement trigger.
           SCD41 measures: 5000 ms

t=6012 ms: Read SCD41 CO2, temperature, humidity.
           Power gate to sensor VDD rail disabled (GPIO output low).
           SCD41 draws 0 µA from this point.

t=6020 ms: MCU prepares BLE advertisement payload (data encoding, ~1 ms).

t=6021 ms: BLE radio TX:
           - SoftDevice prepares advertisement packet.
           - Radio powers up (~1 ms).
           - 3 advertisement events on channels 37, 38, 39.
           - Each event: 0.5 ms TX at ~4.6 mA (0 dBm TX power).
           - Inter-event gaps: radio on standby at 0.5 mA for ~5 ms each.
           Total radio active window: ~25 ms.

t=6046 ms: BLE radio powers down. MCU processing done.
t=6050 ms: MCU enters System-On low-power mode.
           TIMER0 set to fire at t=60,000 ms.

t=6050 ms to 60,000 ms: MCU in low-power mode (RTC running, TIMER0 armed).
           Duration: ~53,950 ms.
```

**Current by phase:**

| Phase | Current | Duration | Charge (µAh) |
|---|---|---|---|
| MCU active at 16 MHz (full cycle init + processing) | 3.0 mA | 50 ms | 3000 * (50/3,600,000) = 0.0417 µAh |
| SHT4x measurement | 0.4 mA | 10 ms | 400 * (10/3,600,000) = 0.00111 µAh |
| SCD41 warm-up | 1.0 mA | 1000 ms | 1000 * (1000/3,600,000) = 0.2778 µAh |
| SCD41 measurement | 13.0 mA | 5000 ms | 13000 * (5000/3,600,000) = 18.056 µAh |
| I2C bus active (pull-ups: 2 x 4.7 kΩ to 3.3 V) | 1.40 mA | 6012 ms | 1400 * (6012/3,600,000) = 2.338 µAh |
| BLE radio active (3 adv events + standby between) | 4.0 mA avg | 25 ms | 4000 * (25/3,600,000) = 0.02778 µAh |
| MCU + SoftDevice processing BLE | 4.0 mA | 25 ms | 0.02778 µAh |
| MCU sleep (System-On, RAM retention, RTC, TIMER0) | 0.003 mA | 53,950 ms | 3 * (53950/3,600,000) = 0.04496 µAh |
| Voltage regulator (if external; assume internal DCDC) | 0 extra | — | (nRF52840 internal DCDC; no external regulator IQ) |
| **Total per 60-second cycle** | | | **20.809 µAh** |

**Average current from this budget:**

```
I_avg = Total_charge_per_cycle / cycle_period
      = 20.809 µAh / (60 / 3600 h)
      = 20.809 / 0.01667
      = 1248 µA = 1.248 mA
```

**This is 139x over budget.** The target is 9 µA; the initial design gives 1248 µA.

---

### Step 3: Dominant Consumers and Optimisations

**Ranked by contribution:**

| Component | Charge per cycle | Fraction of total |
|---|---|---|
| SCD41 measurement (13 mA, 5 s) | 18.056 µAh | **87%** |
| I2C pull-up resistors (during sensor phase) | 2.338 µAh | 11% |
| SCD41 warm-up | 0.278 µAh | 1.4% |
| MCU active processing | 0.042 µAh | 0.2% |
| All others | 0.095 µAh | 0.5% |

The CO2 sensor dominates. Three optimisations are required:

**Optimisation 1: Reduce CO2 measurement frequency.**

CO2 concentration in a stationary environmental monitor changes slowly. Measuring every 60 seconds is excessive. Measure CO2 every 10 minutes, temperature/humidity every 60 seconds.

```
New CO2 contribution:
  Per 60-second cycle: 18.056 µAh * (1/10) = 1.806 µAh per cycle
  (CO2 only runs every 10th cycle; averaged across all cycles)
```

**Optimisation 2: Replace I2C pull-up resistors with value 100 kΩ instead of 4.7 kΩ.**

At 100 kΩ, I2C pull-up current = 3.3 V / 100 kΩ = 33 µA per line. Two lines = 66 µA.
This reduces the I2C phase current from 1400 µA to 66 µA.

Note: 100 kΩ limits I2C speed to ~100 kHz (standard mode). For the SHT4x and SCD41, this is acceptable.

```
New I2C pull-up contribution (6 s at 66 µA):
  66 µA * (6000/3,600,000) = 0.110 µAh per cycle (down from 2.338 µAh)
```

**Optimisation 3: Power gate the sensor VDD rail completely between measurements.**

Use a load switch (e.g., TPS22919, IQ = 1 µA) to remove power from both sensors between measurements. Sensors draw 0 µA (not 0.1 µA) when their supply is disconnected.

At 0.1 µA per sensor over 53.95 seconds: 0.2 µA * (53.95/3600) = 0.003 µAh — negligible, but adds up over many devices.

**Revised power budget after optimisations:**

| Phase | Current | Duration | Charge (µAh) |
|---|---|---|---|
| SCD41 measurement (every 10 min) | 13.0 mA | 5000 ms, 1 of 10 cycles | 18.056 / 10 = 1.806 µAh |
| SCD41 warm-up (every 10 min) | 1.0 mA | 1000 ms, 1 of 10 cycles | 0.2778 / 10 = 0.0278 µAh |
| SHT4x measurement (every cycle) | 0.4 mA | 10 ms | 0.00111 µAh |
| I2C pull-ups at 100 kΩ | 0.066 mA | 100 ms (sensor comms only) | 0.00183 µAh |
| MCU active | 3.0 mA | 15 ms (reduced — no CO2 on 9 of 10 cycles avg) | avg 3.0*(15 + 90*1/10)/3.6M = 0.0125 µAh |
| BLE radio (3 advertisements) | 4.0 mA avg | 25 ms | 0.02778 µAh |
| MCU sleep (59.87 s, 0.003 mA) | 0.003 mA | 59,870 ms | 0.04989 µAh |
| Load switch IQ | 0.001 mA | 60,000 ms | 0.01667 µAh |
| **Total per 60-second cycle** | | | **1.942 µAh** |

**Revised average current:**

```
I_avg = 1.942 µAh / (60/3600 h) = 1.942 / 0.01667 = 116.5 µA
```

Still 13x over budget. The SCD41 dominates even at 1/10th frequency.

**Optimisation 4: Further reduce CO2 frequency to once per hour, or use a different CO2 sensor.**

The SCD41 is a high-accuracy NDIR sensor. For a lower-power application, alternatives exist:

| Sensor | Technology | Active current | Measurement time | Accuracy |
|---|---|---|---|---|
| SCD41 | NDIR | 13 mA | 5 s | ±40 ppm |
| CM1106 (Cubic) | NDIR | 60 mA | 2 s | ±50 ppm |
| STC31 (Sensirion) | Thermal conductivity | 0.5 mA | 66 ms | ±(0.5% + 200 ppm) |
| ENS160 (ScioSense) | Metal oxide | 1.5 mA avg | Continuous | ±10% relative |

For a 9 µA average budget, **no accurate CO2 sensor is compatible with 60-second measurement intervals using current technology**. The only path to the 9 µA budget is to:

1. Reduce CO2 measurement to once per 4 hours or once per day (if the application permits).
2. Accept lower CO2 accuracy (metal oxide sensors like ENS160 are less accurate but far lower power).
3. Increase the battery size (switch from CR2032 to AA/AAA or LiSOCl2 battery).

**Revised budget with hourly CO2 (every 60 cycles):**

```
SCD41 amortised charge per 60-second cycle:
  18.056 µAh + 0.2778 µAh = 18.334 µAh per CO2 measurement
  Amortised over 60 cycles: 18.334 / 60 = 0.3056 µAh per cycle

All other charges per cycle: 0.1077 µAh

Total: 0.413 µAh per 60-second cycle
I_avg = 0.413 / 0.01667 = 24.8 µA
```

With hourly CO2 measurements: I_avg = **24.8 µA** — still 2.75x over the 9 µA target.

**Reaching 9 µA requires either a different CO2 sensing technology or a different battery chemistry:**

```
LiSOCl2 D-size battery: 19,000 mAh capacity.
  At 24.8 µA average: 19,000 / 0.0248 = 766,129 hours = 87 years.
  This comfortably meets a 10-year product life.

For a coin-cell form factor with the SCD41: the 9 µA / 18-month target
  is not achievable. The physical constraint of CO2 measurement energy
  requirements exceeds the available battery energy budget.
```

---

### Step 4: Temperature Impact Analysis

**CR2032 capacity vs temperature:**

```
Temperature   Capacity (% of rated)   Effective capacity (235 mAh nominal)
  +60°C            105%                    247 mAh
  +20°C            100%                    235 mAh  (rated condition)
    0°C             90%                    211 mAh
  -20°C             70%                    164 mAh
  -40°C             40%                     94 mAh  (below operating spec)
```

At -20°C (the product's minimum rated temperature), effective capacity is 164 mAh before self-discharge derating (vs 235 mAh at 20°C).

**Additional cold-temperature consideration — internal resistance:**

At -20°C, CR2032 internal resistance increases from ~10 Ω (at 20°C) to approximately 50–100 Ω. During the BLE radio transmission (peak ~15 mA for SoftDevice), the voltage drop across the internal resistance is:

```
Peak current during BLE TX: 15 mA
Internal resistance at -20°C: 80 Ω (estimate)
Voltage drop: 15 mA * 80 Ω = 1.2 V

If battery open-circuit voltage is 2.8 V at -20°C (near end of useful life):
  Terminal voltage during TX: 2.8 V - 1.2 V = 1.6 V

nRF52840 minimum operating voltage: 1.7 V.
The battery cannot power the BLE radio at -20°C when below ~50% charge.
```

**Mitigation for cold-temperature voltage drop:**

1. Add a 100 µF capacitor in parallel with the battery. The capacitor supplies peak current during BLE TX, reducing the instantaneous draw from the battery. The battery slowly recharges the capacitor during sleep.

2. Use a boost converter to maintain 3.3 V from the battery voltage as it drops. The nRF52840 internal DCDC converter steps down from battery voltage but cannot boost. An external boost (e.g., TPS61099, IQ = 300 nA) adds 0.3 µA to quiescent current but allows operation down to 0.7 V battery input.

**Revised effective capacity for budget:**

```
Temperature: -20°C
Rated capacity at -20°C: 164 mAh
Self-discharge (18 months at ~1.5%): 164 * 0.985 = 161.5 mAh
End-of-life voltage cut-off (boost converter, use 90% of capacity): 161.5 * 0.90 = 145.4 mAh

I_avg_max = 145.4 mAh / 13,149 h = 11.06 µA
```

With the boost converter and cold temperature derating, the revised budget is **11 µA**.

---

### Step 5: Sleep Mode Configuration

**nRF52840 power modes used:**

```
Mode                    Current    Used for
----------------------  ---------  ------------------------------------------
System-on (RAM off)     ~0.5 µA    Theoretical minimum; cannot retain application state
System-on (RAM retain)  ~2.0 µA    Main sleep mode: all RAM retained, RTC running
                                   TIMER0 armed for next wakeup
                                   No peripheral clocks except RTC and TIMER
System-off              ~0.4 µA    Not used (too long to wake for 60-second interval)
```

**Configuration for 2.0 µA system-on sleep:**

```c
/* Configure nRF52840 for minimum current in System-On sleep */

void configure_sleep(void)
{
    /* 1. Enable internal DC-DC converter.
     *    DCDC is ~80% efficient vs ~50% for LDO at low current.
     *    Saves ~1 µA compared to LDO at 2 µA total draw. */
    NRF_POWER->DCDCEN = POWER_DCDCEN_DCDCEN_Enabled;

    /* 2. Configure RAM retention: retain only the banks needed.
     *    nRF52840 has 9 RAM sections (0-8).
     *    Only retain sections used by the application + SoftDevice.
     *    Unused sections: set to OFF (saves ~0.1 µA per unused section). */
    NRF_POWER->RAM[0].POWER = 0x0000FFFFUL;   /* Section 0: ON (used by SD) */
    NRF_POWER->RAM[1].POWER = 0x0000FFFFUL;   /* Section 1: ON (application) */
    /* Sections 2-8: OFF if not needed */
    for (int i = 2; i <= 8; i++) {
        NRF_POWER->RAM[i].POWER = 0x00000000UL;   /* OFF */
    }

    /* 3. Disable all unused peripheral clocks.
     *    Any peripheral with its clock enabled draws quiescent current
     *    even when not active. */
    /* Disable UART (not used during sleep; only during debug) */
    NRF_UART0->ENABLE = UART_ENABLE_ENABLE_Disabled;
    NRF_USBD->ENABLE  = USB_ENABLE_ENABLE_Disabled;

    /* 4. Configure all unused GPIO pins to input with no pull.
     *    In sleep, set them to sense disabled and no drive. */
    /* Sensor power gate pin: output LOW (sensors off) */
    nrf_gpio_cfg_output(SENSOR_PWR_EN_PIN);
    nrf_gpio_pin_clear(SENSOR_PWR_EN_PIN);

    /* Floating input pins: configure as disconnected */
    nrf_gpio_cfg(UNUSED_PIN,
                 NRF_GPIO_PIN_DIR_INPUT,
                 NRF_GPIO_PIN_INPUT_DISCONNECT,   /* disconnects input buffer */
                 NRF_GPIO_PIN_NOPULL,
                 NRF_GPIO_PIN_S0S1,
                 NRF_GPIO_PIN_NOSENSE);

    /* 5. Use RTC1 (32.768 kHz LFXO) as wakeup source.
     *    RTC1 draws ~1.5 µA when running on LFXO crystal.
     *    Using LFRC (RC oscillator) reduces to ~0.5 µA but adds ±2% timing error. */
    /* (Configured separately by the application timer subsystem) */
}

/* Enter sleep until next scheduled wakeup */
void enter_sleep_until(uint32_t wakeup_ticks_from_now)
{
    /* Program RTC compare register */
    uint32_t current_ticks = nrf_rtc_counter_get(NRF_RTC1);
    nrf_rtc_cc_set(NRF_RTC1, 0, current_ticks + wakeup_ticks_from_now);
    nrf_rtc_event_clear(NRF_RTC1, NRF_RTC_EVENT_COMPARE_0);
    nrf_rtc_int_enable(NRF_RTC1, NRF_RTC_INT_COMPARE0_MASK);

    /* Enter WFE-based sleep. SoftDevice must be notified of sleep entry
     * via sd_app_evt_wait() to allow the SoftDevice to also sleep. */
    sd_app_evt_wait();   /* blocks until RTC interrupt fires */
}
```

---

## Summary and Recommendations

**Final power budget (optimised, hourly CO2 measurement):**

| Component | Average current contribution |
|---|---|
| MCU sleep (2.0 µA system-on) | 1.94 µA (97.3% of cycle in sleep) |
| SHT4x amortised | 0.007 µA |
| SCD41 amortised (hourly) | 8.49 µA |
| BLE advertisement amortised | 0.77 µA |
| MCU active processing amortised | 0.33 µA |
| I2C pull-ups amortised | 0.04 µA |
| Load switch IQ | 0.017 µA |
| **Total** | **11.6 µA** |

**Against the 11 µA revised budget (with boost converter at -20°C): marginal.**

**Recommendations to the product team:**

1. The 9 µA / 18-month target with a CR2032 and accurate CO2 sensing every 60 seconds is not physically achievable. This is a fundamental energy constraint, not a firmware limitation.

2. Recommended alternatives (in order of product impact):
   - Switch to a LiSOCl2 "baby" battery (LS14250, 1/2 AA form factor): 1200 mAh capacity, enables hourly CO2 and still achieves >5 years battery life.
   - Reduce CO2 to daily measurement (suitable for ambient air quality monitoring, not real-time hazard detection).
   - Relax temperature requirement to 0°C–+60°C, which increases effective CR2032 capacity to 211 mAh and allows a more aggressive power budget.

3. Measure real current on prototype hardware before finalising the battery selection. Simulated budgets consistently diverge from reality by ±30% due to undocumented quiescent currents, timing variations, and peripheral startup transients.
