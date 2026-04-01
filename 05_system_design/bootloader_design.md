# Bootloader Design

## Prerequisites
- ARM Cortex-M startup sequence and vector table layout
- Flash memory architecture: sectors, pages, erase/program operations
- Basic cryptographic concepts: hash functions, digital signatures
- C pointer arithmetic and memory-mapped I/O

---

## Concept Reference

### What a Bootloader Does

A bootloader is the first code that executes after reset. It runs before the application and is responsible for:

1. Validating that a runnable application image exists in flash.
2. Selecting which image to run when multiple images are present (dual-bank OTA).
3. Initialising minimum hardware required for its own operation (clocks, flash controller, optionally UART or USB for recovery).
4. Jumping to the application entry point by loading the application's stack pointer and reset handler address from the application's vector table.

A bootloader is distinguished from a startup file (crt0/startup.s) which is part of the application itself and runs after the bootloader has transferred control.

### Memory Layout for Dual-Bank OTA

Dual-bank (A/B) partitioning stores two complete firmware images in flash so that a new image can be written while the current image runs, and the bootloader selects the valid image on reset:

```
Flash address space (example: 1 MB NOR flash, 0x0800_0000 base on STM32):

+-------------------+  0x0800_0000
|    Bootloader     |  (32 KB — protected, never overwritten by OTA)
|    + Config page  |  (last 4 KB of bootloader region: boot descriptor)
+-------------------+  0x0800_8000
|    Slot A         |  (480 KB — active image or candidate image)
|    Image Header   |  (first 256 bytes: magic, version, CRC, size, signature)
|    Vector Table   |  (follows header, or header IS placed before flash base)
|    Application    |
+-------------------+  0x0808_8000
|    Slot B         |  (480 KB — other image)
|    Image Header   |
|    Vector Table   |
|    Application    |
+-------------------+  0x0810_8000
|    Scratch / NVS  |  (8 KB — non-volatile storage for boot counters, keys)
+-------------------+  0x0810_A000  [flash end]

Notes:
  - Bootloader region is write-protected via flash option bytes (PCROP on STM32,
    Secure Boot on nRF5340, TrustZone SAU on Cortex-M33).
  - Both slots are identical in size and layout; the bootloader does not need to
    know which slot is "A" vs "B" — it evaluates each slot independently.
  - The image header is at a fixed offset (slot base + 0) so the bootloader can
    find it without parsing application code.
```

### Image Header Structure

```c
/* image_header.h — placed at the very start of each flash slot */

#define IMAGE_MAGIC          0x96F3B83DUL   /* 32-bit magic number */
#define IMAGE_HDR_SIZE       256U            /* header always occupies 256 bytes */
#define IMAGE_VERSION_MAJOR(v) ((v) >> 24)
#define IMAGE_VERSION_MINOR(v) (((v) >> 16) & 0xFF)
#define IMAGE_VERSION_PATCH(v) ((v) & 0xFFFFU)

typedef struct {
    uint32_t  magic;            /* IMAGE_MAGIC — must match for header to be valid   */
    uint32_t  image_size;       /* byte count from end of header to end of image     */
    uint32_t  version;          /* packed: [31:24]=major [23:16]=minor [15:0]=patch  */
    uint32_t  crc32;            /* CRC-32/MPEG-2 of image_size bytes after header    */
    uint8_t   sha256[32];       /* SHA-256 of the same image payload                 */
    uint8_t   signature[64];    /* ECDSA-P256 signature over sha256 field            */
    uint8_t   pubkey_id[8];     /* identifies which root public key to use           */
    uint32_t  flags;            /* bit 0: test image; bit 1: encryption used         */
    uint32_t  sequence_num;     /* monotonically increasing — rollback prevention    */
    uint8_t   reserved[128];    /* pad to IMAGE_HDR_SIZE = 256                       */
} __attribute__((packed)) image_header_t;
```

### Bootloader State Machine

```
Power-on / Reset
      |
      v
[Hardware Init]         -- minimal: flash controller, clocks, watchdog start
      |
      v
[Read Boot Descriptor]  -- NVS page: preferred_slot, boot_attempt_count, last_good
      |
      v
[Validate Slot A]       -- magic check -> size bounds -> CRC-32 -> SHA-256 -> signature
      |
      v
[Validate Slot B]       -- same sequence
      |
      v
[Select Image]          -- rule: prefer preferred_slot if valid; fallback to other;
      |                    if neither valid: enter recovery mode (UART/USB download)
      v
[Update Boot State]     -- increment attempt counter; if > threshold: revert preferred
      |
      v
[Prepare to Jump]       -- disable interrupts, uninit bootloader peripherals,
      |                    set VTOR to application vector table address
      v
[Jump to Application]   -- load SP from app[0], jump to app[1] (reset handler)
```

### CRC-32 Validation

CRC-32 is a fast integrity check that detects accidental corruption (bit flips, incomplete writes). It is NOT a security mechanism — an attacker can forge a correct CRC. Security requires cryptographic signature verification.

```c
/* crc32.c — CRC-32/MPEG-2 (polynomial 0x04C11DB7, initial value 0xFFFFFFFF,
 * no final XOR, no input/output reflection).
 * This matches the hardware CRC unit on STM32 devices (CRC->CR default). */

#include <stdint.h>
#include <stddef.h>

/* Generate a 256-entry lookup table at compile time (C99 compound literals).
 * Each entry is the CRC of a single byte 0x00..0xFF. */
static const uint32_t crc32_table[256] = {
    /* Pre-computed for CRC-32/MPEG-2, included here abbreviated.
     * In production, generate with a script and place in ROM. */
    0x00000000, 0x04C11DB7, 0x09823B6E, 0x0D4326D9,
    /* ... 252 more entries ... */
};

uint32_t crc32_compute(const uint8_t *data, size_t length)
{
    uint32_t crc = 0xFFFFFFFFUL;   /* MPEG-2 initial value — no input reflection */

    while (length--) {
        /* Table-driven: process one byte per iteration */
        uint8_t index = (uint8_t)((crc >> 24) ^ *data++);
        crc = (crc << 8) ^ crc32_table[index];
    }

    return crc;   /* MPEG-2: no final XOR inversion */
}

/* Verify an image slot: returns 1 if CRC matches, 0 on failure */
int image_crc_valid(const image_header_t *hdr)
{
    /* The image payload starts immediately after the 256-byte header */
    const uint8_t *payload = (const uint8_t *)hdr + IMAGE_HDR_SIZE;

    if (hdr->image_size == 0 || hdr->image_size > MAX_IMAGE_SIZE) {
        return 0;   /* reject implausible sizes before computing CRC */
    }

    uint32_t computed = crc32_compute(payload, hdr->image_size);
    return (computed == hdr->crc32) ? 1 : 0;
}
```

### Jumping to the Application

The bootloader must hand off execution to the application correctly. On Cortex-M this means:

1. Setting the vector table offset register (VTOR) to the application's vector table.
2. Loading the application's initial stack pointer (first word of its vector table).
3. Calling the application's reset handler (second word of its vector table).

```c
/* boot_jump.c — platform: ARM Cortex-M */

#include <stdint.h>
#include "cmsis_compiler.h"   /* provides __set_MSP(), __DSB(), __ISB() */

typedef void (*app_entry_t)(void);

void bootloader_jump_to_app(uint32_t app_base_addr)
{
    /* app_base_addr: base of the slot, AFTER the image header.
     * The first word at app_base_addr is the initial stack pointer value.
     * The second word is the reset handler address. */

    uint32_t *vector_table = (uint32_t *)app_base_addr;
    uint32_t  app_sp       = vector_table[0];
    uint32_t  app_reset    = vector_table[1];

    /* Basic sanity: reset handler must be in flash and have bit 0 set (Thumb mode) */
    if ((app_reset & 0x1U) == 0) {
        /* Not a Thumb address — corrupt vector table, do not jump */
        return;
    }

    /* Disable all interrupts before jumping.
     * The application will re-enable what it needs after its own init. */
    __disable_irq();

    /* Relocate the vector table.
     * VTOR (0xE000ED08) must be set BEFORE updating SP, so that any
     * NMI or fault that fires during the jump sequence uses the correct vectors. */
    SCB->VTOR = app_base_addr;

    /* Memory and instruction barriers: ensure VTOR write is visible to the
     * processor before the stack pointer is changed. */
    __DSB();
    __ISB();

    /* Load the application's initial MSP */
    __set_MSP(app_sp);

    /* Construct function pointer to app reset handler and jump.
     * Cast through (uintptr_t) to suppress pointer-from-integer warnings. */
    app_entry_t app_entry = (app_entry_t)(uintptr_t)app_reset;
    app_entry();

    /* Unreachable. If we get here, the application returned — which is
     * a hard fault on Cortex-M because the reset handler stack frame
     * has been destroyed. The watchdog will recover. */
    while (1) { }
}
```

---

## Tier 1 — Fundamentals

### Question F1
**What is the difference between a bootloader and a startup file (e.g., startup.s or crt0.s)?**

**Answer:**

A **startup file** is part of the application. It runs after the bootloader has transferred control and is responsible for:
- Setting up the stack (on some systems).
- Copying initialised variables from flash to RAM (`.data` section copy).
- Zero-initialising the BSS section.
- Calling static C++ constructors.
- Calling `main()`.

A **bootloader** runs before any application code. It is responsible for:
- Selecting which application image to run (in systems with multiple images).
- Validating image integrity before execution.
- Providing a recovery path if no valid image exists.
- Optionally loading or updating images.

**Key distinction:** The startup file trusts that the image it belongs to has already been validated. The bootloader provides that validation. On a simple system with no OTA, a startup file is sufficient. On a product that receives field firmware updates, a bootloader is essential.

**Common mistake:** Candidates describe the linker script or startup file as the "bootloader". The linker script is a build tool artefact — it has no runtime role. The startup file runs after the bootloader.

---

### Question F2
**Why must the bootloader be stored in a write-protected region of flash? What happens if it is overwritten?**

**Answer:**

The bootloader is the root of trust for the entire system. If the bootloader itself can be overwritten by application code (or a malicious update), an attacker or buggy firmware can:
- Replace the bootloader with code that skips signature verification, enabling unsigned firmware to run.
- Brick the device permanently by writing invalid code to the bootloader slot (no recovery path remains).
- Bypass secure boot entirely.

**Protection mechanisms:**

| Platform | Mechanism |
|---|---|
| STM32 | Flash option bytes: PCROP (Proprietary Code Read-Out Protection) marks sectors as execute-only and write-protected |
| nRF5340 | Trusted Firmware-M (TF-M) partition in Secure Processing Environment; SPE flash is inaccessible from NSPE |
| ESP32-S3 | Flash encryption + secure boot V2: bootloader slot verified by ROM and cannot be re-flashed without the private key |
| ARM Cortex-M33 | SAU (Security Attribution Unit) maps bootloader to Secure state; NSPE application cannot execute or write to Secure regions |

**Practical consequence:** Once a product ships, the write protection must be permanently enabled. This is often done by blowing OTP (one-time programmable) fuses during manufacturing test. Leaving this step incomplete is a common supply-chain security failure.

---

### Question F3
**Describe the minimum steps a Cortex-M bootloader must perform before jumping to an application image.**

**Answer:**

```
1. Validate the image (at minimum: magic number check, CRC verification).
   - Do not jump to an image that fails validation — it may be partially written
     or corrupted and will cause unpredictable behaviour.

2. Set VTOR (Vector Table Offset Register, SCB->VTOR at 0xE000ED08) to the
   address of the application's vector table.
   - Without this, fault handlers and interrupts will vector into the bootloader's
     exception table, not the application's.

3. Load the application's initial Main Stack Pointer from vector_table[0].
   - The bootloader runs with its own stack. The application may require a
     different stack location. __set_MSP() must be called.

4. Issue DSB + ISB memory/instruction barriers.
   - Ensures the VTOR write has propagated through the pipeline before the
     first instruction of the application is fetched.

5. Disable interrupts (__disable_irq()).
   - Prevents a peripheral interrupt (e.g., a pending UART RX) from firing
     immediately after the jump and routing to the bootloader's ISR (which
     is no longer valid after VTOR and SP changes).

6. Jump to vector_table[1] (application reset handler address).
```

**What NOT to do:**
- Do not leave the watchdog running with the bootloader's refresh period — the application must configure and kick the watchdog after startup, or it will reset before it completes initialisation.
- Do not leave DMA channels running — they will write to bootloader-managed addresses after control has been transferred.
- Do not leave peripherals in an unconfigured state that prevents the application from initialising them correctly (e.g., a locked I2C bus).

---

### Question F4
**What is a "golden image" or "factory image", and why is it valuable in a bootloader design?**

**Answer:**

A **golden image** (also called factory image or recovery image) is a minimal, known-good firmware version stored in a protected region of flash. It is never overwritten by OTA updates.

**Purpose:** If both OTA update slots become corrupted or hold images that fail to boot, the bootloader falls back to the golden image. This guarantees the device is never permanently bricked and can always return to a recoverable state.

**Typical layout with golden image:**

```
+-------------------+  Flash base
|    Bootloader     |  Write-protected; never updated OTA
+-------------------+
|    Golden Image   |  Write-protected; holds the factory firmware
|    (Slot 0)       |  Bootloader falls back here if all update slots fail
+-------------------+
|    Update Slot A  |  Updatable by OTA
+-------------------+
|    Update Slot B  |  Updatable by OTA
+-------------------+
|    NVS / Config   |  Boot counters, preferred slot, device config
+-------------------+
```

**Trade-off:** The golden image consumes flash. On a device with limited flash, storing a third complete image may be impractical. In that case, a minimal recovery image (containing only enough firmware to connect to an OTA server and download a new image) saves space.

---

## Tier 2 — Intermediate

### Question I1
**Explain dual-bank (A/B) OTA architecture. How does the bootloader select which slot to boot, and how does it handle a failed update?**

**Answer:**

In dual-bank OTA, two complete firmware images coexist in flash simultaneously:

```
State: Normal operation
  Slot A: valid, running (version 1.2.0)
  Slot B: valid, older (version 1.1.0) or empty

OTA update arrives:
  1. Application downloads new firmware into Slot B while running from Slot A.
     Slot A is read-only during download; the device remains fully operational.
  2. Download completes. Application writes the image header to Slot B, including
     CRC, SHA-256, and signature over the new payload.
  3. Application writes to NVS: preferred_slot = B, boot_attempt_count = 0.
  4. Application triggers a system reset.

After reset — bootloader selects slot:
  1. Read NVS: preferred_slot = B.
  2. Validate Slot B: magic -> CRC -> SHA-256 -> ECDSA signature.
  3. If Slot B is valid: boot_attempt_count++, write to NVS, jump to Slot B.
  4. Application boots from Slot B. If it starts successfully and passes
     its self-test, it writes "confirmed = true" to NVS.

Failed update scenario:
  - Slot B boots but crashes before confirming. Watchdog resets the device.
  - Bootloader reads NVS: preferred_slot = B, boot_attempt_count = 1, confirmed = false.
  - Bootloader increments attempt count. If attempt count > MAX_BOOT_ATTEMPTS (e.g., 3):
    mark Slot B as failed, revert preferred_slot to A, reset attempt_count.
  - Next reset: bootloader boots from Slot A (known-good previous version).
```

**Key requirement: confirmation.** The application must actively confirm a successful boot. A bootloader that marks an image "good" simply because it started running cannot distinguish a crash-on-first-use from a working image. Confirmation should happen only after the application has verified its critical subsystems (network connectivity, sensor initialisation, etc.).

**Why not single-bank OTA:** Single-bank OTA erases and rewrites the running firmware slot. If power is lost mid-write, the device is bricked. Dual-bank eliminates this risk: the running slot is never erased during an update.

---

### Question I2
**Walk through the steps a bootloader takes to validate an image using CRC-32 followed by SHA-256. Why are both checks performed? In what order and why?**

**Answer:**

**Step-by-step validation sequence:**

```
1. Read image header from slot base address.
   Check: header.magic == IMAGE_MAGIC
   Fail: slot is empty or unformatted. Stop immediately.

2. Check: header.image_size > 0 && header.image_size <= MAX_SLOT_SIZE
   Fail: size is implausible; the header itself may be corrupted.

3. Compute CRC-32 over header.image_size bytes starting from
   (slot_base + IMAGE_HDR_SIZE).
   Check: computed_crc == header.crc32
   Fail: image data is corrupted (bit flip, incomplete write). Stop.

4. Compute SHA-256 over the same byte range.
   Check: computed_sha256 == header.sha256
   Fail: image data is corrupted more severely than CRC detected.

5. Verify ECDSA-P256 signature:
   - The signature (header.signature) is over header.sha256 using the device's
     root public key identified by header.pubkey_id.
   - Load the corresponding public key from the bootloader's embedded key store.
   - Verify signature(header.sha256, pubkey) using ECDSA-P256.
   Fail: image was not signed by a trusted party. Boot refused.
```

**Why both CRC-32 and SHA-256:**

| Check | Purpose | Why not sufficient alone |
|---|---|---|
| CRC-32 | Fast integrity check (~1 cycle/byte with HW accelerator) | Not collision-resistant; an attacker can forge a message with any desired CRC |
| SHA-256 | Cryptographically secure hash; input to signature | Slow without hardware accelerator; but required for signature verification |
| ECDSA signature | Proves the image was produced by the holder of the private key | Requires SHA-256 as input |

**Why CRC-32 first:** CRC is orders of magnitude faster than SHA-256 on software-only implementations (e.g., 10x faster on Cortex-M4 without a crypto accelerator). Running CRC first provides an early exit for clearly corrupted images (e.g., flash was erased mid-write), avoiding the multi-second SHA-256 computation on garbage data. On hardware with a SHA-256 accelerator, the speed difference narrows and the CRC step may be omitted.

**Common mistake:** Computing SHA-256 and then verifying the CRC over the same range is redundant — SHA-256 detects all corruption CRC does. The order CRC → SHA-256 → signature is optimal for performance, not logical necessity.

---

### Question I3
**What is a boot descriptor (or boot metadata) page, and what information does it need to contain for a robust OTA system?**

**Answer:**

The boot descriptor is a small region of flash (typically one erase sector, 4 KB) that the bootloader reads on every reset to determine its behaviour. It stores persistent state that survives resets without being part of either image slot.

**Contents of a minimal boot descriptor:**

```c
typedef struct {
    uint32_t magic;               /* detects uninitialised/erased NVS           */
    uint32_t preferred_slot;      /* 0 = Slot A, 1 = Slot B                     */
    uint32_t boot_attempt_count;  /* how many consecutive unconfirmed boots      */
    uint32_t max_boot_attempts;   /* threshold before reverting (e.g., 3)       */
    uint32_t last_good_slot;      /* slot that last confirmed successfully       */
    uint32_t image_confirmed;     /* 1 = current slot confirmed OK by app        */
    uint32_t sequence_num[2];     /* monotonically increasing per slot           */
                                  /* for rollback prevention                     */
    uint8_t  padding[4096 - 28]; /* pad to one flash page for atomic erase      */
    uint32_t crc32;               /* CRC of all preceding bytes in this struct   */
} boot_descriptor_t;
```

**Why store a CRC over the boot descriptor itself:** The boot descriptor page is written frequently (on every update attempt, every confirmed boot). If power is lost during a descriptor write, the page may be partially updated. The CRC allows the bootloader to detect this and fall back to safe defaults (boot from last_good_slot).

**Wear levelling consideration:** NOR flash pages have a finite erase/write endurance (typically 10,000–100,000 cycles). A boot descriptor written on every boot will wear out long before the product lifetime. Solutions:
1. Write the descriptor only when it changes (update + confirmation), not on every boot.
2. Use a circular log of descriptor records within the NVS sector, advancing a write pointer to a fresh page slot on each write.
3. Use dedicated EEPROM emulation libraries (e.g., STM32 EEPROM emulation, nRF5 FDS).

---

### Question I4
**How does a bootloader defend against rollback attacks? What is a sequence number and what hardware feature supports permanent rollback prevention?**

**Answer:**

A **rollback attack** downgrades a device to an older firmware version that contains known vulnerabilities that have been patched in newer versions. If the bootloader only checks that an image is signed by a trusted key, an attacker who obtains a valid signed older image can reinstall it.

**Sequence number (monotonic counter) approach:**

```
Each signed image contains a sequence_num field in its header.
The bootloader stores the minimum_acceptable_sequence_num in NVS.

On every confirmed successful boot:
  minimum_acceptable_sequence_num = max(current, image.sequence_num)
  Write updated minimum to NVS.

On validation of a candidate image:
  if image.sequence_num < minimum_acceptable_sequence_num:
      reject image (rollback detected)
```

**Limitation of NVS-stored counters:** NVS can be erased by an attacker with physical access to JTAG or by a malicious firmware that erases the NVS sector before installing an old image. This defeats the software counter.

**Hardware solution: OTP (One-Time Programmable) fuses or monotonic counters:**

| Platform | Mechanism |
|---|---|
| STM32H7 | 96-bit OTP area in Option Bytes; each bit can be programmed 0 once; roll a bit per firmware version |
| nRF9160 | FICR (Factory Information Configuration Register) write-once fields; NSIB (Non-Secure Immutable Bootloader) manages counter |
| ESP32 | eFuse: 32-bit VERSION field, bits programmed 0 cumulatively (bit-blowing cannot be reversed) |
| ARM TrustZone (PSA) | `psa_fwu_set_version()` writes to a TrustZone-protected counter that cannot be decremented from NSPE |

The OTP counter increment is performed by the **bootloader** (or Secure Processing Environment) after confirming a successful boot — not by the application. The application must not have write access to the OTP counter register.

---

## Tier 3 — Advanced

### Question A1
**Design the signature verification flow for a secure bootloader using ECDSA-P256. What data is signed, what is verified, and what are the security requirements on the public key store?**

**Answer:**

**What is signed:** The cryptographic signature covers the SHA-256 hash of the image payload (everything after the image header). The header itself — including the signature field — is not part of the signed data, because the signature cannot sign itself. Some designs sign a canonical header (with the signature field zeroed), but signing only the payload hash is simpler and equally secure.

```
Signing (performed by the build system / release pipeline):

  1. Build the firmware binary (image payload, no header).
  2. Compute SHA-256(payload) -> digest[32 bytes].
  3. Sign: ECDSA-P256(private_key, digest) -> (r, s) each 32 bytes = 64 bytes total.
  4. Construct image header with:
       header.sha256    = digest
       header.signature = r || s  (raw P1363 format, or DER-encoded)
       header.pubkey_id = ID of the public key the verifier should use
  5. Prepend header to payload -> complete firmware image file.

Verification (performed by the bootloader at runtime):

  1. Recompute SHA-256 over payload (header.image_size bytes after the header).
     Compare to header.sha256. If mismatch: reject (data corruption or header tampering).
  2. Load the public key corresponding to header.pubkey_id from the bootloader's
     embedded key store (see below).
  3. Call ECDSA-P256 verify(public_key, header.sha256, header.signature).
     This checks: is the signature a valid proof that the private_key holder
     approved this exact payload?
  4. If verification passes: image is authentic and unmodified. Proceed to boot.
```

**Security requirements on the public key store:**

The public key is embedded in the bootloader binary itself and protected by the bootloader's write-protection. Requirements:

1. **Immutability at runtime:** The key store must be in read-only flash (or ROM). An attacker must not be able to substitute their own public key, which would allow them to sign arbitrary firmware with their own private key and have it accepted.

2. **Key revocation support:** Systems that may need to revoke a compromised signing key require a key store with multiple entries and a revocation mechanism. The simplest approach: embed multiple public keys; a revoked key's ID is recorded in OTP (once written, the ID can never be un-revoked). The bootloader rejects any image whose `pubkey_id` matches a revoked entry.

3. **Key rotation:** Introduce a new public key in the next bootloader version (itself signed with the old key). Once the new bootloader is confirmed across the fleet, revoke the old key via OTP.

4. **Protection against timing attacks:** ECDSA implementations must use constant-time arithmetic. Variable-time implementations leak the private key through side-channel timing measurements. For a bootloader verifying against a fixed public key this is less critical than for a server verifying user keys, but libraries like mbedTLS and libsodium provide constant-time implementations by default.

**Practical library choices for Cortex-M:**
- mbedTLS: widely used, ECDSA-P256 with hardware RNG support, configurable for ROM-size.
- tinycrypt: minimal (~3 KB for P256 ECDSA verify), designed for constrained devices.
- wolfSSL: FIPS 140-2 validated; required for some safety/security standards.

---

### Question A2
**A device has 512 KB of NOR flash, a Cortex-M4 at 80 MHz, and must complete a full bootloader validation (CRC + SHA-256 + ECDSA) within 500 ms of reset to meet a system startup SLA. Walk through the timing analysis and identify the dominant bottleneck.**

**Answer:**

**System parameters:**
- Flash: 512 KB per slot, 32-bit read bus at 80 MHz with 2-wait-state access (practical throughput ~20 MB/s).
- CPU: Cortex-M4 at 80 MHz. No SHA-256 or crypto hardware accelerator.
- Image size: assume a representative 300 KB application image.
- Boot time budget: 500 ms total (hardware init ~50 ms, leaving ~450 ms for validation).

**Step 1: Flash read throughput**

```
Flash read bandwidth (2 wait states, 32-bit bus, 80 MHz):
  Effective clock per 32-bit word = 3 cycles (1 + 2 wait states)
  Words per second = 80,000,000 / 3 = 26.7 Mwords/s = 106.7 MB/s

Read 300 KB image from flash:
  Time = 300 KB / 106.7 MB/s = 307,200 / (106.7 x 1024 x 1024) ≈ 2.7 ms
```

Flash read time is negligible — but it must be counted if the CPU cannot cache the entire image.

**Step 2: CRC-32 (software, table-driven)**

```
Cortex-M4 table-driven CRC-32: approximately 8-12 cycles per byte.
Conservative estimate: 10 cycles/byte.

300 KB at 10 cycles/byte = 307,200 x 10 = 3,072,000 cycles
At 80 MHz: 3,072,000 / 80,000,000 = 38.4 ms

With STM32 hardware CRC unit (32-bit words, 1 cycle each):
  300 KB / 4 bytes = 76,800 words at ~2 cycles each (FIFO + compute) = 153,600 cycles
  At 80 MHz: ~1.9 ms   ← hardware is 20x faster
```

**Step 3: SHA-256 (software)**

```
Cortex-M4 software SHA-256 (mbedTLS, optimised ARM Thumb2):
  Typical throughput: ~5-7 MB/s on Cortex-M4 at equivalent clock.
  At 80 MHz: approximately 6 MB/s.

300 KB at 6 MB/s = 307,200 / (6 x 1024 x 1024) ≈ 48.8 ms

With SHA-256 hardware accelerator (e.g., STM32H7 HASH peripheral):
  DMA-fed, processes 64-byte blocks at ~60 cycles each = ~150 MB/s
  300 KB: ≈ 1.9 ms   ← hardware is ~25x faster
```

**Step 4: ECDSA-P256 verify (software)**

```
Cortex-M4 ECDSA-P256 verify (tinycrypt, no hardware acceleration):
  Dominant operation: two point multiplications on P-256 curve.
  Typical runtime on Cortex-M4 at 80 MHz: 800–1200 ms.

This is the dominant bottleneck by an enormous margin.
```

**Summary:**

| Step | Software | Hardware accelerated |
|---|---|---|
| Flash read (300 KB) | 2.7 ms | 2.7 ms (no HW for this) |
| CRC-32 | 38.4 ms | 1.9 ms |
| SHA-256 | 48.8 ms | 1.9 ms |
| ECDSA-P256 verify | **900 ms (estimate)** | ~5 ms (if HW ECC engine present) |
| **Total** | **~990 ms** | **~12 ms** |

**Conclusion and remediation options:**

Pure software ECDSA on Cortex-M4 at 80 MHz violates the 500 ms SLA by approximately 2x.

Options in order of preference:
1. **Use a hardware cryptographic accelerator.** Many modern Cortex-M4 SoCs (STM32H5, nRF5340, ATSAML11) include a hardware PKA (Public Key Accelerator) that reduces ECDSA verify to under 10 ms.
2. **Switch to EdDSA (Ed25519) with an optimised implementation.** Ed25519 verify on Cortex-M4 with the STROBE/SUPERCOP implementation runs in ~40 ms at 80 MHz due to efficient field arithmetic over Curve25519.
3. **Increase CPU clock during boot.** Boot at maximum clock (e.g., 120 MHz bypassing power optimisation) and reduce clock after jumping to the application.
4. **Accept the constraint and relax the SLA.** If the 500 ms SLA is a soft target, 990 ms may be acceptable for the product.
5. **Cache a "pre-validated" flag in NVS.** After first successful validation, store a flag in NVS. On subsequent boots, skip full ECDSA if CRC passes and flag is set. Reset the flag on any OTA activity. Risk: reduces security to CRC on non-OTA boots.

---

### Question A3
**How does a bootloader interact with a hardware security module (HSM) or TrustZone Secure Enclave for key storage? What threat does this address over embedding keys in flash?**

**Answer:**

**The threat addressed:**

If a signing public key is embedded in plain flash alongside the bootloader binary, an attacker with physical access (via JTAG or by de-packaging the chip) can read the key. For a public key this is not directly harmful (public keys are inherently public), but it enables:
- Identifying which key family is in use and attacking the corresponding private key infrastructure.
- In systems where the bootloader also stores a device-private key (for mutual authentication or attestation), flash-resident key storage exposes the secret.

The more critical concern is **protecting the private key** used for device attestation or decrypting encrypted firmware images. If a device holds a private attestation key in plain flash, that key can be extracted and cloned to impersonate the device.

**TrustZone-based key protection (ARM Cortex-M33 / PSA):**

```
System architecture with TrustZone:

  +-----------------------------+    +-----------------------------+
  |  Secure Processing Env (S)  |    | Non-Secure Processing Env   |
  |                             |    | (NSPE — application)        |
  | - Bootloader                |    |                             |
  | - Trusted Firmware-M (TF-M) |    | - Application firmware      |
  | - Key store (PSA Crypto)    |    | - OTA download manager      |
  | - Crypto operations         |    |                             |
  | - Attestation service       |    |                             |
  +-----------------------------+    +-----------------------------+
            |                                    |
            +------------ Secure/NS boundary ----+
            |
     SAU enforces: NS code cannot read S memory regions
                   NS code cannot execute S code directly
                   NS calls S via NSC (Non-Secure Callable) veneer functions
```

**Key operations in TrustZone bootloader flow:**

1. **Boot:** The TF-M secure bootloader (MCUboot in TF-M configuration) runs entirely in Secure state. The signing public key is stored in Secure flash, inaccessible from NSPE. The application image (NSPE) is validated before NSPE is entered.

2. **Device attestation key:** The device's private key for PSA attestation (`psa_initial_attest_key`) is generated in the secure enclave and never leaves Secure flash. The NSPE application can request an attestation token via a veneer call, but the private key is never exposed.

3. **Encrypted firmware OTA:** If the OTA image is encrypted (protecting IP), the decryption key is held in the secure enclave. The NSPE downloads the ciphertext; decryption happens in SPE; only plaintext is written to the update slot by the secure bootloader.

**Hardware security module (external HSM):**

For higher-assurance applications (automotive, industrial), an external HSM (e.g., Infineon SLB 9745, NXP SE050) connected via SPI or I2C provides:
- Key generation and storage in tamper-resistant hardware (active shielding, glitch detection).
- On-chip ECDSA signing/verification that never exposes the private key to the MCU.
- Physical attack resistance (the HSM will self-destruct its keys on tamper detection).

The bootloader sends the image hash to the HSM via SPI and receives a pass/fail result — the private key material never crosses the bus.

**Trade-off:** External HSM adds cost (~$0.50–$2.00 per unit), SPI/I2C bus initialisation time during boot (adds ~5–20 ms), and a second point of failure. TrustZone is sufficient for most IoT security profiles; HSMs are justified for payment terminals, medical devices, and high-value automotive ECUs.
