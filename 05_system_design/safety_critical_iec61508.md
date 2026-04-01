# Safety-Critical Design and IEC 61508

## Prerequisites
- Basic software engineering lifecycle concepts
- Embedded C programming fundamentals
- Understanding of hardware faults: stuck-at, transient, systematic
- RTOS task management and watchdog timers

---

## Concept Reference

### IEC 61508: Functional Safety of Electrical/Electronic/Programmable Electronic Systems

IEC 61508 is the foundational international standard for functional safety. It defines a framework for developing systems whose malfunction could cause injury or death. It is a **generic standard** — sector-specific standards derive from it:

| Sector | Derived Standard | Application |
|---|---|---|
| Automotive | ISO 26262 | Road vehicle E/E systems |
| Machinery | IEC 62061, ISO 13849 | Industrial machinery |
| Process industry | IEC 61511 | Chemical plants, oil/gas |
| Railway | EN 50128, EN 50129 | Signalling, rolling stock |
| Medical | IEC 62304 | Medical device software |
| Nuclear | IEC 61513 | Nuclear instrumentation |

IEC 61508 covers the full development lifecycle from hazard analysis through to decommissioning. For software engineers, Part 3 (software requirements) is most directly relevant.

### Safety Integrity Levels (SIL)

SIL defines the required probability of failure for a safety function:

```
SIL  Probability of Failure on Demand (PFD)  Probability of Failure per Hour (PFH)
     [Low Demand Mode]                        [High Demand / Continuous Mode]
---  ----------------------------------------  --------------------------------------
SIL 1  >= 10^-2 to < 10^-1 (1 in 10 to 1 in 100)    >= 10^-6 to < 10^-5
SIL 2  >= 10^-3 to < 10^-2 (1 in 100 to 1 in 1000)  >= 10^-7 to < 10^-6
SIL 3  >= 10^-4 to < 10^-3                            >= 10^-8 to < 10^-7
SIL 4  >= 10^-5 to < 10^-4 (1 in 10,000 to 1 in 100,000) >= 10^-9 to < 10^-8

Low demand mode: safety function activated < once per year (e.g., emergency stop button).
High demand / continuous mode: safety function active continuously (e.g., ABS braking).
```

**Intuition for SIL 2 PFH:** SIL 2 continuous mode allows at most one dangerous failure per 10 million operating hours — roughly one failure per 1140 years of continuous operation. For a fleet of 10,000 devices each operating 8,760 hours per year, the maximum allowable dangerous failure rate is one failure per year across the entire fleet.

### The Safety Lifecycle

IEC 61508 mandates a specific lifecycle with defined phases and verification gates:

```
1. Concept
2. Overall Scope Definition
3. Hazard and Risk Analysis (HAZOP, FMEA, FTA)  <-- determines required SIL
4. Overall Safety Requirements
5. Safety Requirements Allocation (hardware vs. software)
6. E/E/PE System Design
   +-- 6a. Hardware design and development
   +-- 6b. Software design and development
              |
              +-- Software Safety Requirements Specification
              +-- Software Architecture Design
              +-- Software Unit Design (detailed design)
              +-- Software Unit Testing
              +-- Software Integration Testing
              +-- Software Validation
7. Overall Installation and Commissioning
8. Overall Safety Validation
9. Overall Operation, Maintenance, and Repair
10. Decommissioning
```

The key output of Phase 3 is the **Safety Requirements Specification (SRS)** which documents the required SIL for each safety function and flows into both hardware and software design.

### Fault Classification

IEC 61508 distinguishes types of faults to determine which mitigation technique applies:

| Fault type | Description | Mitigation |
|---|---|---|
| Random hardware failure | Physical degradation: electromigration, dielectric breakdown, cosmic ray bit-flip | Redundancy, diagnostics, proof testing |
| Systematic failure | Deterministic faults from design errors or incorrect specification | Process: requirements reviews, formal methods, independent verification |
| Common cause failure (CCF) | Single event causes failure in multiple redundant channels simultaneously | Physical separation, diverse technology, beta factor analysis |
| Transient fault | Temporary error: EMI-induced bit-flip, power glitch, radiation | ECC, CRC, periodic state refresh, watchdog |

**Safety software primarily addresses systematic failures** — bugs, logic errors, and specification mistakes. Hardware random failures are addressed by hardware redundancy and diagnostic coverage. The partition between hardware and software responsibility is determined during hazard analysis.

### MISRA C: Safety-Critical C Coding Standard

MISRA C (Motor Industry Software Reliability Association) provides coding guidelines that eliminate C language constructs known to introduce defects in safety-critical software. The 2012 edition (MISRA C:2012) defines:

- **22 mandatory rules:** Must never be violated.
- **171 required rules:** Must be followed unless a documented deviation is approved.
- **17 advisory guidelines:** Recommended best practice.

Key rule categories and rationale:

```
Category: Language Constructs
  Rule 14.4: The controlling expression of an if-statement must be essentially Boolean.
  Rationale: Prevents accidental assignment in if-condition (e.g., "if (x = 5)" instead
             of "if (x == 5)"), which is valid C but almost never the intent.

  Rule 10.3: A value of essential type category shall not be assigned to a variable of
             a different essential type category.
  Rationale: Prevents silent implicit conversions that may lose precision or sign.

Category: Pointers
  Rule 11.2: Conversions shall not be performed between a pointer to an incomplete type
             and any other type.
  Rule 11.5: A conversion shall not be performed from pointer to void into pointer to
             object.
  Rationale: Pointer type confusion is a leading source of memory safety bugs.

Category: Dynamic Memory
  Rule 21.3: The memory allocation and deallocation functions of <stdlib.h> shall not
             be used.
  Rationale: malloc/free can fail unpredictably and produce heap fragmentation,
             leading to non-deterministic timing and hard-to-reproduce failures.

Category: Functions
  Rule 17.2: Functions shall not call themselves, either directly or indirectly.
  Rationale: Recursion makes stack depth non-deterministic; stack overflow is
             catastrophic in safety-critical systems.

  Rule 15.5: A function should have a single point of exit at the end.
  Advisory — but critical for testability and control flow traceability.
```

**Enforcement:** MISRA C compliance is verified by static analysis tools:
- PC-lint Plus (Gimpel Software)
- Polyspace Code Prover (MathWorks)
- LDRA Testbed
- Klocwork (Perforce)
- Helix QAC (Perforce)

---

## Tier 1 — Fundamentals

### Question F1
**What is the difference between a safety function and a safety integrity level (SIL)? Give a concrete example of each.**

**Answer:**

A **safety function** is a specific action that a system must perform to maintain or achieve a safe state when a hazardous condition occurs. It is defined in functional terms, not implementation terms.

Examples of safety functions:
- "When the operator presses the emergency stop button, de-energise the motor drive within 250 ms."
- "If the measured temperature exceeds 95°C, open the cooling valve and log a fault."
- "If the vehicle speed exceeds 200 km/h with the doors unlocked, lock the doors."

A **Safety Integrity Level (SIL)** is the required level of reliability (probability of failure) assigned to a safety function based on the risk analysis. It is a number from 1 to 4 where higher numbers mean lower permissible failure probability.

The SIL is determined by a **risk assessment**, not by the engineer's preference. The assessment considers:
- **Severity of consequence:** What happens if the safety function fails? Minor injury? Fatality?
- **Frequency of exposure:** How often is a person exposed to the hazard?
- **Probability of avoiding the hazard:** If the safety function fails, can the person escape?

**Concrete example:**

Safety function: "Emergency stop relay must open within 500 ms of button press."
- Consequence if it fails: unguarded rotating machinery; fatality possible.
- Exposure: operator works adjacent to machinery 4 hours/day.
- Avoidability: low — operator cannot reliably detect relay failure.
- **SIL assigned: SIL 2** by risk graph analysis.

The implementation (hardware, software, proof testing schedule) must then achieve a PFD of at most 10^-2 (1 failure per 100 demands) for this specific safety function.

**Common mistake:** Candidates say "this system is SIL 2" as if SIL is a property of the entire system. In IEC 61508, SIL is assigned per safety function. A single system may have multiple safety functions with different SIL requirements.

---

### Question F2
**Why does IEC 61508 prohibit dynamic memory allocation (malloc/free) in high-SIL software? What alternatives does it recommend?**

**Answer:**

IEC 61508 Part 3 lists dynamic memory management as a technique that is **not recommended** for SIL 2 and **prohibited** for SIL 3/4. MISRA C Rule 21.3 enforces this at the coding level.

**Reasons for prohibition:**

1. **Non-deterministic failure.** `malloc()` can fail at runtime and return NULL, even when the system was working correctly moments before. In a deterministic safety function, a runtime allocation failure is unacceptable — the system must either always succeed or fail in a predictable, detectable manner.

2. **Non-deterministic timing.** The execution time of `malloc()` depends on the current state of the heap (fragmentation, free list length). This makes worst-case execution time (WCET) analysis impossible, which is required for timing-critical safety functions.

3. **Fragmentation.** Long-running systems that repeatedly allocate and free memory of varying sizes develop heap fragmentation. A fragmentation failure may not occur during testing but manifests after months of operation in the field — after the product is certified and deployed.

4. **Corruption impact.** A heap corruption bug (double-free, use-after-free, buffer overflow on a heap object) can corrupt the allocator's metadata, causing subsequent allocations to return incorrect addresses and potentially overwrite any data structure in the system.

**Alternatives recommended by IEC 61508:**

1. **Static allocation:** Declare all data structures with fixed size at compile time. The linker and startup code guarantee initialisation. WCET is deterministic. No fragmentation. This is the preferred approach.

```c
/* Bad: dynamic allocation */
sensor_data_t *buf = malloc(NUM_SENSORS * sizeof(sensor_data_t));
if (buf == NULL) { /* handle error — but when? */ }

/* Good: static allocation */
static sensor_data_t sensor_buf[NUM_SENSORS];   /* known size at compile time */
```

2. **Stack allocation with bounded depth.** Local variables are stack-allocated and deterministic in lifetime. Acceptable for temporary buffers, provided the stack size is formally analysed and sized with margin.

3. **Fixed-size memory pool allocator.** If objects must be "allocated" and "freed" at runtime, use a fixed-size pool:

```c
/* Pool of N fixed-size blocks, allocated at startup. */
/* No fragmentation: all blocks are the same size. */
/* Deterministic O(1) allocation and free. */
/* Can prove at compile time that at most N objects exist simultaneously. */

#define POOL_SIZE    16
#define BLOCK_SIZE   64

static uint8_t pool_storage[POOL_SIZE][BLOCK_SIZE];
static bool    pool_used[POOL_SIZE] = {false};

void *pool_alloc(void) {
    for (int i = 0; i < POOL_SIZE; i++) {
        if (!pool_used[i]) {
            pool_used[i] = true;
            return pool_storage[i];
        }
    }
    return NULL;   /* pool exhausted — this is a design error, not a runtime error */
}
```

**The key insight:** In safety-critical software, resource exhaustion must be detectable at design time (by formal analysis or testing), not discovered at runtime in the field.

---

### Question F3
**What is a watchdog timer and how does it contribute to system safety? Describe the difference between an independent watchdog and a window watchdog.**

**Answer:**

A **watchdog timer** is a hardware counter that resets the system if not periodically serviced ("kicked") by software. Its purpose is to detect software lockup, runaway loops, or deadlock and recover to a known safe state.

**Safety contribution:** A watchdog provides a last-resort recovery mechanism for software faults. If a safety-critical control loop stops executing (due to a deadlock, stack overflow, or infinite loop), the watchdog triggers a reset and the system restarts into a safe state rather than remaining in an unknown, potentially dangerous condition.

**Independent watchdog (IWDG):**

```
Behaviour: Hardware counter counts down continuously.
           Software must write the "kick" sequence (magic word + reload)
           before the counter reaches 0.
           If software fails to kick within the timeout period, the
           counter reaches 0 and a hard reset is triggered.

Window: The kick can be issued at ANY time before the counter expires.
        There is no minimum time between kicks.

Problem: A runaway loop that executes the kick instruction on every iteration
         will successfully prevent the watchdog from firing, even though the
         system is executing abnormally.
```

**Window watchdog (WWDG):**

```
Behaviour: Hardware counter counts down.
           Software MUST kick within a window: not before time T_min and
           not after time T_max.

        |------|------------|------|
        0    T_min        T_max  (counter expires, reset)
               ^    Valid    ^
               |   window   |

Too early: kick issued before T_min => immediate RESET.
Too late:  kick not issued before T_max => RESET.
Just right: kick issued between T_min and T_max => counter reloaded.

Benefit: A runaway loop that kicks the WWDG too rapidly (before T_min)
         triggers an immediate reset. The window forces the watchdog kick
         to occur at the expected program flow point, at the expected time.
```

**For safety-critical applications:** The WWDG is strongly preferred. In an RTOS-based system, the watchdog task executes at a known period. The WWDG window is set to match that period ± tolerance. Any deviation from expected timing (too fast or too slow) is detected.

**Limitation of watchdogs as a sole safety mechanism:** A watchdog can detect that the CPU has stopped or is looping, but it cannot detect:
- A CPU executing incorrect computations (silent data corruption).
- A correct program producing wrong output due to a systematic fault in the algorithm.
- Multiple redundant watchdogs being reset by a single piece of code (common-cause failure).

For SIL 2 and above, a watchdog is a necessary but not sufficient safety mechanism.

---

## Tier 2 — Intermediate

### Question I1
**Describe four software safety mechanisms required or recommended by IEC 61508 Part 3. For each, provide a C code example showing how it is implemented.**

**Answer:**

**1. Control flow monitoring**

Verify that the program executes in the expected order. A flow signature is computed and checked at key points.

```c
/* Control flow signature monitoring.
 * Each function updates a running signature. The expected final value
 * is verified at the end of the safety function execution cycle. */

static uint32_t flow_sig;   /* running signature — must match expected value */

#define FLOW_POINT(id)  (flow_sig ^= (uint32_t)(id))

/* Expected flow for the main safety function:
 * read_sensors() -> process_data() -> actuate() -> log_result()
 * Expected final signature = 0xA1 ^ 0xB2 ^ 0xC3 ^ 0xD4 = 0x14 */
#define EXPECTED_FLOW_SIG  0x14U

void safety_function_cycle(void)
{
    flow_sig = 0;

    FLOW_POINT(0xA1);
    read_sensors();

    FLOW_POINT(0xB2);
    process_data();

    FLOW_POINT(0xC3);
    actuate_outputs();

    FLOW_POINT(0xD4);
    log_cycle_result();

    /* Verify: all steps executed in correct order */
    if (flow_sig != EXPECTED_FLOW_SIG) {
        safety_fault_handler(FAULT_CONTROL_FLOW);
    }
}
```

**2. Data integrity checking (CRC on safety-critical RAM)**

RAM bit flips (from EMI or cosmic rays) can corrupt safety-critical variables silently. Periodic CRC checks detect this.

```c
/* Protect a safety-critical structure with a CRC computed at write time.
 * Recompute and compare at read time. */

typedef struct {
    int32_t   setpoint;         /* safety-critical value */
    int32_t   measured;
    uint32_t  control_flags;
    uint32_t  crc;              /* CRC-32 of the preceding fields */
} safety_data_t;

static safety_data_t s_data;

void safety_data_write(int32_t sp, int32_t meas, uint32_t flags)
{
    s_data.setpoint      = sp;
    s_data.measured      = meas;
    s_data.control_flags = flags;
    /* Compute CRC over all fields except the crc field itself */
    s_data.crc = crc32_compute((uint8_t *)&s_data,
                                offsetof(safety_data_t, crc));
}

bool safety_data_read(safety_data_t *out)
{
    uint32_t computed = crc32_compute((uint8_t *)&s_data,
                                       offsetof(safety_data_t, crc));
    if (computed != s_data.crc) {
        safety_fault_handler(FAULT_RAM_CORRUPTION);
        return false;
    }
    *out = s_data;
    return true;
}
```

**3. Redundant variable storage with voter logic**

Store safety-critical values in triplicate with independent storage locations. Use majority voting to detect single-bit errors.

```c
/* Triple Modular Redundancy (TMR) for a critical flag */

typedef struct {
    uint32_t copy_a;
    uint32_t copy_b;
    uint32_t copy_c;
} tmr_uint32_t;

/* Storage at well-separated memory addresses to reduce common-cause
 * corruption from a single memory fault (burst error) */
static tmr_uint32_t tmr_emergency_stop __attribute__((section(".safety_data")));

void tmr_write(tmr_uint32_t *tmr, uint32_t value)
{
    tmr->copy_a = value;
    tmr->copy_b = value;
    tmr->copy_c = value;
    /* Memory barrier: ensure all three writes complete before returning */
    __DSB();
}

bool tmr_read(const tmr_uint32_t *tmr, uint32_t *out)
{
    uint32_t a = tmr->copy_a;
    uint32_t b = tmr->copy_b;
    uint32_t c = tmr->copy_c;

    /* Majority voter: at least two of three must agree */
    if (a == b) { *out = a; return true; }   /* A and B agree */
    if (a == c) { *out = a; return true; }   /* A and C agree */
    if (b == c) { *out = b; return true; }   /* B and C agree */

    /* All three disagree: uncorrectable error */
    safety_fault_handler(FAULT_TMR_FAILURE);
    return false;
}
```

**4. Range and plausibility checking on sensor inputs**

Validate that sensor readings are within physically possible ranges before using them in safety calculations.

```c
#define TEMP_SENSOR_MIN_C   (-40.0f)   /* sensor operating range: -40 to +125°C */
#define TEMP_SENSOR_MAX_C   (125.0f)
#define TEMP_RATE_MAX_C_PER_S (50.0f)  /* max physical rate of change */

typedef struct {
    float     value_degC;
    uint32_t  timestamp_ms;
    bool      valid;
} temperature_reading_t;

static temperature_reading_t last_valid_temp;

bool validate_temperature(float new_temp, uint32_t now_ms,
                           temperature_reading_t *result)
{
    /* Range check: physically impossible values */
    if (new_temp < TEMP_SENSOR_MIN_C || new_temp > TEMP_SENSOR_MAX_C) {
        safety_fault_handler(FAULT_SENSOR_OUT_OF_RANGE);
        return false;
    }

    /* Rate-of-change check: temperature cannot change faster than physics allows */
    if (last_valid_temp.valid) {
        float dt_s  = (float)(now_ms - last_valid_temp.timestamp_ms) / 1000.0f;
        float dT    = fabsf(new_temp - last_valid_temp.value_degC);
        float rate  = (dt_s > 0.0f) ? (dT / dt_s) : 0.0f;

        if (rate > TEMP_RATE_MAX_C_PER_S) {
            safety_fault_handler(FAULT_SENSOR_IMPLAUSIBLE_RATE);
            return false;
        }
    }

    result->value_degC   = new_temp;
    result->timestamp_ms = now_ms;
    result->valid        = true;
    last_valid_temp      = *result;
    return true;
}
```

---

### Question I2
**Explain the concept of diagnostic coverage (DC) in IEC 61508. How does it affect the hardware SIL achievement? Give an example of a diagnostic mechanism and its associated DC.**

**Answer:**

**Diagnostic Coverage (DC)** is the fraction of the dangerous failure rate of a hardware element that is detected by automated diagnostics:

```
DC = lambda_detected_dangerous / lambda_total_dangerous

Where:
  lambda_detected_dangerous  = rate of dangerous failures detected by diagnostics
  lambda_total_dangerous     = total rate of dangerous failures (detected + undetected)

DC = 0:    no diagnostics (all failures are undetected until a demand)
DC = 60%:  low coverage (most dangerous failures go undetected)
DC = 90%:  medium coverage
DC = 99%:  high coverage (very few dangerous failures are undetected)
```

**How DC affects SIL achievement:**

For a hardware element to achieve a given SIL, it must meet both:
1. A Safe Failure Fraction (SFF) requirement: the fraction of ALL failures that are safe or detected.
2. A hardware fault tolerance (HFT) requirement: the system must tolerate N dangerous hardware faults without entering a dangerous state.

IEC 61508 Part 2, Table 3 (for Type B components — complex hardware):

```
                    HFT = 0         HFT = 1         HFT = 2
SFF < 60%:          Not recommended SIL 1           SIL 2
60% <= SFF < 90%:   SIL 1           SIL 2           SIL 3
90% <= SFF < 99%:   SIL 2           SIL 3           SIL 4
SFF >= 99%:         SIL 3           SIL 4           SIL 4
```

A single-channel design (HFT = 0) with SFF < 90% can achieve at most SIL 1. To achieve SIL 2 with a single channel, SFF must be at least 90% — which requires diagnostic coverage of 90% of dangerous failures.

**Example diagnostic mechanism and its DC:**

**CPU self-test using a test program:**
- The CPU executes a sequence of known-result computations at startup and periodically.
- If the result deviates, a fault is flagged.
- Detects: ALU errors, register file corruption, stuck pipeline.
- Diagnostic coverage: 90–95% for a comprehensive test program.
- Does not detect: faults that produce correct results for the specific test vectors (limited observability).

**RAM test (checkerboard pattern):**
```c
/* Write alternating 0x55 / 0xAA pattern to RAM region.
 * Read back and verify. Detects: stuck-at faults, addressing errors.
 * Note: this test is destructive — must be done at startup or on
 * a RAM region not currently in use. */

bool ram_self_test(uint32_t *start, uint32_t *end)
{
    /* Write phase: 0x55555555 */
    for (uint32_t *p = start; p < end; p++) {
        *p = 0x55555555UL;
    }
    /* Verify phase */
    for (uint32_t *p = start; p < end; p++) {
        if (*p != 0x55555555UL) return false;
    }

    /* Write complementary pattern: 0xAAAAAAAA */
    for (uint32_t *p = start; p < end; p++) {
        *p = 0xAAAAAAAAUL;
    }
    for (uint32_t *p = start; p < end; p++) {
        if (*p != 0xAAAAAAAAUL) return false;
    }

    return true;
}
/* DC for stuck-at faults: ~95%. For addressing faults: ~90%. */
```

---

### Question I3
**What is a "safe state" and why must every safety function define one? Provide two examples of safe state definitions from different application domains.**

**Answer:**

A **safe state** is a condition of the system in which the risk to persons or the environment is acceptably low, even though the system may not be fulfilling its intended function. The safe state is the condition the system must enter when a fault is detected and normal operation can no longer be guaranteed.

**Why every safety function must define a safe state:**

When a fault is detected, the system must do something. Without a defined safe state, the default behaviour is undefined — the system may remain in a partially functional, partially faulty state that is more dangerous than simply stopping. The safe state must be:
1. Achievable from the current system state within a defined time window.
2. Maintained even in the presence of further faults (the safe state itself must be robust).
3. Defined before design begins, because hardware and software architecture depend on it.

**Example 1 — Industrial motor control:**

Safety function: "Stop the motor if the guard door is opened while the motor is running."
Safe state: Motor shaft stationary, motor drive de-energised, brake applied.

```
Definition:
  - Motor output torque = 0 Nm (torque command set to zero).
  - PWM signals to the inverter gate drivers = disabled (tri-state or low).
  - Mechanical brake engaged (fail-safe spring brake: spring applies when de-energised).
  - Status indicator: amber LED flashing "FAULT".
  - The system remains in safe state until an operator manually acknowledges
    the fault and re-enables the system.

"Passive fail-safe" design note: the mechanical brake engages by spring force
  when its solenoid is de-energised. Therefore the safe state is achieved by
  removing power, not by actively commanding the brake. Power loss (including
  MCU failure) automatically achieves the safe state.
```

**Example 2 — Automotive electronic power steering (EPS):**

Safety function: "Disable power-assisted steering if the torque sensor fails."
Safe state: Manual steering (driver still has mechanical steering, but without assistance).

```
Definition:
  - Electric motor assist torque = 0 Nm (motor disabled, current = 0).
  - Steering column clutch: disengaged (no mechanical coupling to motor).
  - Dashboard warning: "Steering Fault" displayed.
  - System remains in safe state for remainder of ignition cycle.
  - Safe state is NOT "lock the steering wheel" — that would be catastrophically
    dangerous. Manual steering at higher effort is safe; no steering is not.

Design implication: The EPS controller must default to zero torque on any
  fault — not to maximum assist, which could cause unintended steering.
  This is another passive fail-safe: the motor produces zero torque when
  unpowered, so power removal achieves the safe state automatically.
```

**Key principle: passive fail-safe.** For safety-critical systems, the safe state should be achieved when power is removed or the actuator is de-energised, not when power is applied. This ensures that a power supply failure, MCU failure, or software lockup automatically brings the system to a safe condition without requiring active intervention from software.

---

## Tier 3 — Advanced

### Question A1
**Describe a full FMEA (Failure Mode and Effects Analysis) for a simple embedded temperature monitoring safety function. Identify at least five failure modes, their effects, severity, detection methods, and mitigations.**

**Answer:**

**System under analysis:** A single-channel temperature monitoring controller that must activate a cooling fan if the measured temperature exceeds 80°C. Required SIL: SIL 1.

**FMEA table:**

| # | Component | Failure Mode | Effect on Safety Function | Severity | Probability | Detection Method | DC | Mitigation |
|---|---|---|---|---|---|---|---|---|
| 1 | NTC thermistor | Open circuit | ADC reads maximum voltage (effective: -273°C). Safety function sees temperature below threshold: fan stays off. | Critical — high temperature undetected | Low (wire break) | Out-of-range check: temperature < -40°C is physically impossible | 95% | Define lower range limit; flag any reading below -35°C as sensor fault; activate fan on sensor fault (fail-safe) |
| 2 | NTC thermistor | Short circuit | ADC reads 0 V (effective: +∞°C or above sensor range). Two sub-cases: if the fault reads as >80°C, fan activates (fail-safe). If ADC clips to max range and software truncates, may read as valid high temperature. | Low for this sub-case (fan activates) | Low | Upper range check: >125°C is beyond sensor rating | 90% | Upper range check identical to lower; any impossible reading triggers fault and fan-on |
| 3 | ADC (internal) | Gain error / offset drift | Temperature readings are biased: actual 85°C reads as 75°C. Fan does not activate. | Critical — fan activation threshold missed | Very low | Cross-check with a second temperature source (PCB thermal sensor or second ADC channel) | 90% | Periodic ADC calibration against internal reference voltage; dual-sensor cross-validation for SIL 2+ |
| 4 | MCU software | Memory corruption of threshold variable | The 80°C threshold stored in RAM is corrupted to 200°C. Fan never activates. | Critical | Very low (radiation / EMI) | CRC check on safety-critical RAM region; refresh threshold from flash every cycle | 95% | TMR for threshold value; CRC on safety data structure; compare against flash-resident backup |
| 5 | Fan motor | Electrical failure (open coil) | Fan commanded to run but does not spin. Temperature continues rising despite fan command. | Critical — fan appears to activate but thermal protection fails | Low | Monitor fan tachometer output; expected RPM within 2 seconds of command | 85% | Tachometer feedback check: if fan_cmd==ON and RPM < threshold after 2 s, escalate to second safety action (cut power to heater) |
| 6 | Software | Watchdog not kicked (deadlock) | MCU stops executing the monitoring loop. Fan output state frozen at last value. | Critical if fan was last set to OFF | Very low | IWDG triggers reset; WWDG detects timing deviation | 99% | WWDG configured with tight window; on reset, output register initialises to fan-ON (fail-safe default state) |
| 7 | Power supply | 3.3 V rail sags to 2.8 V | MCU and ADC still operational but fan driver MOSFET gate voltage insufficient for full saturation. Fan may run at reduced speed. | Moderate — reduced cooling capacity | Very low | Supply voltage monitored by ADC channel on internal bandgap reference; compare supply to expected range | 80% | Brownout detection with reset threshold at 2.9 V; ensure fan driver operates correctly at minimum specified supply |

**FMEA conclusions for SIL 1 achievement:**

The most critical undetected failure modes are #1, #3, and #4 — all of which allow the safety function to fail silently (fan off when temperature is dangerously high). Mitigations:
1. Implement out-of-range checks for both sensor limits.
2. CRC-protect the threshold variable in RAM; refresh from flash each cycle.
3. Add tachometer feedback to verify fan operation.

With these mitigations, the overall dangerous undetected failure rate should meet SIL 1 PFH requirements.

---

### Question A2
**Compare lockstep dual-core execution to software-implemented redundancy for a SIL 2 embedded controller. What does each detect, what are the failure modes of each approach, and which is preferred at SIL 3?**

**Answer:**

**Lockstep dual-core execution:**

```
Architecture:
  Core A (main core):    executes the safety application
  Core B (checker core): executes the same code in parallel, cycle-for-cycle
  Comparator logic:      hardware circuit compares outputs of A and B every cycle
  If outputs differ:     fault interrupt triggered immediately

Example devices:
  TMS570LC4357 (TI):    Cortex-R5F in lockstep for ISO 26262 ASIL D
  STM32H5 (ST):         Cortex-M33 with shadow core option
  TriCore TC397 (Infineon): lockstep mode for ASIL D automotive

What lockstep detects:
  - Transient faults: cosmic ray bit-flip affecting one core's pipeline or register file
  - Permanent faults: stuck-at in ALU, address lines, data bus
  - Timing faults: one core computes faster due to process variation (detected by
    cycle-accurate comparison)
  - Most random hardware failures in the processor core itself

What lockstep does NOT detect:
  - Common cause failures: both cores affected simultaneously (EMI, power supply glitch,
    cosmic ray striking both cores on same die)
  - Systematic software faults: a bug in the application produces wrong output on BOTH
    cores identically — the comparator sees identical wrong results and reports no fault
  - External hardware faults: sensor input corruption, actuator failure, memory outside
    the lockstep domain

Coverage (DC): typically 90–99% for random hardware failures in the CPU core.
```

**Software-implemented redundancy (SIL 2 approach without lockstep hardware):**

```
Architecture:
  Single CPU executes the safety function twice using diverse versions:
    Version A: compute result using one algorithm / data path
    Version B: compute result using a different algorithm or data representation
  Software voter: compare results of A and B; flag fault if they differ.

Example — diverse computation:
  Compute temperature in Celsius and Kelvin from the same ADC reading.
  If (celsius + 273.15) deviates from kelvin by more than 0.1°C: fault.

What software redundancy detects:
  - Systematic faults in one version that are not shared with the diverse version
  - Data corruption between computation passes (if separate RAM is used for A and B)
  - Some transient faults that affect one computation but not the other

What software redundancy does NOT detect:
  - Faults that affect both execution passes identically (same CPU, same clock domain)
  - Transient faults at the computation step (affects both A and B using same ALU)
  - Hardware faults in the CPU (not observable by software)

Coverage: 60–80% for random hardware faults (significantly lower than lockstep).
```

**Comparison table:**

| Property | Lockstep dual-core | Software redundancy |
|---|---|---|
| Hardware cost | High (dedicated second core) | Low (software only) |
| Development effort | Low (hardware handles comparison) | High (develop and validate two diverse versions) |
| DC for CPU random failures | 90–99% | 60–80% |
| Detects systematic software bugs | No | Partially (if versions are truly diverse) |
| Common cause failure vulnerability | Medium (same die) | High (same ALU, same clock) |
| SIL 2 achievable? | Yes, with SFF analysis | Yes, with careful design |
| SIL 3 achievable? | Yes (TMS570 achieves ASIL D) | Difficult — DC requirements are very tight |

**Preferred approach at SIL 3:**

IEC 61508 Part 2 for hardware SIL 3 with a single-channel architecture (HFT = 0) requires SFF >= 99%, which requires DC >= 99%. Software redundancy on a single CPU achieves DC of approximately 60–80% for hardware faults — insufficient.

At SIL 3, the preferred architecture is:
1. **Lockstep hardware** with diverse memory (CPU core random hardware failures addressed), combined with
2. **Diverse software** between a main channel and a monitoring channel (systematic software faults addressed), combined with
3. **Independent watchdog** on a separate power domain (MCU total failure addressed).

For the highest safety (SIL 3/4 or ASIL D), the safety manual for certified processors like the TMS570 defines exactly which diagnostic test sequences must be executed and which failure modes are covered, providing the IEC 61508 SFF value needed for the safety case.

---

### Question A3
**What is the purpose of a Safety Case, and what documents must a development team produce under IEC 61508 to support a SIL 2 software certification?**

**Answer:**

**Purpose of a Safety Case:**

A Safety Case is a structured argument, supported by evidence, that a system is acceptably safe for a specified application in a specified operating environment. It is not a single document — it is a collection of evidence and the logical chain of reasoning that connects the evidence to the safety claims.

The Safety Case answers: "We claim this system is safe at SIL 2. Here is why you should believe that claim, and here is the evidence supporting each step of the argument."

**Documents required for a SIL 2 software certification under IEC 61508 Part 3:**

```
Phase: Safety Planning
  Document: Software Safety Plan (SSP)
  Content:  Defines the development methodology, tools, personnel qualifications,
            verification activities, configuration management strategy, and
            how IEC 61508 Part 3 requirements are addressed for SIL 2.

Phase: Requirements
  Document: Software Safety Requirements Specification (SSRS)
  Content:  Derived from the overall system safety requirements.
            Defines: safety functions, SIL allocation per function,
            response time requirements, safe state definitions,
            interface requirements to hardware and other software.
  Verification: Formal review (inspection against checklist);
                traceability from SSRS to system requirements.

Phase: Architecture Design
  Document: Software Architecture Design (SAD)
  Content:  Top-level decomposition into software components.
            Independence between safety-related and non-safety-related code.
            Fault containment boundaries.
            Communication interfaces between components.
            For SIL 2: semi-formal methods (structure charts, state machines)
            are required; formal methods (Z notation, B method) are recommended.

Phase: Detailed Design
  Document: Software Unit Design (SUD)
  Content:  Detailed design of each software unit (function, module).
            Covers: data flows, control flows, resource usage, timing analysis.
            For SIL 2: structured programming is required; no dynamic objects
            (no recursion, no dynamic allocation — enforced by MISRA C).

Phase: Implementation
  Document: Source code with inline documentation
  Standard: MISRA C:2012 compliance mandatory for SIL 2.
             Static analysis tool output (zero justified deviations from
             Required rules).
             Coding standard compliance report from approved tool.

Phase: Testing
  Documents:
    Software Unit Test Plan and Report
      - Requirement: statement coverage 100% (SIL 1), branch coverage 100% (SIL 2).
      - MC/DC (Modified Condition/Decision Coverage) recommended for SIL 2,
        required for SIL 3/4.
      - All SIL 2 safety functions must have dedicated test cases tracing to SSRS.

    Software Integration Test Plan and Report
      - Tests interactions between software units.
      - Boundary conditions between components.

    Software Validation Report
      - Demonstrates that the software meets the SSRS.
      - Tests performed in a representative environment (target hardware or
        qualified simulator).
      - Independent review by a person not involved in software development.

Phase: Safety Assessment
  Document: Functional Safety Assessment (FSA)
  Content:  Independent third-party assessment (required for SIL 2) that
            reviews all documents, test results, and tool qualifications.
            Produces: FSA Report documenting whether the development process
            and evidence are sufficient to support the SIL 2 safety claim.
            Issued by a TÜV, Exida, Bureau Veritas, or equivalent assessor.

Configuration Management:
  Document: Configuration Management Plan + tool records
  Content:  All documents, source code, test vectors, and tool versions under
            version control. Full traceability matrix: requirement -> design ->
            code -> test case. Build reproducibility: the exact binary produced
            by the build can be reproduced from the stored source and tool versions.
```

**Key practical insight:** The documentation burden for IEC 61508 SIL 2 is substantial. A rule of thumb is that documentation and testing effort equals or exceeds implementation effort — often 60–70% of total project cost. Organisations that attempt to retrofit IEC 61508 compliance onto an existing codebase routinely underestimate this cost by 5–10x. The standard must be applied from the beginning of the development lifecycle, not added at the end.
