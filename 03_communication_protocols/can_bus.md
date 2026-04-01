# CAN Bus

## Overview

CAN (Controller Area Network) is a differential serial bus originally developed by Bosch for
automotive systems. Its non-destructive bitwise arbitration, robust error detection, and
guaranteed message delivery make it the dominant protocol for vehicle ECU networks, industrial
automation, and safety-critical embedded systems.

This file covers classical CAN (ISO 11898), CAN FD (ISO 11898-1:2015), frame formats,
arbitration, error frames, bus-off recovery, message filtering, and the error confinement
mechanisms that make CAN self-healing.

---

## Fundamentals

### Q1. Explain CAN bus differential signalling and why it provides noise immunity.

**Question:** How is data encoded on the CAN bus physically? What are dominant and recessive
bits, and why does this design provide better noise immunity than single-ended UART?

**Answer:**

CAN uses a twisted-pair differential bus with two lines: **CANH** and **CANL**.

**Differential signalling:**

```
Dominant  bit (logical 0): CANH = 3.5 V, CANL = 1.5 V  -> Differential = +2.0 V
Recessive bit (logical 1): CANH = 2.5 V, CANL = 2.5 V  -> Differential =  0.0 V
```

The receiver measures only the voltage difference (CANH - CANL), not the absolute voltage.
Common-mode noise (noise that affects both wires equally) cancels out:

```
Common-mode noise affects both CANH and CANL equally:
Noisy CANH = 3.5 + 0.5 = 4.0 V
Noisy CANL = 1.5 + 0.5 = 2.0 V
Differential = 4.0 - 2.0 = +2.0 V   (still reads dominant correctly)
```

**Wired-AND bus (open-collector analogy):**

Any node can force the bus to dominant (0) by driving CANH high and CANL low. The bus is
recessive (1) only when ALL nodes release it. This is the physical basis of CAN arbitration:
a dominant bit always overrides a recessive bit.

**Termination:** Both ends of the bus require 120 Ω termination resistors between CANH and
CANL to prevent signal reflections at high speeds.

**Noise immunity in practice:** CAN transceivers typically tolerate common-mode noise of
±25 V, making them suitable for environments with heavy inductive loads (motors, relays,
solenoids) where single-ended UART would require careful isolation.

---

### Q2. Describe the structure of a standard CAN 2.0A data frame.

**Question:** Draw and describe every field in a standard CAN data frame (11-bit identifier).
What is the maximum data payload?

**Answer:**

**Standard CAN frame (CAN 2.0A, 11-bit identifier):**

```
|SOF|  Arbitration Field  | Control | Data (0-8 bytes) | CRC  |ACK|EOF|IFS|
 1b   11b ID + RTR (1b)    6b        0-64 bits           15b+d  2b  7b  3b

SOF   = Start of Frame (1 dominant bit)
ID    = 11-bit message identifier (higher priority = lower numeric value)
RTR   = Remote Transmission Request: 0=data frame, 1=remote frame (requesting data)
IDE   = Identifier Extension: 0=standard frame (11-bit ID)
r0    = Reserved bit (recessive)
DLC   = Data Length Code (4 bits, 0-8)
DATA  = 0 to 8 bytes of payload
CRC   = 15-bit CRC + delimiter bit
ACK   = ACK slot (1 bit) + ACK delimiter (1 bit)
EOF   = End of Frame (7 recessive bits)
IFS   = Interframe Space (3 recessive bits minimum)
```

**Maximum throughput calculation:**

At 500 kbps with maximum payload (8 bytes, 64 bits of data):
- Minimum frame size: 1+11+1+1+1+4+64+15+1+2+7+3 = 111 bits (without bit stuffing)
- Bit stuffing adds ~20% overhead on average
- Effective throughput: approximately 8 bytes / (111 * 2 µs) ≈ 36 kB/s per bus

**Extended CAN frame (CAN 2.0B, 29-bit identifier):**

Adds an 18-bit extension to the identifier after the SRR and IDE bits, allowing 2^29 ≈
500 million unique message IDs. Used in SAE J1939, CANopen, and DeviceNet.

---

### Q3. How does CAN bus arbitration work?

**Question:** Three nodes on a CAN bus attempt to transmit simultaneously. Describe bit-by-bit
how arbitration resolves the conflict. What invariant ensures no message is corrupted?

**Answer:**

CAN uses **non-destructive bitwise arbitration** based on the wired-AND property of the bus.

**Setup:**

```
Node A transmits message with ID = 0x1A3 = 0b000_1101_0001_1
Node B transmits message with ID = 0x1A5 = 0b000_1101_0001_1 (same base)
Actually:
Node A: ID = 0b000_1101_0000_1 = 0x1A1
Node B: ID = 0b000_1101_0001_0 = 0x1A2
Node C: ID = 0b000_1101_0001_1 = 0x1A3
```

**Arbitration process (bit by bit, MSB first):**

```
Bit:  10   9    8    7    6    5    4    3    2    1    0   (ID bits)
A:     0    0    0    1    1    0    1    0    0    0    1   (0x1A1)
B:     0    0    0    1    1    0    1    0    0    1    0   (0x1A2)
C:     0    0    0    1    1    0    1    0    0    1    1   (0x1A3)
Bus:   0    0    0    1    1    0    1    0    0    1    ?

Bits 10-2: All three transmit the same value. All read back their own value.
           All continue transmitting.

Bit 1: A transmits 0. B and C transmit 1.
       Bus = dominant = 0 (wired-AND: 0 wins).
       A reads back 0 — matches. A continues.
       B reads back 0 — but transmitted 1. B detects ARBITRATION LOSS.
       C reads back 0 — but transmitted 1. C detects ARBITRATION LOSS.
       B and C immediately stop transmitting and enter receive mode.
       B and C will retry after the bus is free.

Bit 0: Only A is transmitting. A sends 1.
       Bus = 1. A continues to completion.
```

**The invariant that prevents corruption:**

Until the point of divergence, all transmitting nodes are sending identical bit patterns.
The bus value at the divergence point is the dominant value (0), which is exactly what the
winning node was transmitting. The losing nodes withdraw silently. The receiving node (and
any other nodes on the bus) see a perfectly valid, uninterrupted frame from the winning
node.

**Lower ID = higher priority:** Because a 0 bit (dominant) wins over a 1 bit (recessive),
a numerically smaller ID wins arbitration. This is how CAN priority is encoded.

---

### Q4. What are the five types of CAN error frames and what triggers each?

**Question:** Name and describe the five CAN error detection mechanisms. When is each error
frame generated?

**Answer:**

CAN has five built-in error detection mechanisms. When any node detects an error, it
transmits an error frame (6 consecutive dominant bits — deliberately violating the maximum
5-bit-stuffing rule) to abort the current transmission.

**1. Bit Error**

A transmitting node (non-arbitration, non-ACK phase) monitors the bus and compares what it
sent to what it reads back. If it transmitted a recessive bit (1) but reads back dominant (0),
or vice versa, it flags a bit error.

*Exception:* Bit errors are NOT flagged during the arbitration field (because another node
overwriting a recessive with a dominant is expected and normal) or the ACK slot (because the
receiving node drives dominant to ACK).

**2. Stuff Error**

CAN uses NRZ (Non-Return-to-Zero) encoding with bit stuffing: after 5 consecutive bits of
the same polarity, one complementary bit is inserted. The receiver strips these stuffed bits.

A stuff error occurs when a receiver sees 6 or more consecutive bits of the same polarity
in the data, CRC, or arbitration fields. This indicates corruption or a hardware malfunction.

**3. CRC Error**

Each frame includes a 15-bit CRC computed over the SOF, arbitration, control, and data
fields. Every receiver recomputes the CRC and compares it to the transmitted value.

A CRC error occurs when the receiver's computed CRC does not match the received CRC field.
The receiver transmits an error frame after the CRC delimiter bit.

**4. Form Error**

Certain fields in a CAN frame are defined as having fixed bit patterns (always recessive):
- CRC delimiter
- ACK delimiter
- EOF (7 recessive bits)
- Interframe Space

A form error occurs when a node detects a dominant bit in one of these fixed-form fields.

**5. Acknowledgement Error**

After transmitting a frame, the transmitter monitors the ACK slot. Every receiver that
successfully receives the frame (correct CRC) must pull the ACK slot dominant.

An acknowledgement error occurs when the transmitter sees the ACK slot remain recessive,
meaning no node on the bus received the frame without error. This indicates the bus has
no other nodes, or all nodes detected an error.

**Error frame format:**

```
Active error flag:  6 dominant bits  (transmitted by node that detected error)
Passive error flag: 6 recessive bits (transmitted by error-passive nodes)
Error delimiter:    8 recessive bits
```

Multiple nodes may detect the same error and superimpose their error flags, producing
up to 12 consecutive dominant bits (6 from first node + 6 from a second node that detects
the bit stuffing violation caused by the first node's error flag).

---

## Intermediate

### Q5. Explain the CAN error confinement state machine (active, passive, bus-off).

**Question:** What are the Transmit Error Counter (TEC) and Receive Error Counter (REC)?
How do nodes transition between active error, passive error, and bus-off states?

**Answer:**

Each CAN node maintains two error counters:

- **TEC** (Transmit Error Counter): incremented on transmit errors, decremented on successes.
- **REC** (Receive Error Counter): incremented on receive errors, decremented on successes.

**Counter increment/decrement rules (summary of ISO 11898):**

| Event | TEC change | REC change |
|-------|-----------|-----------|
| Transmitted successfully | -1 (min 0) | — |
| Received frame without error | — | -1 (min 0) if REC > 0 |
| Transmitter detects bit/stuff/form/ack error | +8 | — |
| Receiver detects bit/stuff/form/CRC error | — | +1 |
| Receiver detects form error in EOF/IFS | — | +8 |
| Transmitter detects dominant bit after 11 recessive | +8 | — |

**State machine:**

```
         REC, TEC < 128              TEC >= 128 or REC >= 128
[Error Active] ──────────────────> [Error Passive]
               <──────────────────
               REC < 128 AND TEC < 128

[Error Passive] ─────────────────> [Bus-Off]
                    TEC >= 256
```

**Error Active (normal operation):**
- Node transmits Active Error Flags (6 dominant bits) on error detection.
- Can interfere with other nodes' communications (dominant error flag corrupts any ongoing frame).

**Error Passive:**
- REC >= 128 or TEC >= 128.
- Node transmits Passive Error Flags (6 recessive bits) — these are invisible to other nodes
  if the bus is dominant at the same time.
- The node must wait an additional Suspend Transmission time (8 bit periods) after a frame
  before it may retransmit. This slows down a failing transmitter.

**Bus-Off:**
- TEC >= 256.
- The node completely disconnects from the bus (stops transmitting AND receiving).
- This prevents a malfunctioning node from disrupting the entire network.
- Recovery requires 128 occurrences of 11 consecutive recessive bits (bus idle).

---

### Q6. How does bus-off recovery work, and when should it be automatic vs manual?

**Question:** Describe the hardware and software aspects of bus-off recovery. When is automatic
recovery appropriate and when should it require human intervention?

**Answer:**

**Hardware recovery mechanism (ISO 11898):**

A bus-off node monitors the bus and counts sequences of 11 consecutive recessive bits (one
bus idle period). After 128 such sequences (128 * 11 = 1408 recessive bits), the CAN
controller automatically resets TEC and REC to 0 and re-enters the Error Active state.

The 128-sequence requirement ensures a minimum recovery time, preventing a continuously
faulting node from rapidly cycling through bus-off and re-entering the network.

**Software control on STM32 (example):**

```c
/* CAN controller registers controlling bus-off recovery behaviour */

/* Option 1: Automatic bus-off recovery (ABOM bit in CAN_MCR) */
void can_enable_automatic_recovery(CAN_TypeDef *can)
{
    SET_BIT(can->MCR, CAN_MCR_ABOM);   /* hardware counts 128*11 bits automatically */
}

/* Option 2: Manual recovery — application decides when to recover */
void can_handle_bus_off(CAN_TypeDef *can)
{
    if (READ_BIT(can->ESR, CAN_ESR_BOFF)) {
        /* Log the error, notify the system health monitor */
        log_can_fault(CAN_FAULT_BUS_OFF,
                      READ_REG(can->ESR));   /* capture TEC/REC at time of fault */

        /* For safety-critical systems: require explicit operator acknowledgement
           before reconnecting, or apply a retry limit. */
        if (system_can_safely_retry()) {
            /* Manually trigger recovery: */
            /* 1. Request initialisation mode */
            SET_BIT(can->MCR, CAN_MCR_INRQ);
            while (!(can->MSR & CAN_MSR_INAK)) {}   /* wait for ack */

            /* 2. Exit initialisation mode — hardware waits 128*11 bits */
            CLEAR_BIT(can->MCR, CAN_MCR_INRQ);
            while (can->MSR & CAN_MSR_INAK) {}
        }
    }
}
```

**When automatic recovery is appropriate:**
- General industrial systems where transient bus disturbances (ESD, connector bounce) can
  cause temporary bus-off, and immediate reconnection is safe.
- Nodes with low TEC/REC histories that entered bus-off due to external noise, not internal
  faults.

**When manual (or limited) recovery is required:**
- Safety-critical systems (automotive, medical): a node that went bus-off may have done so
  because its hardware is faulty. Automatically reconnecting a faulty ECU to a safety bus
  can be dangerous.
- Systems with retry limits: count bus-off events per time window. After N events, escalate
  to a fault handler rather than retrying indefinitely.
- Any node whose bus-off was caused by consistently sending malformed frames (TEC climbed
  to 256): the root cause must be investigated.

**Detection in interrupt handler:**

```c
void CAN1_SCE_IRQHandler(void)   /* CAN status change / error interrupt */
{
    uint32_t esr = CAN1->ESR;

    if (esr & CAN_ESR_BOFF) {
        /* Bus-off state entered */
        can_fault_flags |= CAN_FAULT_BUS_OFF;
        uint8_t tec = (esr & CAN_ESR_TEC_Msk) >> CAN_ESR_TEC_Pos;
        uint8_t rec = (esr & CAN_ESR_REC_Msk) >> CAN_ESR_REC_Pos;
        log_can_bus_off(tec, rec);
    }

    if (esr & CAN_ESR_EPVF) {
        /* Error passive state — warn but don't disconnect */
        can_fault_flags |= CAN_FAULT_ERROR_PASSIVE;
    }

    /* Clear interrupt flags */
    CAN1->MSR |= CAN_MSR_ERRI;
}
```

---

### Q7. What is CAN message filtering and how do hardware acceptance filters work?

**Question:** Explain identifier-based acceptance filtering in CAN hardware. Describe the
difference between mask mode and list mode filters. Why is filtering important?

**Answer:**

A CAN bus carries messages from many nodes. Every node receives every frame at the physical
layer, but most messages are irrelevant to most nodes. Hardware acceptance filters
automatically discard irrelevant messages, preventing the CPU from being interrupted for
every frame.

**Without hardware filtering:**

At 500 kbps with 8-byte frames (~111 bits), the bus can carry ~4500 frames/second. At 1 Mbps:
~9000 frames/second. Without filtering, each frame generates a CPU interrupt, consuming
significant bandwidth even on a 100+ MHz MCU.

**Mask mode filtering (identifier + mask):**

A mask filter specifies which bits of the incoming ID must match a pattern:

```
Filter ID:   0b000_1010_0010   = 0x0A2
Filter Mask: 0b111_1111_1111   = 0x7FF  (all bits must match)
Accepts only: ID == 0x0A2

Filter ID:   0b000_1010_0010   = 0x0A2
Filter Mask: 0b111_1111_1000   = 0x7F8  (lower 3 bits are don't-care)
Accepts: 0x0A0 through 0x0A7 (range of 8 IDs)
```

The matching rule: `(received_id & mask) == (filter_id & mask)`

**List mode filtering (identifier list):**

Each filter register holds an exact ID. The message must match one of the listed IDs exactly.
On STM32, each filter bank in list mode holds two 11-bit IDs (or one 29-bit extended ID).

**STM32 filter bank configuration example:**

```c
/* CAN filter configuration for STM32 */
void can_configure_filters(CAN_HandleTypeDef *hcan)
{
    CAN_FilterTypeDef filter;

    /* Filter bank 0: accept IDs 0x100-0x10F (range via mask) */
    filter.FilterBank           = 0;
    filter.FilterMode           = CAN_FILTERMODE_IDMASK;   /* mask mode */
    filter.FilterScale          = CAN_FILTERSCALE_16BIT;   /* 11-bit IDs */
    filter.FilterIdHigh         = 0x100 << 5;   /* ID: shift left 5 for filter reg */
    filter.FilterIdLow          = 0x000;
    filter.FilterMaskIdHigh     = 0x7F0 << 5;   /* mask: top 8 bits must match */
    filter.FilterMaskIdLow      = 0x0000;
    filter.FilterFIFOAssignment = CAN_RX_FIFO0;
    filter.FilterActivation     = ENABLE;
    HAL_CAN_ConfigFilter(hcan, &filter);

    /* Filter bank 1: accept exactly ID 0x200 (list mode, both entries same) */
    filter.FilterBank           = 1;
    filter.FilterMode           = CAN_FILTERMODE_IDLIST;   /* list mode */
    filter.FilterIdHigh         = 0x200 << 5;
    filter.FilterIdLow          = 0x200 << 5;   /* same ID in both slots */
    filter.FilterMaskIdHigh     = 0x200 << 5;
    filter.FilterMaskIdLow      = 0x200 << 5;
    filter.FilterFIFOAssignment = CAN_RX_FIFO1;
    filter.FilterActivation     = ENABLE;
    HAL_CAN_ConfigFilter(hcan, &filter);
}
```

**Dual FIFO assignment:**

Most CAN controllers provide two receive FIFOs. By assigning different filters to different
FIFOs, you can prioritise message handling:
- FIFO0 → high-priority safety-critical messages (motor control, brake commands)
- FIFO1 → lower-priority diagnostic or configuration messages

---

## Advanced

### Q8. Describe CAN FD. What are the key differences from classical CAN?

**Question:** What problem does CAN FD (Flexible Data-rate) solve? Describe the two bit rate
phases, the extended payload, and the implications for transceivers and nodes.

**Answer:**

**The problem with classical CAN:**

Classical CAN is limited to:
- 8 bytes of payload per frame
- 1 Mbps maximum bit rate (constrained by propagation delay across the bus)

Modern vehicles may have 100+ ECUs exchanging large sensor arrays, vision data, and OTA
firmware updates. Classical CAN cannot meet these bandwidth requirements.

**CAN FD key improvements (ISO 11898-1:2015):**

**1. Extended payload: up to 64 bytes per frame**

```
Classical CAN DLC: 0-8 bytes
CAN FD DLC encoding:
  DLC 0-8:   0-8 bytes (identical to classical CAN)
  DLC 9:     12 bytes
  DLC 10:    16 bytes
  DLC 11:    20 bytes
  DLC 12:    24 bytes
  DLC 13:    32 bytes
  DLC 14:    48 bytes
  DLC 15:    64 bytes
```

**2. Two-phase bit rate (the "FD" aspect):**

```
Nominal bit rate (arbitration phase):  up to 1 Mbps (same as classical CAN)
Data bit rate (data phase):            up to 8 Mbps (5 Mbps practical with good layout)

Frame:
|SOF| Arbitration | Control |BRS|ESI| Data (0-64 bytes) | CRC | ACK | EOF |
                              ^                             ^
                              BRS (Bit Rate Switch)         end of fast phase
                              marks start of fast data phase
```

The **BRS (Bit Rate Switch)** bit signals the transition to the faster data rate. Transceivers
see this bit and switch their internal sampling to the higher rate. The bus returns to the
nominal rate for the CRC delimiter and ACK.

**3. Improved CRC:**

CAN FD uses a 17-bit CRC for frames with up to 16 bytes, and a 21-bit CRC for larger frames.
Classical CAN's 15-bit CRC has a residual error probability that is unacceptably high for
64-byte payloads.

**New frame fields:**

- **FDF (FD Frame):** replaces the reserved bit; distinguishes CAN FD frames from classical.
- **BRS (Bit Rate Switch):** initiates data phase at higher speed.
- **ESI (Error State Indicator):** transmitter advertises its error state (active/passive).
- **Stuff Count:** added to CRC calculation to improve error detection.

**Hardware requirements for CAN FD:**

- Classical CAN transceivers CANNOT be used at FD data rates > 1 Mbps; they lack the
  required signal edge rates (ISO 11898-2:2016 defines FD-capable transceiver specs).
- The bus stub length and termination requirements are much stricter at 5+ Mbps.
- All nodes on a CAN FD bus must support CAN FD — a single classical CAN node on the bus
  will cause errors when it sees an FDF bit (it interprets the new fields incorrectly).

**Bandwidth comparison:**

```
Classical CAN at 500 kbps, 8 bytes:
  Frame = ~111 bits -> throughput = 8 * 500000 / 111 = ~36 kbps effective data rate

CAN FD at 500kbps arbitration / 2Mbps data, 64 bytes:
  Arbitration phase (nominal): ~50 bits at 500 kbps = 100 µs
  Data phase (fast): ~64 bytes * 8 bits / 2 Mbps = 256 µs
  Total per frame: ~400 µs -> 64 bytes / 400 µs = ~160 kbps effective
  ~4.4x improvement for large payloads
```

---

### Q9. How would you design a CAN message scheduling scheme for a time-sensitive control system?

**Question:** An automotive body control module must transmit 12 CAN messages at different
periodicities. Two messages are safety-critical with 5 ms deadlines. Design a scheduling
scheme that guarantees deadline satisfaction.

**Answer:**

**Problem analysis:**

Classical CAN scheduling is event-driven with priority-based arbitration. Lower-ID messages
preempt higher-ID messages. A naive design with all messages at fixed periods will suffer
from priority inversion when multiple low-priority messages are pending simultaneously.

**Step 1: Assign IDs by priority**

```
Message             Period    Deadline   Assigned ID
Safety_Brake_Cmd    5 ms      5 ms       0x001  (highest priority)
Safety_Steer_Cmd    5 ms      5 ms       0x002
Engine_Status       10 ms     10 ms      0x100
Throttle_Position   10 ms     10 ms      0x101
Wheel_Speed_FL      20 ms     20 ms      0x200
Wheel_Speed_FR      20 ms     20 ms      0x201
Wheel_Speed_RL      20 ms     20 ms      0x202
Wheel_Speed_RR      20 ms     20 ms      0x203
Body_Lights_Status  100 ms    100 ms     0x400
Door_Status         100 ms    100 ms     0x401
HVAC_Status         200 ms    200 ms     0x600
Diag_Heartbeat      1000 ms   1000 ms    0x700
```

**Step 2: Worst-case response time analysis**

For a message m with period Pm and priority p, the worst-case response time R_m includes:
- Its own transmission time (q_m)
- The blocking time from any one lower-priority message that started before m became ready
  (a single low-priority frame can delay m by up to its own transmission time)
- All higher-priority messages that become ready while m waits

```
R_m = B_m + q_m + sum_over_higher_priority_j( ceil(R_m / Pj) * qj )

Where q = frame transmission time = ~220 µs at 500 kbps for 8-byte frame
      B = maximum blocking from lower-priority frame = 220 µs
```

For Safety_Brake_Cmd (0x001, highest priority):
```
R = B + q = 220 + 220 = 440 µs << 5 ms deadline (easily met)
```

For Engine_Status (0x100):
```
R0 = B + q_0x100 + ceil(R0/5ms)*q_0x001 + ceil(R0/5ms)*q_0x002
   = 220 + 220 + 1*220 + 1*220 = 880 µs
R1 = 220 + 220 + ceil(880/5000)*220 + ceil(880/5000)*220 = 880 µs (converged)
880 µs << 10 ms deadline (met)
```

**Step 3: Avoid transmission bursts**

Offset the transmit times of messages with the same period to spread bus load:

```c
/* Offset scheduling: stagger same-period messages */
/* At T=0:   transmit Wheel_Speed_FL */
/* At T=5ms: transmit Wheel_Speed_FR */
/* At T=10ms: transmit Wheel_Speed_RL */
/* At T=15ms: transmit Wheel_Speed_RR */
/* (20ms period, offset by 5ms each) */

typedef struct {
    uint32_t   can_id;
    uint32_t   period_ms;
    uint32_t   offset_ms;   /* initial offset to spread bursts */
    uint32_t   next_tx_ms;  /* next scheduled transmission time */
    uint8_t    data[8];
    uint8_t    dlc;
} CanMessage;

void can_scheduler_tick_1ms(CanMessage *msgs, uint32_t count, uint32_t now_ms)
{
    for (uint32_t i = 0; i < count; i++) {
        if (now_ms >= msgs[i].next_tx_ms) {
            can_transmit(&msgs[i]);                    /* enqueue for HW TX mailbox */
            msgs[i].next_tx_ms = now_ms + msgs[i].period_ms;
        }
    }
}
```

**Step 4: Monitor for missed deadlines**

```c
void can_check_deadline(CanMessage *msg, uint32_t now_ms)
{
    if (now_ms > msg->next_tx_ms + msg->period_ms) {
        /* Message missed its deadline — log and escalate */
        fault_log(FAULT_CAN_DEADLINE_MISSED, msg->can_id,
                  now_ms - msg->next_tx_ms);
        if (msg->can_id <= 0x010) {
            /* Safety-critical message missed deadline — trigger safe state */
            enter_safe_state(SAFE_STATE_CAN_FAULT);
        }
    }
}
```

---

### Q10. How do you implement a CAN bootloader for remote firmware update?

**Question:** Describe the protocol design for a CAN-based firmware update (bootloader).
What are the minimum message types required? How do you handle CRC verification and
partial flashes?

**Answer:**

**Protocol design principles:**

A CAN bootloader must be resilient to: message loss, bus-off events, power loss mid-flash,
and CAN retransmission timeouts. It cannot use TCP-like streams — each CAN message is
independent.

**Minimum message set:**

```
Message ID  | Direction        | Purpose
------------|------------------|------------------------------------------------
0x7DF       | Tester -> ECU    | UDS Broadcast (ISO 15765-2 / ISO 14229 base)
ECU_ID      | ECU -> Tester    | UDS Response
0x700       | Tester -> ECU    | Bootloader: Enter Bootloader Request
0x701       | ECU -> Tester    | Bootloader: ACK / NACK / Status
0x702       | Tester -> ECU    | Bootloader: Data block (sequence + 7 bytes payload)
0x703       | ECU -> Tester    | Bootloader: Block ACK (sequence number confirmed)
0x704       | Tester -> ECU    | Bootloader: End of Image + CRC32
0x705       | ECU -> Tester    | Bootloader: Flash result (OK / CRC_FAIL / ERASE_FAIL)
```

**Sequence diagram:**

```
Tester                                        ECU
  |                                             |
  |---- 0x700: Enter Bootloader Request ------->|
  |<--- 0x701: ACK (bootloader active) ---------|
  |                                             |
  |---- 0x700: Erase Flash (addr, size) ------->|
  |<--- 0x701: NACK_BUSY (erase in progress) ---|
  |---- 0x700: Status Poll -------------------->|
  |<--- 0x701: ACK (erase complete) ------------|
  |                                             |
  | (loop: send all 7-byte blocks)              |
  |---- 0x702: [seq=0x0001] [7 bytes data] ---->|
  |<--- 0x703: [seq=0x0001 ACK] ---------------|
  |---- 0x702: [seq=0x0002] [7 bytes data] ---->|
  |<--- 0x703: [seq=0x0002 ACK] ---------------|
  | ... N blocks ...                            |
  |                                             |
  |---- 0x704: End + CRC32 of image ----------->|
  |                                             | (ECU verifies CRC of received image)
  |<--- 0x705: FLASH_OK (or CRC_FAIL) ----------|
  |                                             |
  |---- 0x700: Reset and Boot New Image ------->|
```

**Block sequence handling (retransmit on missing ACK):**

```c
#define BLOCK_SIZE      7       /* bytes per CAN frame (1 byte for seq MSB, 7 data) */
#define MAX_RETRIES     3
#define ACK_TIMEOUT_MS  100

typedef enum {
    BL_OK        = 0x00,
    BL_ACK       = 0x01,
    BL_NACK_BUSY = 0x02,
    BL_NACK_SEQ  = 0x03,   /* sequence number mismatch */
    BL_NACK_CRC  = 0x04,
} BlStatus;

int can_bl_send_block(uint16_t seq, const uint8_t *data, uint8_t len)
{
    uint8_t frame[8];
    frame[0] = (seq >> 8) & 0xFF;   /* sequence high byte */
    frame[1] = (seq     ) & 0xFF;   /* sequence low byte  */
    /* Actually for 8-byte CAN: use 2 bytes for seq, 6 for data */
    memcpy(&frame[2], data, len > 6 ? 6 : len);

    for (int attempt = 0; attempt < MAX_RETRIES; attempt++) {
        can_transmit_frame(0x702, frame, 8);

        /* Wait for ACK */
        CanFrame ack;
        if (can_wait_frame(0x703, &ack, ACK_TIMEOUT_MS)) {
            uint16_t acked_seq = ((uint16_t)ack.data[0] << 8) | ack.data[1];
            if (acked_seq == seq) return 0;   /* success */
            if (ack.data[2] == BL_NACK_SEQ) {
                /* Sequence error — ECU missed a block */
                /* Tester should restart from the missed sequence */
                return -1;
            }
        }
        /* Timeout — retransmit */
    }
    return -2;   /* max retries exceeded */
}
```

**CRC verification on the ECU (flash side):**

```c
int bootloader_verify_and_program(void)
{
    /* After all blocks received, verify CRC32 of image in RAM buffer */
    uint32_t computed_crc = crc32_calculate(image_buffer, image_size);
    uint32_t received_crc = last_end_frame.crc32;

    if (computed_crc != received_crc) {
        can_send_status(0x705, BL_NACK_CRC);
        /* Do NOT program flash — keep old firmware intact */
        return -1;
    }

    /* CRC matches — program flash page by page */
    if (flash_erase_region(APP_FLASH_ADDR, image_size) != FLASH_OK) {
        can_send_status(0x705, BL_NACK_ERASE);
        return -1;
    }

    if (flash_program_buffer(APP_FLASH_ADDR, image_buffer, image_size) != FLASH_OK) {
        can_send_status(0x705, BL_NACK_PROGRAM);
        /* Flash is now in an indeterminate state — ECU cannot boot app */
        /* Must stay in bootloader and await re-flash */
        stay_in_bootloader = true;
        return -1;
    }

    can_send_status(0x705, BL_OK);
    return 0;
}
```

---

## Summary Reference Table

| Feature | Classical CAN | CAN FD |
|---------|--------------|--------|
| Standard | ISO 11898 / Bosch CAN 2.0 | ISO 11898-1:2015 |
| Max payload | 8 bytes | 64 bytes |
| Nominal bit rate | Up to 1 Mbps | Up to 1 Mbps |
| Data bit rate | Same as nominal | Up to 8 Mbps |
| CRC | 15-bit | 17-bit (≤16B) or 21-bit (>16B) |
| Error detection | Bit, stuff, CRC, form, ACK | All classical + improved CRC |
| Backwards compatible | — | FDF bit distinguishes frames |
| Error counters | TEC, REC (0-255) | Same |
| Bus-off threshold | TEC >= 256 | Same |
| Typical automotive use | Body, chassis networks | ADAS, gateway, OTA |
| Identifier length | 11-bit (2.0A) or 29-bit (2.0B) | Same |
| Arbitration | Non-destructive, lower ID wins | Same |
