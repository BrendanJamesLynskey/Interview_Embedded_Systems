# Logic Analyser and Oscilloscope

## Prerequisites
- Basic understanding of digital signals: voltage levels, logic thresholds
- Familiarity with common serial protocols: UART, SPI, I2C
- Awareness of signal integrity concepts: rise time, reflections

---

## Concept Reference

### Oscilloscope vs Logic Analyser — Tool Selection

```
Oscilloscope                          Logic Analyser
------------------------------------  ------------------------------------
Measures analogue voltage vs time     Measures digital state vs time
Typically 2-4 channels                8 to 512+ channels simultaneously
Bandwidth: 50 MHz - 10 GHz           Sample rate limited by memory depth
Vertical resolution: 8-12 bits ADC   Vertical: binary (0/1) only
Shows signal shape, overshoot, noise  Shows transitions and timing
Essential for: signal integrity,      Essential for: protocol decoding,
  analogue debugging, power rails,      multi-signal correlation, bus
  EMI investigation, rise/fall times    capture, digital state analysis
```

**Rule of thumb:** If you need to know the shape of the signal, use an oscilloscope. If you need to know the sequence and timing of many signals simultaneously, use a logic analyser.

---

### Oscilloscope Probe Types and Loading Effects

```
Passive 1x probe:                     Passive 10x probe:
  Tip impedance: ~1 MΩ || 100 pF      Tip impedance: ~10 MΩ || 10-15 pF
  BW: typically 6 MHz                  BW: typically 200 MHz - 500 MHz
  Good for: DC and low-freq signals    Good for: most digital measurements
  Problem: high capacitive loading     Attenuation: signal divided by 10
            degrades rise time on                    (accounted for by scope)
            fast signals

Active probe (FET buffer):
  Tip impedance: ~1 MΩ || 0.3-1 pF
  BW: 1 GHz - 10 GHz
  Good for: high-speed signals, DDR, SerDes eye diagrams
  Cost: high ($500 - $5000)

Current probe (Hall effect clamp):
  Measures current via magnetic field
  Good for: power consumption profiling, inrush current, short detection
  Does not require circuit opening
```

**Probe ground lead length matters:**

```
Ground lead forms an inductor in series with the probe tip capacitance:
  L = 20 nH/cm for typical ground lead wire
  Resonant frequency = 1/(2π√LC)

Ground lead 10 cm long:
  L ≈ 200 nH, C_probe ≈ 12 pF
  f_resonant ≈ 1/(2π√(200e-9 * 12e-12)) ≈ 103 MHz

At resonance: signal appears amplified and ringing is severe.
Fix: use the shortest possible ground spring or SMD ground connection.
     Many probes include a coil-spring ground accessory for high-frequency work.
```

---

### Oscilloscope Triggering Modes

```
Edge trigger: Fires when signal crosses threshold on rising or falling edge.
  Use for: periodic signals, synchronising to a known event.

Pulse-width trigger: Fires when a pulse width is greater than, less than,
  or equal to a specified duration.
  Use for: detecting unexpectedly short or long glitches.
  Example: Trigger if SPI CS pulse < 1 µs (should be 8 µs minimum).

Pattern trigger (mixed-signal scope): Fires when a logic combination
  of multiple channels matches a specified pattern.
  Use for: triggering on a specific byte in a UART stream.

Serial decode trigger: Protocol-aware; fires on specific decoded values.
  Example: Trigger when I2C address byte == 0x68 (MPU-6050 sensor).

Runt trigger: Fires when a pulse crosses one threshold but not the other
  (i.e., incomplete transition -- often indicates signal integrity problem).
  Use for: catching intermittent glitches on noisy power rails.

Timeout trigger: Fires when a signal stays at the same level longer than
  a specified time.
  Use for: detecting protocol stalls, stuck-at faults.
```

---

### Logic Analyser Operation Principles

```
Sampling:
  State mode:   sampled synchronously with an external clock (e.g., SPI SCK)
                Best for: synchronous bus analysis (SPI, I2C, parallel buses)
                Avoids setup/hold metastability on captured data

  Timing mode:  sampled at a fixed internal rate (e.g., 500 MHz)
                Best for: asynchronous protocols (UART, CAN), timing measurements
                Resolution = 1 / sample_rate

Threshold setting:
  Standard TTL:   1.5 V threshold
  Standard 3.3 V: 1.65 V threshold
  1.8 V logic:    0.9 V threshold
  WRONG threshold => missed edges, wrong decoded values

Memory depth:
  Captured samples stored in analyser memory.
  Total capture time = memory_depth / sample_rate
  Example: 256 Msample at 500 MHz = 512 ms capture window

Compression (RLE):
  Store only transitions, not every sample.
  Dramatically extends effective capture on slow/idle signals.
  A CAN bus idle at 500 kbps needs very few transitions stored.
```

---

### Protocol Decoding

Most modern logic analysers and mixed-signal oscilloscopes include software protocol decoders that translate raw digital transitions into human-readable frames.

**UART decode setup:**

```
Required settings:
  Baud rate:      115200 (must match transmitter)
  Data bits:      8
  Parity:         None
  Stop bits:      1
  Voltage level:  3.3 V TTL (not RS-232 ±12 V -- do not connect RS-232 directly!)
  Channel:        CH1 = TX, CH2 = RX

Decoded output:
  Frame: [START][D7..D0][STOP]
  Display: hex 0x48 'H', 0x65 'e', 0x6C 'l', 0x6C 'l', 0x6F 'o'

Common decode errors:
  "Framing error" => baud rate mismatch (most common)
  "Parity error"  => noise on the line, or parity setting mismatch
  All 0xFF frames => SWDIO/JTAG line captured instead of UART
```

**SPI decode setup:**

```
Required channels:
  SCK  - clock
  MOSI - master to slave
  MISO - slave to master
  CS#  - chip select (active low)

Required settings:
  Clock polarity (CPOL): 0 or 1
  Clock phase    (CPHA): 0 or 1 -- WRONG MODE is the most common SPI problem
  Bit order:             MSB first or LSB first
  Data width:            8 or 16 bits

CPOL/CPHA combinations:
  Mode 0 (CPOL=0, CPHA=0): sample on rising edge,  idle clock low
  Mode 1 (CPOL=0, CPHA=1): sample on falling edge, idle clock low
  Mode 2 (CPOL=1, CPHA=0): sample on falling edge, idle clock high
  Mode 3 (CPOL=1, CPHA=1): sample on rising edge,  idle clock high
```

**I2C decode setup:**

```
Required channels:
  SCL - clock
  SDA - data (open-drain, needs pull-up resistor on bus)

Decoded frame structure:
  [START][ADDR 7-bit][R/W][ACK][DATA byte][ACK]...[STOP]

Common I2C errors visible on logic analyser:
  NACK (no acknowledge): slave not responding
    => Wrong address, slave not powered, I2C pins not configured as open-drain
  Clock stretching: slave holds SCL low to pause master
    => Can cause timeout if master does not support it
  Repeated START: master changes direction without releasing bus
  Arbitration lost: multi-master bus, two masters tried to transmit simultaneously
```

---

### Timing Analysis Fundamentals

```
Setup and hold time:
  Setup time (tsu): data must be stable this long BEFORE the clock edge
  Hold time (thd):  data must remain stable this long AFTER the clock edge
  Violation => metastability => unpredictable output, intermittent failures

Measuring with logic analyser:
  Use cursors to measure time between data transition and nearest clock edge.
  Ensure time >= tsu (before edge) and >= thd (after edge).

Propagation delay measurement:
  Use two channels and cursor difference measurement.
  Example: measure SPI CS assertion to first SCK edge.
  This must meet tLEAD (CS setup before clock) specification of slave device.

Glitch detection:
  Set minimum pulse width filter in analyser to remove noise.
  Enable pulse-width trigger on scope to capture anomalies.
  Glitches on CS# line: cause spurious slave selections.
  Glitches on SCK: cause extra clock edges, shifting data by 1 bit.
```

---

### Analog vs Digital Probing Decisions

```
Use an OSCILLOSCOPE for:
  - Power supply noise and ripple measurement (mV-level detail)
  - I2C/SPI signal integrity: rise/fall times, overshoot, ringing
  - Clock quality: jitter, duty cycle, phase noise
  - Measuring pull-up strength: RC time constant on SDA/SCL
  - UART signal eye diagram at high baud rates (1+ Mbps)
  - Debugging reset line glitches (threshold vs analogue shape matters)
  - Any signal where the shape contains diagnostic information

Use a LOGIC ANALYSER for:
  - Capturing 100s of frames of protocol data
  - Correlating multiple signals (SPI CS, SCK, MOSI, MISO + IRQ pin)
  - Long-duration capture: idle-then-burst sequences
  - State machines: visualising all handshake signals simultaneously
  - Debugging software: correlating GPIO toggles with code events
  - CAN bus frame decoding: ID, DLC, data bytes, CRC fields

Use BOTH simultaneously (mixed-signal oscilloscope or scope + LA):
  - Proving that a decoded error correlates with a signal integrity fault
  - I2C stuck bus: measure whether SCL is truly being pulled low or floating
  - Power-induced errors: correlating SMPS switching noise with protocol errors
```

---

## Tier 1 — Fundamentals

### Question F1
**You suspect that I2C communication is failing to a sensor. Walk through how you would use a logic analyser to diagnose the problem.**

**Answer:**

**Step 1 — Physical connection:**
Connect the logic analyser probes to SCL and SDA, as close to the sensor as possible. Set the voltage threshold to match the bus voltage (1.65 V for 3.3 V, 0.9 V for 1.8 V). Verify both signals are pulled high when the bus is idle.

**Step 2 — Capture a transaction:**
Enable I2C protocol decoding with the correct channel assignment. Trigger on a START condition or just free-run and capture a few hundred milliseconds of activity.

**Step 3 — Analyse the decoded output:**

Systematically check the following in order:

```
a) Is there any activity at all?
   No transitions => software not calling the I2C peripheral, wrong GPIO pins,
                     peripheral clock not enabled.

b) Does the START condition appear?
   No START => SDA not being pulled low before SCL.
   Malformed START => check GPIO alternate function configuration.

c) What address is being sent?
   Verify the 7-bit address matches the sensor datasheet.
   Common error: using 8-bit address (datasheet shows 0xD0 = write, 0xD1 = read)
   but the driver uses the 7-bit form 0x68. These are the same sensor; 0xD0 >> 1 = 0x68.

d) Does the sensor ACK the address?
   NACK at address byte:
     - Wrong address
     - Sensor not powered or not yet out of reset
     - ADD pin (some sensors) selects address; check hardware
     - Multi-master: another master is addressing the bus simultaneously

e) Does the sensor ACK data bytes?
   NACK mid-transaction => register address out of range, write to read-only register.

f) Is there a STOP condition?
   Missing STOP => bus remains busy; next transaction will fail with arbitration error.
```

**Step 4 — Check signal quality on scope:**
If decoding looks correct but errors persist, use an oscilloscope to check:
- SDA and SCL rise times (should be < 1 µs for 100 kHz I2C, < 300 ns for 400 kHz)
- Overshoot or ringing indicating too-weak or too-strong pull-ups
- SCL clock stretching (slave holding SCL low longer than master expects)

---

### Question F2
**When would you use an oscilloscope over a logic analyser for debugging a SPI bus, even though SPI is a digital protocol?**

**Answer:**

SPI is digital logically, but the signals have analogue properties that affect reliability. An oscilloscope is needed when:

**1. Signal integrity is suspect:**

```
Symptom: SPI works reliably at 1 MHz but fails at 10 MHz.

Oscilloscope reveals:
  - Ringing on SCK after each edge (transmission line effect)
    => PCB trace too long, no series termination resistor
  - MISO rise time too slow (500 ns at 10 MHz -- too slow for 50 ns setup time)
    => Pull-up on MISO too weak, or slave drive strength too low
  - MOSI overshoot exceeding slave input absolute maximum voltage
    => Remove 0 Ω series resistor, add 22 Ω damping resistor
```

**2. Pull-up and drive strength characterisation:**

The output impedance of a driver combined with PCB capacitance forms an RC low-pass filter. The oscilloscope measures the actual RC time constant, which determines achievable bus speed.

**3. Clock jitter measurement:**

For high-speed SPI (> 20 MHz), clock jitter eats into the setup/hold time budget. A logic analyser typically cannot resolve jitter at the sub-nanosecond level. Use the oscilloscope's persistence display to overlay many clock edges and measure jitter width.

**4. Cross-talk investigation:**

A 40 MHz SCK transitioning on a PCB trace running parallel to MISO can capacitively couple and induce glitches. The oscilloscope can show the induced noise shape relative to the SCK edge.

**Key principle:** A logic analyser shows *what the data is*; an oscilloscope shows *whether the signal is reliably representing that data*.

---

### Question F3
**What is a trigger in the context of an oscilloscope, and why is it important? Describe three trigger modes useful for embedded debugging.**

**Answer:**

A trigger defines the condition that causes the oscilloscope to capture and display a waveform. Without a trigger, the display is a rolling, unstable picture. With a trigger, the scope captures a waveform aligned to a specific event of interest, allowing stable display and precise measurement.

**Trigger Mode 1 — Edge trigger (most common):**

```
Use: Viewing periodic signals, measuring frequency, general-purpose debugging.
Setting: Rising edge on CH1, threshold at 1.65 V.
Example: Trigger on SPI CS# falling edge to see the start of each transaction.
```

**Trigger Mode 2 — Pulse-width (anomaly detection):**

```
Use: Finding protocol glitches or unexpected short pulses.
Setting: Trigger when pulse width on CH1 < 100 ns.
Example: An I2C SDA line is glitching to low for ~30 ns due to capacitive
         coupling from a nearby switching power supply. Edge trigger misses it
         because the main transitions look normal. Pulse-width < 100 ns trigger
         catches the runt pulse.
```

**Trigger Mode 3 — Serial protocol trigger (protocol-aware):**

```
Use: Finding specific data patterns in a stream without analysing every frame.
Setting: UART trigger, baud 115200, trigger when byte value = 0x06 (ACK).
Example: A bootloader occasionally sends a NACK (0x15) instead of ACK.
         Rather than capturing thousands of frames, trigger specifically on
         0x15 to capture the context immediately before and after the NACK.
```

**Common mistake:** Leaving the trigger source on a channel that has no signal connected, causing the scope to display a flat line or to trigger randomly. Always verify the trigger indicator in the display corner shows "Trig'd" (triggered), not "Auto" (free-running).

---

## Tier 2 — Intermediate

### Question I1
**You are investigating why a CAN bus message is occasionally received with a CRC error at one node only. Describe your diagnostic approach using available test equipment.**

**Answer:**

A CAN CRC error at one node only (not all nodes) points to a signal integrity problem at that node, not a software or bit-timing issue. The diagnostic flow:

**Step 1 — Verify the problem is truly node-specific:**

Use a logic analyser with CAN protocol decode on both the affected node and a known-good node simultaneously. Confirm the CRC error flag only appears on the affected node's error counter, and the same frame is received correctly elsewhere.

**Step 2 — Measure differential voltage at the affected node:**

```
CAN requires differential measurement: V_CANH - V_CANL
  Dominant bit:   V_CANH - V_CANL > 0.9 V (typically 2.5 V)
  Recessive bit:  V_CANH - V_CANL < 0.5 V (typically 0 V)

Use oscilloscope with two probes in differential configuration (A-B mode):
  CH1 probe tip = CANH, CH1 ground = chassis GND
  CH2 probe tip = CANL, CH2 ground = chassis GND
  Math: CH1 - CH2

Normal waveform at 500 kbps:
  Bit period: 2 µs
  Dominant-to-recessive transition: < 50 ns edge time
  Recessive level: < 500 mV differential
  Dominant level: 2.0 - 3.0 V differential
```

**Step 3 — Check common causes of node-specific signal integrity:**

```
a) Stub length too long:
   CAN bus topology is a line with short stubs. If the affected node has
   a long stub (>30 cm at 500 kbps), reflections cause bit distortion at
   that node. Measure with scope: look for ringing on bit transitions.

b) Missing or wrong termination:
   CAN requires 120 Ω termination at each end. Measure bus resistance
   (power off): CANH-CANL should be ~60 Ω if both terminators present.
   If one node has an extra 120 Ω incorrectly placed, the bus is over-
   terminated (40 Ω), reducing differential amplitude at all nodes but
   more severely at the over-terminated node.

c) Ground offset:
   If the affected node's GND differs from bus GND by > 1 V, the CAN
   transceiver common-mode range is exceeded. Measure with scope:
   probe CANH and CANL to chassis GND separately. Common-mode voltage
   (average of CANH and CANL) should be 2.5 V ± 1 V.

d) EMI pickup specific to cable routing near affected node:
   Use scope persistence mode over many frames. Normal frames show clean
   overlaid edges. EMI appears as random spikes on recessive bits.
```

**Step 4 — Verify timing with logic analyser:**

Use the analyser's CAN decoder to check bit timing. Sample point should be configured at 75-80% of the bit period. If the affected node's bit timing configuration differs from others, it may be sampling near a bit transition where noise causes errors.

---

### Question I2
**Explain the concept of a scope's bandwidth and sample rate. A colleague says "I bought a 100 MHz oscilloscope, so it can display any signal up to 100 MHz accurately." Is this correct?**

**Answer:**

This is partially correct but importantly incomplete.

**Bandwidth** is the -3 dB frequency of the oscilloscope's analogue front-end (input amplifier + ADC chain). A 100 MHz bandwidth means a 100 MHz sine wave is attenuated to 70.7% (-3 dB) of its true amplitude. Signals significantly above this frequency are not accurately represented.

**Sample rate** is the rate at which the ADC digitises the input. According to Nyquist, the minimum required sample rate is 2x the highest frequency component. But -3 dB at 100 MHz means there are significant frequency components above 100 MHz in even a "clean" 100 MHz digital signal.

**The key problem with digital signals:**

```
A 100 MHz square wave has harmonic content:
  Fundamental: 100 MHz (displayed adequately)
  3rd harmonic: 300 MHz (severely attenuated by 100 MHz BW scope)
  5th harmonic: 500 MHz (invisible to the scope)

Consequence:
  The scope displays the 100 MHz square wave as a sine wave.
  Rise time measured on scope: ~3.5 ns (corresponds to 100 MHz BW)
  Actual rise time (with fast driver): may be <1 ns
  Displayed waveform looks like a slow, rounded sine -- not a sharp square.
```

**The scope bandwidth rule of thumb for digital signals:**

```
Rule: Scope BW >= 5x the highest fundamental frequency of interest
  For a 100 MHz digital signal: need 500 MHz scope
  For 10 MHz SPI clock: 50 MHz scope is sufficient
  For 1 GHz SerDes eye diagram: need 6 GHz+ oscilloscope

Rise time relationship:
  t_rise (10-90%) ≈ 0.35 / BW
  100 MHz BW scope => minimum measurable rise time ≈ 3.5 ns
  A signal with true rise time < 3.5 ns will appear slower than it is.
```

**Sample rate consideration:**

A 100 MHz, 1 Gsps scope has 10 samples per period at 100 MHz. This is marginally adequate for frequency measurement but too few for accurate waveform reconstruction of a square wave. 5+ samples per period on the highest harmonic of interest is a practical minimum.

---

### Question I3
**Describe how you would use a logic analyser to debug a firmware issue where an SPI flash read occasionally returns corrupted data.**

**Answer:**

Occasional SPI corruption is among the harder embedded bugs to catch because it requires capturing the exact transaction where corruption occurs. A systematic approach:

**Step 1 — Characterise the failure:**

```
Add firmware instrumentation (GPIO toggle or ITM trace):
  Before each SPI read:  toggle GPIO high
  After verification:    if data invalid, pulse a second GPIO high
  This gives hardware trigger points for the logic analyser.
```

**Step 2 — Set up the logic analyser:**

```
Channels:
  CH1: SPI SCK
  CH2: SPI MOSI (command/address from MCU)
  CH3: SPI MISO (data from flash)
  CH4: SPI CS# (chip select, active low)
  CH5: MCU GPIO "error detected" pin

Trigger: Rising edge on CH5 (error GPIO).
Pre-trigger memory: 100 ms (capture what happened BEFORE the error).
Sample rate: 10x minimum -- for 20 MHz SPI, use 200 MHz sample rate.
```

**Step 3 — Analyse the captured transaction:**

```
Correct read transaction (e.g., READ command 0x03):
  CS# falls low
  MOSI: 0x03 [command], 0x00, 0x01, 0x00 [24-bit address]
  MISO: don't care during command, then data bytes 0xAB, 0xCD, ...
  CS# rises high

Failure patterns to look for:

a) CS# glitch:
   CS# momentarily rises then falls during the transaction.
   => Flash sees two separate commands; aborts and restarts.
   => MISO returns 0xFF or default response, not requested data.
   Cause: software disabling/re-enabling SPI CS via GPIO accidentally,
          or RTOS task preemption between CS assert and transfer start.

b) Clock count wrong:
   Count the SCK edges. READ command = 32 bits command/address + N*8 data bits.
   If SCK count is not a multiple of 8, data will be bit-shifted.
   Cause: DMA transfer count off-by-one, or SPI peripheral restarted mid-transfer.

c) Mode mismatch (most common):
   Zoom in on the first byte. Is data sampled on the correct clock edge?
   Flash memory typically supports SPI Mode 0 and Mode 3.
   If the MCU SPI peripheral is in Mode 1 or 2, every byte is shifted by 1 bit.
   Usually reproducible every time, not occasional -- unless mode changes at runtime.

d) Timing violation:
   Measure tCSS (CS# assertion to first SCK edge).
   Measure tCSH (last SCK edge to CS# de-assertion).
   These must meet flash datasheet minimums (typically 5-10 ns for modern flash).
   Violation: CS# deasserted too early => last bits not latched by flash.
```

**Step 4 — Correlate with firmware:**

If the error capture shows a correctly formed protocol transaction but wrong data, the corruption is in memory (DMA overwrite, cache issue) not on the bus. If the protocol is malformed, the analyser output pinpoints the exact timing error.

---

## Tier 3 — Advanced

### Question A1
**You are debugging a system where a 10 MHz SPI clock is causing interference on an adjacent analog ADC input, resulting in ADC readings jumping by several LSBs synchronously with SPI activity. Describe how you would characterise and fix this problem, and what test equipment you would use at each stage.**

**Answer:**

This is a classic EMI/crosstalk problem requiring a systematic analogue and digital investigation.

**Phase 1 — Confirm the coupling (oscilloscope):**

```
Equipment: oscilloscope with two probes

Probe 1 (CH1): SPI SCK line — confirm the aggressor signal
Probe 2 (CH2): ADC analogue input pin — observe the victim signal

Display math: both channels simultaneously, time-aligned.

Expected finding:
  CH2 shows 50-100 mV spikes synchronous with CH1 rising and falling edges.
  Spike amplitude proportional to SPI clock edge rate (dV/dt).
  Spike polarity may alternate if both rising and falling edges couple.
```

**Phase 2 — Identify the coupling mechanism:**

```
Capacitive coupling test:
  Lift one end of the PCB trace from SCK to a via (or cut if needed for test).
  If noise on ADC disappears: coupling was via PCB trace capacitance.
  Typical coupling: C_couple = ε * A / d (parallel trace capacitance)
  At 10 MHz with 2 pF coupling and 1 kΩ ADC source impedance:
    V_noise = V_aggressor * (2πf * C * R_source) ≈ 10 * (63e6 * 2e-12 * 1000) ≈ 126 mV

Inductive coupling test:
  Shield the aggressor trace with copper tape to GND.
  If noise decreases significantly: inductive (magnetic) coupling is dominant.
  A power ground loop between the SPI return path and ADC return path is typical.

Power supply coupling test:
  Use scope on VDD_ADC rail during SPI activity.
  If VDD_ADC shows switching noise: coupling via shared power supply impedance.
  Measure with small decoupling capacitor (100 nF MLCC) probed directly at ADC VDD pin.
```

**Phase 3 — Fix strategies:**

```
For capacitive coupling:
  1. Increase physical separation: re-route SCK trace away from ADC input.
     Rule of thumb: 3x trace width separation reduces coupling by ~10x.
  2. Add ground guard trace between SCK and ADC input trace.
  3. Reduce ADC source impedance: lower Thevenin impedance of the sensor front-end.
  4. Add analogue low-pass filter at ADC input: RC filter with fc << 10 MHz.
     For 16-bit ADC at 100 kSps: fc = 1 kHz is reasonable.
     R = 1 kΩ, C = 160 nF => fc ≈ 1 kHz, 10 MHz attenuated by 80 dB.

For inductive coupling:
  1. Ensure SPI return current (GND) is not sharing the ADC analogue GND plane.
     Separate AGND and DGND, joined at a single star point near the power supply.
  2. Keep SPI traces as short as possible, or use differential pair for high-speed clock.

For power supply coupling:
  1. Add 100 nF MLCC + 10 µF electrolytic decoupling directly at ADC AVDD pin.
  2. Use a ferrite bead or LDO between digital VDD and ADC AVDD.
  3. Consider synchronising ADC sampling to a quiet period between SPI transactions.
```

**Phase 4 — Validate the fix (scope + ADC reading):**

```
Verification measurements:
  1. Scope CH1=SCK, CH2=ADC_IN: confirm noise spike reduced below 1 LSB level.
     For 16-bit ADC at 3.3 V: 1 LSB = 50 µV. Target < 50 µV coupling.

  2. Collect ADC readings over 1000 samples during SPI activity.
     Calculate standard deviation. Should equal thermal noise floor only.

  3. Spectrum analyser (if available): FFT of ADC samples should show no
     10 MHz or 20 MHz spurs (harmonics of SPI clock).
```

---

### Question A2
**Explain how you would use a logic analyser to reverse-engineer an undocumented proprietary serial protocol between two ICs on a PCB. What are the limits of this approach?**

**Answer:**

Reverse engineering a serial protocol requires systematic observation, hypothesis generation, and validation.

**Step 1 — Physical identification:**

```
Identify likely protocol candidates from:
  - Pin count between the ICs (2 pins => UART or I2C; 4 pins => SPI; 3 pins => 1-wire)
  - Chip markings: look up datasheets, application notes, FCC filings
  - PCB silkscreen: CLK, DIN, DOUT labels often present
  - Pull-up resistors: open-drain signalling (I2C, 1-wire) requires pull-ups
  - Crystal/oscillator frequency: can indicate baud rate families
```

**Step 2 — Initial capture:**

```
Set logic analyser to timing mode at 10-100x the expected clock rate.
Capture a full operational sequence (power-on, operation, idle).

Initial observations:
  a) Identify the clock signal (regular, periodic transitions).
  b) Identify CS# or sync signals (active during data bursts, idle between).
  c) Count channels: MOSI/MISO separate => SPI-like; shared line => I2C-like.
  d) Measure the idle bit period => infer baud rate.
     Example: 8.68 µs bit period => 1/8.68e-6 ≈ 115200 baud (UART).
  e) Look for start/stop bits on UART (1 bit low before data, 1+ bits high after).
  f) Look for START/STOP conditions on I2C (specific SDA-SCL patterns).
```

**Step 3 — Frame structure analysis:**

```
Export raw capture to CSV. Use spreadsheet or Python to analyse bit patterns.

For an unknown SPI-like protocol:
  Group bits by CS# assertion (one transaction per CS assertion).
  Align all transactions vertically.
  First byte/nibble often a command code: note repeating patterns.
  Subsequent bytes: data fields. Look for incrementing values (addresses),
                    constant high-byte values (fixed headers), or
                    values that correlate with observable IC behaviour.

For UART-like (async):
  Identify start bit (first falling edge after idle).
  Count 8 bits, check for parity, find stop bit.
  Try 7E1, 8N1, 8N2, 9N1 combinations with analyser decoder.
  ASCII text is easily recognisable (values 0x20-0x7E).
```

**Step 4 — Validation:**

```
Stimulate a known action (e.g., press a button, change a parameter).
Capture before and after.
Diff the captures: which bytes/fields changed?
Correlate changed fields with the action performed.
Repeat with different inputs to build a command table.
```

**Limits of the approach:**

```
a) Encrypted or checksummed protocols:
   If payload is encrypted or obfuscated, captured bytes reveal structure
   but not meaning. CRC/checksum fields can be identified (change with data)
   but not deciphered without knowing the algorithm.

b) Timing-dependent protocols:
   Some protocols use precise inter-byte gaps or timing windows for framing.
   A logic analyser captures timing but the decoder must account for it.

c) Protocol state machines:
   Some protocols only make sense in context of prior transactions.
   Capture must be long enough to observe initialisation sequences.

d) Legal considerations:
   Reverse engineering for interoperability is generally permitted in many
   jurisdictions, but copying a proprietary protocol implementation may
   infringe intellectual property. Obtain legal advice before commercialisation.

e) Physical access:
   Test points must be accessible. BGA packages with buried traces are
   not accessible without destructive delayering or focused ion beam (FIB) access.
```
