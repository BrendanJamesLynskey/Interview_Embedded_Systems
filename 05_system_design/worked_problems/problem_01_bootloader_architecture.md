# Problem 01: Bootloader Architecture Design

## Problem Statement

You are designing the bootloader for a new industrial IoT gateway. The system specification is:

| Parameter | Value |
|---|---|
| MCU | ARM Cortex-M33 at 120 MHz |
| Flash | 2 MB NOR (internal), 16 MB QSPI NOR (external) |
| RAM | 512 KB SRAM |
| Connectivity | LTE-M modem via UART, Ethernet via SPI |
| Power supply | Mains-powered, no battery backup |
| Target market | Industrial process monitoring (IEC 62061 SIL 1) |
| OTA requirement | Devices deployed in remote unmanned facilities; physical access for recovery is expensive (>8 hours travel) |
| Security requirement | Firmware must be signed; only images signed by the manufacturer's root key are accepted |
| Bootloader flash budget | Max 64 KB in internal flash |
| Boot time SLA | System must be operational within 3 seconds of power-on |

**Tasks:**

1. Design the flash memory layout for the complete system.
2. Define the image header structure.
3. Describe the bootloader state machine in sufficient detail to implement it.
4. Design the validation pipeline: specify which checks are performed, in what order, and what action is taken on each failure.
5. Analyse whether the 3-second boot time SLA can be met. Show your working.
6. Identify three specific risks in this design and propose mitigations for each.

---

## Design Requirements

- The bootloader must never be updatable via OTA (write-protected after manufacturing).
- If both update slots are corrupted, the device must not permanently brick — it must enter a recovery mode reachable over Ethernet.
- Rollback attacks must be prevented.
- The bootloader must fit within 64 KB of internal flash.

---

## Solution

### Step 1: Flash Memory Layout

**Internal flash (2 MB at 0x0800_0000):**

The Cortex-M33 boot ROM will load from internal flash. The bootloader, most security-sensitive, lives here in a write-protected region. The remainder of the 2 MB internal flash is used for Slot A.

```
Internal Flash (2 MB):
+-----------------------------------+  0x0800_0000
|  Bootloader (48 KB)               |  Code + read-only data
|  Bootloader Config Page (4 KB)    |  Write-once at manufacturing (pubkey, HW ID)
|  Boot Descriptor (NVS) (4 KB)     |  Written frequently — preferred slot, counters
|  Reserved (8 KB)                  |  Future bootloader expansion
+-----------------------------------+  0x0801_0000  (64 KB from base)
|  Slot A — Application Image       |
|  Image Header (256 bytes)         |
|  Application Code + Data          |
|  (1984 KB = ~1.94 MB)             |
+-----------------------------------+  0x0810_0000  (2 MB from base = end of internal)
```

**External QSPI flash (16 MB at 0x9000_0000 — memory-mapped mode):**

```
External QSPI Flash (16 MB):
+-----------------------------------+  0x9000_0000
|  Slot B — Application Image       |  (2 MB — matches Slot A size)
|  Image Header (256 bytes)         |
|  Application Code + Data          |
+-----------------------------------+  0x9020_0000
|  Key Store (4 KB)                 |  Root public key + revocation list
|  (Write-protected by QSPI WP pin) |
+-----------------------------------+  0x9020_1000
|  Filesystem / Application Data    |  (remaining ~13.99 MB)
|  Log storage, configuration,      |
|  OTA download scratch space       |
+-----------------------------------+  0x9100_0000  (16 MB end)
```

**Rationale for split layout:**

- Bootloader in internal flash: accessible at reset without QSPI initialisation. QSPI controller requires driver code before the QSPI bus is usable.
- Slot A in internal flash: fast XIP execution (no QSPI latency) for the production application. Slot A is the primary slot after manufacturing.
- Slot B in external QSPI: sufficient for OTA candidate image during download. After a confirmed boot from Slot B, on next update the OTA manager writes to Slot A (internal) and boots from it, keeping the faster slot as primary when possible. Alternatively: always run from Slot A, always update Slot B.

**Write protection plan:**

| Region | Protection mechanism |
|---|---|
| Bootloader (0x0800_0000 – 0x0800_FFFF) | FLASH_WRP (write protection) option bytes; programmed during manufacturing test |
| Bootloader Config Page | Programmed once at manufacturing; WRP covers this sector |
| Key Store (QSPI) | QSPI hardware WP signal driven high by MCU GPIO; software cannot clear this after boot |

---

### Step 2: Image Header Structure

```c
/* image_header.h
 * All multi-byte fields are little-endian (Cortex-M33 native byte order).
 * Header occupies the first 256 bytes of each slot.
 * The application's vector table begins at slot_base + 256. */

#define IMAGE_MAGIC          0x494F5447UL   /* "IOTG" in ASCII */
#define IMAGE_HDR_VERSION    1U
#define IMAGE_HDR_SIZE       256U

/* Flags field bit definitions */
#define IMAGE_FLAG_TEST_BUILD   (1U << 0)   /* test/development image; not for production */
#define IMAGE_FLAG_ENCRYPTED    (1U << 1)   /* payload is AES-256-GCM encrypted */
#define IMAGE_FLAG_ROLLBACK_OK  (1U << 2)   /* set by manufacturer to allow deliberate downgrade */

typedef struct __attribute__((packed)) {
    /* --- Identity (bytes 0-15) --- */
    uint32_t  magic;             /* must equal IMAGE_MAGIC                            */
    uint8_t   hdr_version;       /* image header format version (currently 1)         */
    uint8_t   hdr_size;          /* header size in bytes / 4 (currently 64 = 256 B)  */
    uint16_t  flags;             /* IMAGE_FLAG_* bitmask                              */

    /* --- Version and size (bytes 16-31) --- */
    uint32_t  fw_version;        /* packed: [31:24]=major [23:16]=minor [15:0]=patch  */
    uint32_t  sequence_num;      /* monotonically increasing per signing event        */
    uint32_t  image_size;        /* byte count of payload (excludes this header)      */
    uint32_t  vector_table_off;  /* offset from slot_base to vector table (= hdr_size)*/

    /* --- Integrity (bytes 32-67) --- */
    uint32_t  crc32;             /* CRC-32/MPEG-2 of image_size bytes of payload      */
    uint8_t   sha256[32];        /* SHA-256 of image_size bytes of payload            */

    /* --- Authenticity (bytes 68-131) --- */
    uint8_t   signature[64];     /* ECDSA-P256 signature over sha256 (r||s, 32B each) */
    uint8_t   pubkey_id[4];      /* identifies which public key verifies this image   */

    /* --- Hardware compatibility (bytes 136-143) --- */
    uint32_t  hw_id;             /* must match SYSCTRL->HW_ID register in bootloader  */
    uint32_t  min_bootloader_ver;/* this image requires bootloader >= this version    */

    /* --- Padding (bytes 144-255) --- */
    uint8_t   reserved[112];     /* zero-padded; future fields must be backward compatible */
} image_header_t;

_Static_assert(sizeof(image_header_t) == IMAGE_HDR_SIZE,
               "image_header_t must be exactly 256 bytes");
```

---

### Step 3: Bootloader State Machine

```
                          Power-On Reset
                               |
                               v
                    +-----------------------+
                    |   Hardware Init       |
                    |  - Flash controller   |
                    |  - QSPI controller    |
                    |  - IWDG start (8 s)   |
                    |  - Clocks to 120 MHz  |
                    +-----------+-----------+
                                |
                                v
                    +-----------------------+
                    |  Read Boot Descriptor |
                    |  Verify descriptor CRC|
                    |  If CRC fails: use    |
                    |  safe defaults        |
                    +-----------+-----------+
                                |
                    +-----------v-----------+
                    |  Hardware ID check    |  <-- verify flash image is for this board
                    |  Read SYSCTRL->HW_ID  |
                    +-----------+-----------+
                                |
                     +----------+----------+
                     |                     |
                     v                     v
             Validate Slot A         Validate Slot B
             (internal flash)        (QSPI flash)
                     |                     |
                     +----------+----------+
                                |
                                v
                    +-----------------------+
                    |    Image Selection    |
                    |                       |
                    |  preferred_slot valid?|
                    |   YES -> use it       |
                    |   NO  -> use other    |
                    |  Both invalid?        |
                    |   -> Recovery Mode    |
                    +-----------+-----------+
                                |
                    +-----------v-----------+
                    |  Update Boot State    |
                    |  - attempt_count++    |
                    |  - if count > 3:      |
                    |    revert preferred   |
                    |    to last_good_slot  |
                    |  - Write to NVS       |
                    +-----------+-----------+
                                |
                    +-----------v-----------+
                    |   Pre-Jump Checks     |
                    |  - min bootloader ver |
                    |  - HW ID match        |
                    |  - Rollback check vs  |
                    |    OTP sequence num   |
                    +-----------+-----------+
                                |
                    +-----------v-----------+
                    |     Jump to App       |
                    |  - Disable interrupts |
                    |  - Set VTOR           |
                    |  - Load app SP        |
                    |  - DSB + ISB          |
                    |  - Call reset handler |
                    +-----------+-----------+

Recovery Mode (both slots invalid or explicit request):
  - Enable Ethernet via SPI at minimum configuration
  - Advertise recovery IP address on local network
  - Accept firmware download via HTTPS (server cert pinned)
  - Write validated image to Slot A
  - Reset
```

---

### Step 4: Validation Pipeline

Each slot is validated through a sequence of checks. The sequence is ordered from fastest/cheapest to slowest/most expensive to give early exits for obviously corrupted slots:

```
Slot validation: input = slot_base_addr, hw_id
Returns: VALID, INVALID_SILENT (bad data), INVALID_SECURITY (signature fail)

Step  Check                    Failure action          Time (estimate)
----  -----------------------  ----------------------  ---------------
1     Read first 256 bytes     INVALID_SILENT          ~0.1 ms (flash read)
      (image header)

2     magic == IMAGE_MAGIC     INVALID_SILENT          <1 µs
      (detects erased/empty slot: magic = 0xFFFFFFFF)

3     image_size > 0 AND        INVALID_SILENT          <1 µs
      image_size <= MAX_SIZE
      (MAX_SIZE = 1,984 KB for Slot A; 2,048 KB for Slot B)

4     hw_id == SYSCTRL->HW_ID  INVALID_SILENT          <1 µs
      (rejects images built for different hardware)

5     min_bootloader_ver <=    INVALID_SILENT          <1 µs
      BOOTLOADER_VERSION
      (rejects images that require a newer bootloader)

6     CRC-32 over payload       INVALID_SILENT          ~12 ms SW, ~2 ms HW CRC
      (fast integrity check)

7     SHA-256 over payload      INVALID_SILENT          ~42 ms SW (no HW accel)
      (required input for ECDSA)

8     ECDSA-P256 verify         INVALID_SECURITY        ~800-1200 ms SW (!)
      (image authenticity)      Log security event
                                to secure audit log

9     sequence_num >= OTP       INVALID_SECURITY        <1 ms (OTP read)
      minimum sequence          Log rollback attempt
      (rollback prevention)

10    flags check:              INVALID_SILENT          <1 µs
      if production build
      and IMAGE_FLAG_TEST_BUILD is set: reject
      (prevents dev images in production)
```

**Critical observation:** Step 8 (ECDSA-P256) dominates validation time on software-only implementations. The Cortex-M33 at 120 MHz running tinycrypt will take approximately 600–900 ms per slot verification. With two slots to check in the worst case (both valid; we check both to select the best), this adds up to 1.2–1.8 seconds for ECDSA alone.

This is addressed in the boot time analysis below.

**Action on validation failure:**

- `INVALID_SILENT`: mark slot as invalid for this boot cycle; try the other slot.
- `INVALID_SECURITY`: log a security event to the tamper log (written to write-once NVS area); mark slot as invalid; if the same slot has failed security validation 3 times, write a "permanently invalid" flag so the OTA manager knows this slot needs to be re-downloaded.

---

### Step 5: Boot Time SLA Analysis

**SLA: 3 seconds from power-on to operational.**

**Time budget breakdown:**

```
Phase                                  Duration (estimate)
-------------------------------------  -------------------
Hardware init (clocks, flash, QSPI)    80 ms
Read boot descriptor from NVS          1 ms
Validate Slot A or Slot B:
  Header read + magic/size checks      1 ms
  CRC-32 (1.94 MB, SW, M33@120 MHz)   ~25 ms  (SW: ~13 cycles/byte; 2MB*13/120M)
  SHA-256 (1.94 MB, SW, M33@120 MHz)  ~65 ms  (SW: ~4 MB/s on M33 = 500 ms/MB)
  ECDSA-P256 verify (tinycrypt)        ~500 ms (M33@120 MHz is faster than M4@80 MHz)
Update boot state (write NVS)          5 ms   (flash page write)
Jump preparation and vector relocation 0.1 ms
Application init (HAL, RTOS, drivers)  ~500 ms (application code, not bootloader)
-------------------------------------
Sub-total (single slot validation):    ~677 ms
Application init:                      ~500 ms
Total:                                 ~1.18 seconds
```

**Conclusion: The 3-second SLA is achievable** for normal operation (single slot validation, application starts cleanly).

**Worst case (both slots need validation, second slot falls back):**

```
Validate Slot A (full validation): 677 ms
Validate Slot B (full validation): 677 ms  [if Slot A fails after CRC but before boot]
Application init:                  500 ms
Total worst case:                  1.85 seconds
```

Still within the 3-second SLA. Margin: ~1.15 seconds.

**Risk: if ECDSA is slower than estimated.** On a Cortex-M33 with some crypto extensions, tinycrypt P256 verify has been measured at 500–800 ms at 120 MHz depending on specific implementation. In the pessimistic case (800 ms):

```
Worst case (two ECDSA verifications):
  800 ms + 800 ms + 200 ms (rest) + 500 ms (app init) = 2.3 seconds
```

This still meets the SLA. However, to ensure margin:
- Use `mbedTLS` with Cortex-M hardware acceleration intrinsics, which can achieve ~200 ms for ECDSA-P256 on M33 with DSP extension enabled.
- Alternatively: use EdDSA (Ed25519) with an optimised Curve25519 implementation, which runs in ~100 ms on M33 at 120 MHz.

---

### Step 6: Risks and Mitigations

**Risk 1: QSPI flash unavailable at boot (bus fault, uninitialised QSPI controller)**

Description: Slot B resides on external QSPI flash. If the QSPI controller fails to initialise (hardware fault, corrupted QSPI registers, ESD damage to QSPI bus pins), the bootloader cannot read or validate Slot B. If Slot A is simultaneously invalid (e.g., first boot after factory programming), the device is stuck.

Mitigation:
- The bootloader must handle QSPI init failure as a non-fatal error: if QSPI is unavailable, mark Slot B as inaccessible and proceed with Slot A only.
- At manufacturing, program and validate Slot A before shipping. Slot A must always hold a factory image. Slot A is only erased by a deliberate OTA update — never during initial programming.
- Add a hardware watchdog external to the MCU (e.g., MAX16054) that resets the board if the MCU does not drive a heartbeat GPIO within 10 seconds. This handles complete MCU lockup during QSPI init.

**Risk 2: Boot descriptor NVS corruption leads to boot loop**

Description: The boot descriptor tracks preferred_slot and attempt_count. If the NVS sector is corrupted (power loss during write, flash wear-out), the bootloader may read garbage values: attempt_count = 0xFFFFFFFF (exceeds max_attempts immediately), preferred_slot = 0xDEADBEEF (invalid). The bootloader could enter recovery mode on every boot even though both slots hold valid images.

Mitigation:
- Protect the boot descriptor with a CRC. If the CRC fails, reconstruct safe defaults: preferred_slot = 0 (Slot A), attempt_count = 0, confirmed = false.
- Implement a tiered NVS: two copies of the boot descriptor in separate 4 KB sectors. Write the new copy, verify it, then mark the old copy obsolete. The bootloader reads the most recently written valid copy. This provides power-loss protection.
- Log NVS corruption events to a secondary log sector to aid field diagnostics.

**Risk 3: Recovery mode is the only fallback, but network connectivity may not be available at the remote site**

Description: If both slots are corrupted, the device enters recovery mode and waits for an Ethernet-based firmware download. In a remote unmanned facility, there may be no person to initiate the download, or the local network may also be down (dependent on the same facility power that failed).

Mitigation:
- Introduce a factory image in a third, permanently write-protected flash region. The factory image is minimal (connects to LTE, downloads latest firmware, installs it). This provides a recovery path even without Ethernet.
- The OTA server should monitor device health telemetry. If a device stops reporting, an automated process can pre-stage a firmware image in a cloud bucket and the device can retrieve it when it next connects.
- Design the recovery mode to also accept firmware via the LTE-M modem, not only Ethernet. Implement fallback priority: try Ethernet first, then LTE-M, then wait for physical access.

---

## Key Takeaways

1. **Flash layout must be designed for the worst-case scenario first.** The most important question is not "where does the application live?" but "what happens when everything goes wrong?" Design the recovery path before the happy path.

2. **ECDSA verification dominates boot time on software-only MCUs.** Profile this early in the design phase. If the SLA is tight, choose a MCU with a hardware PKA (Public Key Accelerator) or plan to use Ed25519 instead of ECDSA-P256.

3. **Write-protect the bootloader at manufacturing, not at first boot.** A "one-time lock" triggered by software on first boot is vulnerable to a firmware bug or attacker that prevents first boot from occurring, leaving the bootloader unlocked in production devices.

4. **Every flash region that is written at runtime must be protected against power-loss corruption.** The boot descriptor, key store, and NVS all require CRC protection and ideally write-with-verify or dual-copy journaling.

5. **The recovery mode is itself a security boundary.** An unauthenticated recovery mode that accepts any firmware image is a factory-reset-to-arbitrary-firmware exploit. Recovery mode must also validate image signatures using the same root key as the normal boot path.
