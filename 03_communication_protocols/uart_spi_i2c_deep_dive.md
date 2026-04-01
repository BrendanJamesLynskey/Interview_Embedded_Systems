# UART, SPI, and I2C Deep Dive

## Overview

UART, SPI, and I2C are the three workhorses of embedded peripheral communication. Nearly every
embedded systems interview covers at least one of them in depth. This file goes beyond the basics
to cover the edge cases, timing subtleties, and system-level integration topics that differentiate
candidates at top companies.

Topics covered:
- UART framing, baud rate generation, flow control (RTS/CTS, XON/XOFF), and DMA integration
- SPI clock polarity (CPOL), clock phase (CPHA), mode configuration, and multi-device wiring
- I2C addressing, clock stretching, multi-master arbitration, and repeated starts

---

## Fundamentals

### Q1. What is UART and how is a frame structured?

**Question:** Describe the structure of a UART frame and explain how the receiver synchronises to
the transmitter without a shared clock signal.

**Answer:**

UART (Universal Asynchronous Receiver/Transmitter) is an asynchronous serial protocol with no
shared clock. Both sides must be configured to the same baud rate (bits per second) in advance.

**UART frame structure (standard 8N1):**

```
Idle:  ___________
Start: |
Data:       D0  D1  D2  D3  D4  D5  D6  D7
Stop:                                        |___

Line level (idle = HIGH / mark):

_______   __________________________________________
       |_| S | D0| D1| D2| D3| D4| D5| D6| D7| ST|__

       S  = start bit (always LOW)
       Dn = data bits (LSB first by convention)
       ST = stop bit (always HIGH, 1 or 2 bits)
```

**Synchronisation mechanism:**

1. The line idles HIGH. The receiver watches for a HIGH-to-LOW transition (start bit).
2. On detecting the falling edge, the receiver waits 1.5 bit periods before sampling the first
   data bit. This centres the sample point in the middle of each bit cell.
3. Subsequent data bits are sampled every 1 bit period thereafter.
4. The stop bit is sampled and must be HIGH. A LOW stop bit generates a framing error.

**Frame format options:**

| Parameter | Typical Values | Notes |
|-----------|---------------|-------|
| Data bits | 7 or 8 | 8 is nearly universal |
| Parity | None (N), Even (E), Odd (O) | Most modern systems use N |
| Stop bits | 1 or 2 | 2 required at low baud rates or to allow receiver time to process |
| Baud rate | 9600, 115200, 921600, ... | Must match on both ends |

**Common notation:** 8N1 means 8 data bits, No parity, 1 stop bit. Total wire time per byte:
10 bit periods (1 start + 8 data + 1 stop).

---

### Q2. How is baud rate generated in hardware? What is the effect of a baud rate mismatch?

**Question:** Explain how a microcontroller generates a target baud rate from its system clock.
How much baud rate error is acceptable before framing errors occur?

**Answer:**

**Baud rate generation:**

Most UART peripherals use an integer (or fractional) clock divider:

```
Baud rate = f_PCLK / (16 * BRR)

where BRR = baud rate register value (integer divisor)
and the factor 16 comes from the 16x oversampling used to find sample centre.
```

For a target baud rate of 115200 with a 72 MHz peripheral clock:

```
BRR = 72,000,000 / (16 * 115200) = 72,000,000 / 1,843,200 = 39.0625

Round to nearest integer: BRR = 39
Actual baud rate = 72,000,000 / (16 * 39) = 115384 baud
Error = (115384 - 115200) / 115200 * 100% = +0.16%
```

Fractional baud rate registers (e.g., STM32's USART_BRR with DIV_Mantissa and DIV_Fraction)
allow sub-integer divisors, reducing the error to near zero.

**Effect of baud rate mismatch:**

The receiver samples each bit at the centre of its expected bit period. With a mismatch, the
sampling point drifts across a frame:

```
Error accumulates from the start bit edge.
For an 8N1 frame, the last data bit (D7) is sampled at 8.5 bit periods from the start bit edge.

Maximum tolerable accumulated error at D7 sample point = ±0.5 bit period.
Allowable per-bit error = 0.5 / 8.5 = ±5.9% per side.
Total combined error budget = ±5.9% (combined transmitter + receiver error must stay within this).
```

In practice, manufacturers specify UART receivers as accepting up to ±2% total clock error.
Exceeding this causes intermittent framing errors, especially at high baud rates.

---

### Q3. What is UART hardware flow control (RTS/CTS) and when must you use it?

**Question:** Explain the RTS/CTS handshake mechanism. What problem does it solve, and what is
the difference between hardware flow control and software flow control (XON/XOFF)?

**Answer:**

**The problem:** A fast transmitter can overflow the receiver's FIFO or software buffer if the
receiver cannot consume data quickly enough (e.g., during context switches or interrupt latency).

**RTS/CTS mechanism (hardware flow control):**

- **RTS** (Request To Send): output from the receiver, input to the transmitter.
- **CTS** (Clear To Send): output from the transmitter's peer; received by the transmitter.

Signal names are confusingly from the perspective of the DTE (computer) side, not the
peripheral side. In practice, RTS and CTS are cross-wired: the receiver's RTS pin connects
to the transmitter's CTS pin.

```
Device A (transmitter)          Device B (receiver)
TX  ----------------------->  RX
RX  <-----------------------  TX
CTS <-----------------------  RTS   (B asserts RTS = "I have space in my FIFO")
RTS ----------------------->  CTS   (A asserts RTS = "I have space" — for full-duplex)
```

**Operation:**
- When Device B's receive FIFO falls below a threshold, it deasserts RTS (goes HIGH = inactive,
  using RS-232 convention; or LOW for TTL active-low convention).
- Device A monitors CTS. When CTS is deasserted, A stops transmitting after the current byte.
- When B's FIFO drains sufficiently, B reasserts RTS and A resumes.

**This happens in hardware** — no CPU intervention required once configured. Latency is on the
order of the UART peripheral's detection time (typically 1-2 bit periods).

**XON/XOFF (software flow control):**

- The receiver transmits a special byte (XOFF = 0x13, Ctrl-S) in-band to tell the transmitter
  to stop. It transmits XON (0x11, Ctrl-Q) to resume.
- Pros: works on a 3-wire connection (TX, RX, GND); no extra hardware pins.
- Cons: those byte values cannot appear in the data stream without escaping; higher latency
  (software must transmit and process the control bytes); unreliable if any bytes are dropped.

**When to use hardware flow control:**
- High-speed links (>115200 baud)
- DMA-driven reception where the software response time is unbounded
- Any protocol where data loss is unacceptable and software handling of XON/XOFF is too slow

---

### Q4. What are the four SPI modes and how do CPOL and CPHA define them?

**Question:** Explain SPI clock polarity (CPOL) and clock phase (CPHA). Draw the waveform for
each mode and state which mode is most common.

**Answer:**

SPI has four modes defined by two parameters:

- **CPOL (Clock Polarity):** the idle state of the clock line.
  - CPOL=0: clock idles LOW
  - CPOL=1: clock idles HIGH

- **CPHA (Clock Phase):** which clock edge drives (shifts) data and which captures (samples) it.
  - CPHA=0: data is driven on the inactive-to-active edge and sampled on the active-to-inactive edge.
    ("Data is valid on the leading edge of the clock.")
  - CPHA=1: data is driven on the active-to-inactive edge and sampled on the inactive-to-active edge.
    ("Data is valid on the trailing edge of the clock.")

**Mode table:**

| Mode | CPOL | CPHA | Clock idle | Data captured on | Used by |
|------|------|------|-----------|-----------------|---------|
| 0    | 0    | 0    | LOW        | Rising edge     | Most flash (W25Qxx), SD cards |
| 1    | 0    | 1    | LOW        | Falling edge    | Some ADCs |
| 2    | 1    | 0    | HIGH       | Falling edge    | Some sensors |
| 3    | 1    | 1    | HIGH       | Rising edge     | Some IMUs (e.g., ADXL345) |

**Waveforms:**

```
Mode 0 (CPOL=0, CPHA=0):

SCK:  ___|--|_|--|_|--|_|--|_|--|_|--|_|--|_|--|___
MOSI: ___[  D7  |  D6  |  D5  |  D4  |  D3  ...]__
      Data valid before first rising edge; sampled on rising edge

Mode 1 (CPOL=0, CPHA=1):

SCK:  ___|--|_|--|_|--|_|--|_|--|_|--|_|--|_|--|___
MOSI: _________[  D7  |  D6  |  D5  |  D4  ...]__
      Data shifts on rising edge; sampled on falling edge

Mode 2 (CPOL=1, CPHA=0):

SCK:  ------|_|--|_|--|_|--|_|--|_|--|_|--|_|------
MOSI: ___[  D7  |  D6  |  D5  |  D4  |  D3  ...]__
      Data valid before first falling edge; sampled on falling edge

Mode 3 (CPOL=1, CPHA=1):

SCK:  ------|_|--|_|--|_|--|_|--|_|--|_|--|_|------
MOSI: _________[  D7  |  D6  |  D5  |  D4  ...]__
      Data shifts on falling edge; sampled on rising edge
```

**Mode 0 is by far the most common.** When a datasheet does not specify the SPI mode, try
Mode 0 first. If the device documentation states "data valid on rising clock edge" and
"clock idles low", that is Mode 0.

**Common mistake:** confusing which edge "shifts" vs "samples". The key invariant is that
MOSI/MISO data must be stable when the sampling edge arrives. If CPHA=0, the master drives
new data slightly before the first active clock edge; if CPHA=1, new data is driven on each
active clock edge, and the slave samples on the subsequent idle-going edge.

---

### Q5. Explain the I2C bus address scheme, START/STOP conditions, and ACK/NACK.

**Question:** Describe how I2C initiates and terminates a transaction, how devices are addressed,
and how the ACK bit works. What is the significance of a NACK?

**Answer:**

**Bus signals:**
- **SDA** (Serial Data): bidirectional, open-drain with pull-up resistor.
- **SCL** (Serial Clock): driven by the master, open-drain.

Open-drain means any device can pull the line LOW, but the line only goes HIGH when all
devices release it (wired-AND). This is fundamental to I2C arbitration and clock stretching.

**START condition:**
A HIGH-to-LOW transition on SDA while SCL is HIGH. This is the unique condition that begins
every transaction. Any idle master can issue a START.

**STOP condition:**
A LOW-to-HIGH transition on SDA while SCL is HIGH. Releases the bus. Only the current master
may issue STOP (unless arbitration is lost).

```
START:                STOP:
SCL: _______   _____  SCL: _______   _____
            |_|              |___|__|
SDA: _____      __   SDA: __   _______
          |____|                  |___|
          ^ SDA falls             ^ SDA rises
            while SCL high          while SCL high
```

**7-bit addressing frame:**

```
Bit:   7    6    5    4    3    2    1    0   | ACK
       A6   A5   A4   A3   A2   A1   A0  R/W |  A
                                               ^
                                               Master releases SDA; addressed
                                               slave pulls SDA LOW to ACK
```

- Bits 7-1: 7-bit device address (112 valid addresses; 16 are reserved)
- Bit 0: R/W direction bit. 0 = write (master sends data), 1 = read (master receives data)
- ACK bit: the addressed slave must pull SDA LOW within the 9th clock pulse. If no device
  responds, SDA stays HIGH = NACK (No ACKnowledge).

**Data bytes:**

After the address byte, each subsequent byte is followed by an ACK/NACK:
- During write transactions: the slave ACKs each received byte. A NACK means the slave
  cannot accept more data (buffer full, or an error state).
- During read transactions: the master ACKs each received byte. The master sends a NACK
  on the last byte it wants to receive, signalling to the slave to stop driving data, after
  which the master issues STOP.

**NACK meanings:**
| Context | NACK meaning |
|---------|-------------|
| After address byte | No device with that address is on the bus |
| During write | Slave cannot accept more data (full or invalid state) |
| During read (from master) | Master has received all the bytes it wanted |
| After register address | Slave does not support that register address |

---

### Q6. What is I2C clock stretching?

**Question:** Explain clock stretching in I2C. Which device performs it, and how does the master
detect and respond to it?

**Answer:**

**Clock stretching** is the mechanism by which a slave (or occasionally a slow master) holds SCL
LOW to pause the transaction, giving itself more time to prepare data or process a received byte.

**How it works:**

After the master releases SCL HIGH (the start of the high phase), the slave may hold SCL LOW by
pulling it down. Because SCL is open-drain (wired-AND), the line stays LOW regardless of what
the master drives.

```
Master drives SCL HIGH:    SCL: __|  (high phase begins)
Slave holds SCL LOW:       SCL: __|__  (slave stretches, SCL stays LOW)
Slave releases SCL:        SCL: __|   (high phase continues)
```

**The master must** wait for SCL to actually go HIGH (i.e., sample the SCL pin) before measuring
the clock high period. A master that ignores stretching and proceeds based on a timer alone will
violate the protocol.

**When slaves use clock stretching:**
- Slow EEPROM after receiving a write command (it needs time to latch/program the data)
- ADC after a conversion request (conversion takes time)
- Any slave whose internal clock is significantly slower than the bus clock

**Slave implementation (bare-metal):**

```c
/* A software I2C slave (e.g., on a bit-bang GPIO implementation):
   After receiving a byte, hold SCL LOW until the data byte is processed. */
void i2c_slave_after_byte_received(uint8_t byte) {
    /* Pull SCL LOW immediately before master releases it */
    GPIO_WritePin(SCL_PIN, GPIO_PIN_RESET);  /* stretch */
    process_received_byte(byte);             /* do the work */
    GPIO_WritePin(SCL_PIN, GPIO_PIN_SET);    /* release, allow to float HIGH */
}
```

**Master implementation:**

The master must check the actual SCL level after driving it HIGH, not just assume it went HIGH:

```c
/* Bit-bang I2C master: generate one clock high phase */
void i2c_master_clock_high(void) {
    GPIO_WritePin(SCL_PIN, GPIO_PIN_SET);       /* release SCL, allow pull-up */
    uint32_t timeout = 1000;
    /* Wait for SCL to actually reach HIGH — slave may be stretching */
    while (!GPIO_ReadPin(SCL_PIN) && --timeout) {
        delay_us(1);
    }
    if (timeout == 0) {
        handle_i2c_timeout_error();             /* slave is stuck or bus fault */
    }
    delay_us(t_HIGH);                           /* hold SCL HIGH for required time */
}
```

Hardware I2C peripherals (STM32, NXP, etc.) handle stretching automatically in hardware.

---

## Intermediate

### Q7. How does I2C multi-master arbitration work?

**Question:** Two masters simultaneously attempt to start an I2C transaction. Describe the
arbitration process. Which master wins? Can data be corrupted?

**Answer:**

I2C arbitration is **non-destructive**: only one master wins, and the losing master backs off
without corrupting the winning master's transaction.

**Mechanism:**

Because SDA is open-drain (wired-AND), a master that drives HIGH while another drives LOW will
read back LOW. Each master compares what it drives on SDA to what it actually reads back.

```
Master A transmits:  1 0 0 1 0 1 1 0 ... (address 0x4B, write)
Master B transmits:  1 0 0 1 0 0 0 0 ... (address 0x48, write)

Bit-by-bit:
  Bit 6: A=1, B=1 -> bus=1. Both see 1. Both continue.
  Bit 5: A=0, B=0 -> bus=0. Both see 0. Both continue.
  Bit 4: A=0, B=0 -> bus=0. Both see 0. Both continue.
  Bit 3: A=1, B=1 -> bus=1. Both see 1. Both continue.
  Bit 2: A=0, B=0 -> bus=0. Both see 0. Both continue.
  Bit 1: A=1, B=0 -> bus=0. A reads 0, but drove 1.
                              A detects loss! A backs off immediately.
                              B continues — B sent 0 and sees 0.
```

After bit 1, Master A stops driving SDA and SCL and waits for the bus to become free (STOP
condition) before re-attempting. Master B continues its transaction uninterrupted.

**The transaction is not corrupted** because:
1. Both masters were transmitting identical bits until the point of divergence.
2. The bus value at the point of divergence is Master B's intended value (LOW = 0).
3. The slave receiving Master B's address sees a valid, correct transmission.

**The lower address wins** (because a 0 dominates over a 1 on the open-drain bus). This is
analogous to CAN bus arbitration.

**SCL arbitration:** Both masters must drive SCL at the same rate. If they differ, the slower
master stretches the clock, which the faster master must accommodate (same as clock stretching).

**When arbitration fails gracefully:**
- Masters must monitor SDA during every bit they drive.
- If a master loses arbitration mid-transaction, it must not issue a STOP condition
  (doing so would terminate the winning master's transaction on the shared bus).

---

### Q8. What is an I2C repeated START condition and why is it needed?

**Question:** Explain the repeated START (Sr) condition. Give a concrete example showing why a
STOP between two operations would cause a problem.

**Answer:**

A **repeated START** allows a master to initiate a new transaction without releasing the bus
(without issuing a STOP). It is issued by producing a START condition while SCL is still driven
(not in the idle state).

**Why STOP between operations causes problems:**

The classic example is reading from a device where the read address must be set atomically with
the read operation. Consider an I2C EEPROM or sensor:

```
To read register 0x05 from device address 0x68:

INCORRECT (with STOP):
  1. Master sends: START | 0x68 W | 0x05 (register addr) | STOP
  2. ** Another master could seize the bus here **
  3. Master sends: START | 0x68 R | (reads 1 byte) | STOP

With a STOP, another master could address 0x68 between steps 1 and 3 and
change the device's internal address pointer, causing step 3 to read the
wrong register.

CORRECT (with repeated START):
  1. Master sends: START | 0x68 W | 0x05 (register addr)
  2. Master sends: Sr (repeated START) | 0x68 R | (reads 1 byte) | STOP

No STOP between write and read: the bus is never released, so no other
master can intervene. The write phase sets the register pointer; the
repeated START switches direction to read without releasing the slave.
```

**Wire-level view:**

```
Normal START:   SCL=HIGH,  SDA: H->L  (from idle state)
Repeated START: SCL=HIGH,  SDA: H->L  (master releases SDA high, then pulls low)
                               ^--- same condition, but bus was not idle

STOP then START:  ...data | SCL=HIGH, SDA: L->H | (bus idle) | SCL=HIGH, SDA: H->L
Repeated START:   ...data | SCL=HIGH, SDA: H->L  (no STOP)
```

**Common use cases:**
- Write register address, then read value (sensors, EEPROMs, RTC chips)
- Atomic read-modify-write (though most devices provide this natively)
- Direction change within a compound command (some display controllers)

---

### Q9. How do you integrate a UART peripheral with DMA? What configuration is required?

**Question:** Describe the DMA configuration required for zero-CPU-overhead UART reception.
What interrupt events are still needed? What happens if the DMA buffer overflows?

**Answer:**

**Goal:** Receive N bytes via UART without the CPU handling each byte individually.

**DMA configuration for UART RX (example: STM32 HAL / LL approach):**

```c
/* DMA stream configuration for USART1 RX */
typedef struct {
    volatile uint8_t *src;   /* USART data register address         */
    uint8_t          *dst;   /* application receive buffer           */
    uint16_t          count; /* number of bytes to receive           */
} DmaRxConfig;

void uart_dma_rx_init(USART_TypeDef *uart, uint8_t *buf, uint16_t len)
{
    /* 1. Configure DMA channel:
     *    - Source: USART->DR (peripheral address, fixed)
     *    - Destination: buf (memory address, incrementing)
     *    - Transfer size: byte (8-bit)
     *    - Direction: peripheral-to-memory
     *    - Mode: circular (so DMA wraps and never stops)
     *    - Priority: high (UART bytes arrive in real-time)
     *    - Transfer complete interrupt: enabled
     *    - Half-transfer interrupt: enabled (for double-buffering)
     */
    DMA_InitTypeDef dma = {
        .Direction           = DMA_PERIPH_TO_MEMORY,
        .PeriphInc           = DMA_PINC_DISABLE,    /* source fixed (USART->DR) */
        .MemInc              = DMA_MINC_ENABLE,     /* destination increments   */
        .PeriphDataAlignment = DMA_PDATAALIGN_BYTE,
        .MemDataAlignment    = DMA_MDATAALIGN_BYTE,
        .Mode                = DMA_CIRCULAR,
        .Priority            = DMA_PRIORITY_HIGH,
    };
    HAL_DMA_Init(&hdma_usart1_rx, &dma);

    /* 2. Enable UART DMA request */
    SET_BIT(uart->CR3, USART_CR3_DMAR);

    /* 3. Start DMA transfer */
    HAL_DMA_Start_IT(&hdma_usart1_rx,
                     (uint32_t)&uart->DR,
                     (uint32_t)buf,
                     len);
}
```

**Interrupts still needed:**

Even with DMA, two interrupt sources are essential:

1. **DMA half-transfer and transfer-complete interrupts:** For a circular DMA buffer of length N,
   the half-transfer interrupt fires at N/2 bytes (process the first half), and the
   transfer-complete interrupt fires at N bytes (process the second half). This is the
   double-buffer (ping-pong) technique — the CPU processes one half while DMA fills the other.

2. **UART IDLE line interrupt:** The UART peripheral can generate an interrupt when the RX
   line is idle for one full frame period after the last received byte. This is critical for
   variable-length packets: the DMA may not be exactly at a half or full point when the
   packet ends, but the IDLE interrupt fires immediately after the last byte, allowing the
   CPU to read NDTR (number of data items remaining) to determine how many bytes arrived.

**DMA buffer overflow:**

In circular mode, if the CPU does not process data fast enough, the DMA write pointer will
wrap around and overwrite unprocessed data. The DMA hardware does NOT stop; it silently
overwrites.

Prevention strategies:
1. **Ensure the buffer is large enough** for the maximum burst of unprocessed data.
2. **Monitor NDTR** (number of data items remaining in DMA) in the application to detect
   when the write pointer is approaching the read pointer.
3. **Use the UART overrun error flag** (ORE in USART_SR): set when a new byte arrives before
   the previous one was consumed. This is distinct from DMA overflow but indicates the FIFO
   or shift register was full.
4. **Raise an error flag** and notify the application layer when overflow is detected; do not
   silently discard or corrupt data.

---

### Q10. How do SPI chip-select timing requirements affect bus design?

**Question:** What are setup and hold times around chip-select (CS) assertion and deassertion?
What happens if you violate them? How does this affect multi-device SPI bus design?

**Answer:**

**CS timing parameters (from a typical SPI flash datasheet):**

```
t_CSS (CS setup before first SCK edge):  typically 5-10 ns
t_CSH (CS hold after last SCK edge):     typically 5-10 ns
t_CSDF (CS deassert high time):          typically 50-100 ns (deselect between transactions)

Waveform:
CS:  __|                                            |__
        ^<-- t_CSS -->^                  ^<-- t_CSH-->^
SCK:               _|--|_|--|_|--|_|--|_|
```

**Effect of violations:**

- **Violating t_CSS:** The device's internal state machine has not had time to recognise the
  CS transition. The first few bits of data may be ignored or misinterpreted, leading to
  corrupted register reads/writes or flash program failures.
- **Violating t_CSH:** The device may latch the last clock edge incorrectly or begin its
  internal command processing prematurely.
- **Violating t_CSDF:** The device has not had enough time between transactions to reset its
  internal state. Back-to-back transactions without adequate CS-high time can cause the
  device to treat the second transaction as a continuation of the first.

**Multi-device bus design implications:**

With multiple SPI devices sharing MOSI, MISO, and SCK, each device has its own CS line.

```
MCU                  Flash (CS0)
 |-- SCK ------------|-- SCK
 |-- MOSI -----------|-- MOSI
 |-- MISO -----------|-- MISO    (all three shared)
 |-- CS0 ------------|-- CS

                     ADC (CS1)
 |-- CS1 ------------|-- CS
```

Key rules:
1. **Only one CS may be asserted at a time.** Driving two CS lines simultaneously if both
   devices drive MISO will cause bus contention (two outputs fighting each other). This can
   damage the MCU's input protection diodes.
2. **MISO is tri-stated when CS is deasserted.** Devices must release MISO (high-impedance)
   when CS is HIGH. Verify this in the datasheet — some devices have a configurable MISO
   output enable.
3. **CS deassertion time between devices** must satisfy the most demanding t_CSDF on the bus.
4. **Different devices may require different SPI modes.** Mode switching (changing CPOL/CPHA)
   must be done while all CS lines are deasserted, as changing the clock polarity affects
   the active level of SCK, which active devices might misinterpret.

---

## Advanced

### Q11. Explain I2C pull-up resistor selection. What are the tradeoffs?

**Question:** How do you calculate the correct I2C pull-up resistor value? What happens with
resistors that are too large or too small?

**Answer:**

I2C buses use open-drain drivers: devices can only pull the bus LOW; the bus goes HIGH only
through the pull-up resistors. This means the pull-up resistors determine the rise time.

**Maximum pull-up resistance (rise time constraint):**

The I2C specification defines maximum rise times:
- Standard mode (100 kHz): t_r(max) = 1000 ns
- Fast mode (400 kHz): t_r(max) = 300 ns
- Fast-mode Plus (1 MHz): t_r(max) = 120 ns

Rise time is approximately: `t_r ≈ 0.8473 * R_pull * C_bus`

Where C_bus is the total bus capacitance (PCB traces + device input capacitances, typically
5-50 pF for a short bus, up to 400 pF for I2C maximum spec).

For Fast mode with 100 pF bus capacitance:
```
R_max = t_r(max) / (0.8473 * C_bus)
      = 300 ns / (0.8473 * 100 pF)
      = 300e-9 / 84.73e-12
      = 3.54 kΩ
```

**Minimum pull-up resistance (current / power constraint):**

When a device pulls the bus LOW, current flows through the pull-up resistor to ground
through the device's output transistor. Devices specify a maximum sink current (typically
3 mA for standard I2C, 20 mA for Fast-mode Plus).

For VCC = 3.3 V, I_sink(max) = 3 mA:
```
R_min = VCC / I_sink(max) = 3.3 V / 3 mA = 1.1 kΩ
```

**Practical selection:**

| Bus speed | Typical R_pull | C_bus | Notes |
|-----------|---------------|-------|-------|
| Standard (100 kHz) | 4.7 kΩ | <100 pF | Common default |
| Fast mode (400 kHz) | 2.2 kΩ | <100 pF | Most embedded systems |
| Fast-mode Plus (1 MHz) | 1.0 kΩ | <50 pF | Short traces only |

**Too-large resistors:** Slow rise times. At high frequencies, SCL/SDA never reach VCC before
the next clock edge. Logic levels become ambiguous. Communication fails at high speeds
(often works at 100 kHz, fails at 400 kHz).

**Too-small resistors:** Excessive current when the line is held LOW. Device output stages
may not meet VOL(max) (maximum LOW output voltage) spec, causing the receiver to see an
invalid LOW level. Increased power consumption. Device sink transistors may be stressed.

---

### Q12. How does SPI DMA work with a half-duplex display controller (e.g., writing pixels)?

**Question:** Describe the complete DMA+SPI transfer sequence for a large pixel buffer write
to an ILI9341 or similar SPI display. What CPU involvement is required?

**Answer:**

SPI displays like the ILI9341 use a command/data interface over SPI with an additional DC
(Data/Command) pin to distinguish register commands from pixel data.

**Transaction flow for a full-screen pixel fill (240x320 pixels, 16-bit color = 153,600 bytes):**

```
Phase 1 (CPU, fast): Send display commands
  Assert CS
  Assert DC=0 (command mode)
  SPI write: 0x2A (Column Address Set) + 4 bytes (x_start, x_end)
  SPI write: 0x2B (Page Address Set)   + 4 bytes (y_start, y_end)
  SPI write: 0x2C (Memory Write command)
  Set DC=1 (data mode)

Phase 2 (DMA, background): Stream pixel data
  Configure DMA:
    Source: pixel_buffer (memory, incrementing)
    Destination: SPI->DR (fixed)
    Count: 153,600 bytes
    Mode: normal (single shot)
    TC interrupt: enabled

  Start DMA transfer (CPU is free until TC interrupt fires)

Phase 3 (CPU, on DMA TC interrupt):
  Wait for SPI to finish transmitting last byte (TXE and BSY flags)
  Deassert CS
  Notify application layer (display update complete)
```

**CPU involvement reduced to:**
- Phase 1: ~10 SPI byte writes for commands (microseconds)
- Phase 3: ~10 cycles in interrupt handler to check flags and deassert CS

**Without DMA:** The CPU would execute 153,600 polling loops checking TXE (transmit empty)
flag, consuming the entire MCU for the duration of the transfer (at 40 MHz SPI, ~3.8 ms — 
enough for 7.6 million cycles on a 2 GHz CPU, or hundreds of RTOS task switches on an MCU).

**Key implementation details:**

```c
void display_write_pixels_dma(const uint16_t *pixels, uint32_t count)
{
    /* Swap bytes for big-endian SPI (display expects big-endian 16-bit color) */
    /* Either use hardware byte-swap, or pre-swap in the pixel buffer */

    /* Configure SPI for 16-bit data frame if supported */
    /* (halves the DMA transaction count) */
    SPI1->CR1 = (SPI1->CR1 & ~SPI_CR1_DFF) | SPI_CR1_DFF;  /* 16-bit frames */

    dma_start_transfer(
        (uint32_t)pixels,        /* source */
        (uint32_t)&SPI1->DR,     /* destination */
        count,                   /* count in 16-bit units */
        DMA_DIRECTION_M2P,
        DMA_SIZE_HALFWORD        /* 16-bit */
    );
    /* Return immediately — DMA runs in background */
}

void DMA2_Stream3_IRQHandler(void)   /* SPI1 TX DMA complete */
{
    if (DMA2->LISR & DMA_LISR_TCIF3) {
        DMA2->LIFCR = DMA_LIFCR_CTCIF3;        /* clear TC flag */
        while (SPI1->SR & SPI_SR_BSY) {}        /* wait for last bit to clock out */
        CS_DEASSERT();
        display_transfer_complete_callback();   /* notify app */
    }
}
```

---

### Q13. What are the I2C arbitration edge cases that can corrupt a transaction?

**Question:** Describe two edge cases in I2C multi-master arbitration that can cause subtle
failures. How do you detect and recover from them?

**Answer:**

**Edge Case 1: Spurious START detection (SCL synchronisation race)**

When two masters start simultaneously, they must synchronise their SCL clocks. The I2C spec
defines SCL synchronisation: the clock low period is the longest of any master's low period
(because any master can extend LOW by holding SCL down). However, if two masters have
slightly offset clocks and one master begins its first bit slightly before the other, the
following race can occur:

```
Master A:  START emitted at T=0. Drives first bit (address bit 6).
Master B:  START emitted at T=1 ns. Also drives bit 6.

Slave:     Sees START from Master A. Then, 1 ns later, sees what appears
           to be another START (Master B's SDA transition), but SCL is
           now being driven — this may be interpreted as a repeated START
           or a bus error depending on SCL state.
```

**Detection:** The slave must reset its state machine on any unexpected START or STOP condition
mid-frame. Most hardware I2C peripherals implement this correctly.

**Edge Case 2: Arbitration loss at the ACK bit**

Arbitration loss is usually checked on data bits, but it can also occur on the ACK bit:

```
Master A wins arbitration and its target device (0x40) ACKs (pulls SDA LOW).
Master B has already lost arbitration but its I2C peripheral detected
the loss LATE, on the ACK bit instead of a data bit.

Result: Master B's hardware may have already clocked in the ACK as part
of its own transaction, and may proceed incorrectly, treating Master A's
target device's ACK as a response to Master B's (different) address.
```

**Correct behavior:** A master must stop immediately upon detecting arbitration loss,
regardless of which bit the loss was detected on. The I2C peripheral's ARBITRATION_LOST
interrupt must trigger an immediate abort of the current transaction.

**Recovery sequence:**

```c
void i2c_arbitration_lost_handler(I2C_TypeDef *i2c)
{
    /* 1. Clear the ARLO (arbitration lost) flag */
    CLEAR_BIT(i2c->SR1, I2C_SR1_ARLO);

    /* 2. Do NOT issue a STOP — the bus is owned by another master */
    /* 3. Wait for the bus to become free (BUSY flag clears after STOP) */
    uint32_t timeout = 10000;
    while ((i2c->SR2 & I2C_SR2_BUSY) && --timeout) { __NOP(); }

    /* 4. Re-enable the peripheral */
    SET_BIT(i2c->CR1, I2C_CR1_PE);

    /* 5. Retry the transaction from the beginning */
    schedule_i2c_retry();
}
```

**Edge Case 3: Stuck bus (SCL or SDA held LOW)**

A slave may be in the middle of a byte transmission during a power cycle. When the master
restarts, the slave is still driving MISO LOW (waiting to complete the byte), causing the
bus to appear permanently busy.

**Recovery (I2C bus reset procedure per NXP application note AN10216):**

```c
void i2c_bus_reset(void)
{
    /* Switch SCL/SDA to GPIO mode temporarily */
    configure_as_gpio(SCL_PIN, SDA_PIN);

    /* Generate up to 9 SCL pulses to clock out any stuck slave byte */
    for (int i = 0; i < 9; i++) {
        GPIO_WritePin(SCL_PIN, 0);
        delay_us(5);
        GPIO_WritePin(SCL_PIN, 1);
        delay_us(5);
        if (GPIO_ReadPin(SDA_PIN) == 1) break;   /* SDA released — slave freed */
    }

    /* Issue a STOP condition */
    GPIO_WritePin(SDA_PIN, 0);
    delay_us(5);
    GPIO_WritePin(SCL_PIN, 1);
    delay_us(5);
    GPIO_WritePin(SDA_PIN, 1);   /* STOP: SDA rises while SCL high */
    delay_us(5);

    /* Restore I2C peripheral control */
    configure_as_i2c(SCL_PIN, SDA_PIN);
}
```

---

## Summary Reference Table

| Feature | UART | SPI | I2C |
|---------|------|-----|-----|
| Wires | 2 (TX, RX) + optional RTS/CTS | 4 (SCK, MOSI, MISO, CS) per device | 2 (SDA, SCL) |
| Clock | Asynchronous (baud rate) | Synchronous (master-driven) | Synchronous (master-driven) |
| Multi-device | Not natively (RS-485 extension) | Yes (one CS pin per device) | Yes (7-bit or 10-bit address) |
| Duplex | Full-duplex | Full-duplex | Half-duplex |
| Max speed | ~20 Mbps (UART standard varies) | Tens to hundreds of MHz | 100k / 400k / 1M / 3.4M / 5M bps |
| Typical use | Debug, GPS, modems, consoles | Flash, ADC, display, SD card | Sensors, EEPROMs, PMICs, RTC |
| Bus topology | Point-to-point | Star (one master, N slaves) | Multi-master multi-slave bus |
| Error detection | Parity bit, framing error | None built-in (CRC added by SW) | ACK/NACK per byte |
| Pull-ups needed | No | No | Yes (both SDA and SCL) |
| DMA support | Yes (byte granularity) | Yes (byte or word) | Less common (complex sequencing) |
