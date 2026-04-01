# OTA Firmware Update

## Prerequisites
- Dual-bank flash layout and bootloader architecture (see bootloader_design.md)
- Basic TCP/IP and TLS concepts
- Flash memory erase/write cycle limitations
- RTOS task management and inter-task communication

---

## Concept Reference

### OTA Update System Overview

An OTA (Over-The-Air) firmware update system allows a deployed embedded device to receive and install new firmware without physical access. The system spans four layers:

```
+----------------------------------------------------------+
|  Cloud / Update Server                                   |
|  - Stores signed firmware images                         |
|  - Manages device fleet, versions, rollout groups        |
|  - Issues update notifications and download URLs         |
+----------------------------------------------------------+
                         |  HTTPS / MQTT over TLS
+----------------------------------------------------------+
|  Device Transport Layer                                  |
|  - WiFi / LTE / BLE / Ethernet                          |
|  - TLS mutual authentication (device cert + server cert) |
|  - Chunked download with resume capability               |
+----------------------------------------------------------+
                         |  Internal IPC / event queue
+----------------------------------------------------------+
|  OTA Manager (application-level task)                    |
|  - Receive update notification                           |
|  - Verify server certificate (trust anchor)              |
|  - Download image in chunks, write to inactive slot      |
|  - Compute and verify CRC progressively                  |
|  - Request reboot when download complete                 |
+----------------------------------------------------------+
                         |  Flash write API
+----------------------------------------------------------+
|  Bootloader (runs after reboot)                          |
|  - Validate image (CRC, SHA-256, ECDSA signature)        |
|  - Swap active/inactive slots if valid                   |
|  - Confirm or roll back based on application health      |
+----------------------------------------------------------+
```

### A/B Partitioning (Symmetric Dual-Slot)

A/B partitioning maintains two equally-sized flash slots. One slot is always running; the other receives updates. After a validated update and successful boot, the roles swap.

```
Normal operation (Slot A active, version 1.3.0):

  Flash:
  [Bootloader] [Slot A: v1.3.0 ACTIVE] [Slot B: v1.2.0 STANDBY] [NVS]

OTA arrives: v1.4.0
  Step 1: OTA manager erases Slot B.
  Step 2: Download v1.4.0, write chunk by chunk to Slot B.
           Device continues running v1.3.0 from Slot A — no downtime during download.
  Step 3: Download complete. Compute CRC over Slot B. Compare to manifest CRC.
  Step 4: Write image header to Slot B (containing CRC, SHA-256, ECDSA signature, version).
  Step 5: Update NVS: preferred_slot = B, confirmed = false, attempt_count = 0.
  Step 6: Reset device.

After reset (bootloader):
  Step 7: Bootloader validates Slot B header: magic -> CRC -> SHA-256 -> signature.
  Step 8: Validation passes. Boot from Slot B.
  Step 9: v1.4.0 starts. Application runs self-test (peripheral init, connectivity check).
  Step 10: Self-test passes. Application writes confirmed = true to NVS.

Next reset:
  Bootloader sees: preferred_slot = B, confirmed = true -> boot Slot B normally.
  Slot A now holds v1.3.0 as the fallback image.
```

### Download and Write Pipeline

Writing a large image to flash while running the application requires careful management of:
1. Flash erase operations (can take 100 ms–2 s per sector on NOR flash).
2. Flash write operations (typically 64–256 bytes per page, ~100 µs per page).
3. Interrupt latency: flash write/erase operations on NOR flash may block CPU read access on the same bus (depending on architecture).

```
Recommended pipeline for chunked OTA download:

     Network         OTA Task            Flash Writer
        |                |                    |
        |  chunk[0]      |                    |
        +--------------->|                    |
        |                |  write chunk[0]    |
        |                +------------------->|  (1) erase sector if needed
        |                |                    |  (2) write page(s)
        |  chunk[1]      |  write complete     |
        +--------------->|<-------------------+
        |                |  write chunk[1]    |
        |                +------------------->|
        ...

Key points:
  - Use a double-buffer: download chunk N+1 into RAM while writing chunk N to flash.
  - Erase sectors ahead of writing (pre-erase), not inline with download.
  - Track write progress in NVS so a power-loss during download resumes rather
    than restarts from byte 0.
```

### Rollback Architecture

Rollback is the ability to revert to the previous firmware version when a new version fails.

```
Failure modes and rollback triggers:

  Type A: Image validation failure at boot
    - Bootloader CRC/signature check fails on preferred slot.
    - Action: immediately boot from other slot (no attempt counter needed).

  Type B: Application crash / watchdog reset
    - New firmware boots but crashes before confirming.
    - Boot attempt counter (in NVS) is incremented each boot.
    - After N consecutive unconfirmed boots: revert preferred_slot to previous.

  Type C: Application confirms but subsequently fails
    - Firmware passes initial self-test but fails in production use.
    - Application detects the failure, clears its confirmed flag, resets.
    - Bootloader sees confirmed = false; triggers rollback.
    - Requires explicit application logic to self-diagnose and request rollback.

  Type D: Cloud-initiated rollback
    - Update server pushes a signed "rollback command" as a special OTA payload.
    - OTA manager interprets the command and sets preferred_slot to previous.
    - Secure: the rollback command itself is signed, preventing replay attacks.
```

### OTA Security Threat Model

```
Threats addressed by a well-designed OTA system:

Threat                   Mitigation
-----------------------  ---------------------------------------------------
Unsigned firmware        ECDSA signature on every image; bootloader rejects
                         unsigned or incorrectly signed images.

Firmware from wrong      Pin server TLS certificate (cert pinning) or use
update server            mutual TLS; verify server's identity before download.

Replay / rollback attack Monotonic sequence number in image header;
                         bootloader rejects images with lower sequence number
                         than the minimum stored in OTP/secure counter.

MITM during download     TLS 1.3 encrypts the transport; image signature
                         provides end-to-end integrity independent of transport.

Image corruption in      CRC-32 checked after download, before reboot;
transit                  SHA-256 checked by bootloader before jumping.

Bricking via bad update  Dual-bank A/B partitioning; bootloader always retains
                         the previous known-good image in the inactive slot.

OTA manager exploited    OTA download runs as unprivileged RTOS task;
to write bootloader      bootloader flash region write-protected by option bytes.
```

---

## Tier 1 — Fundamentals

### Question F1
**What is the difference between a "confirmed" and "unconfirmed" firmware update in an A/B OTA system?**

**Answer:**

An **unconfirmed** update is one where the device has rebooted into the new firmware but the application has not yet signalled that it is operating correctly. The bootloader treats an unconfirmed boot as potentially failed and will revert to the previous slot if the boot attempt counter exceeds the configured threshold.

A **confirmed** update is one where the application has explicitly written a confirmation flag to non-volatile storage after successfully completing its startup self-test. The bootloader treats a confirmed slot as the new permanent baseline.

**Why explicit confirmation is required:**

Simply starting the application is not sufficient evidence that the update succeeded. A firmware update may:
- Boot successfully but fail to connect to the network 30 seconds later.
- Pass initial peripheral checks but crash under first real workload.
- Contain a regression in an infrequently executed code path.

The application is in the best position to know whether it is functioning correctly. The confirmation mechanism forces the developer to define what "working correctly" means and to verify it explicitly.

**Example confirmation sequence:**

```c
/* In application startup, after all critical subsystems are verified */

void app_startup_check(void)
{
    bool network_ok  = network_connect_and_ping(PING_TIMEOUT_MS);
    bool sensors_ok  = sensors_self_test();
    bool nvs_ok      = nvs_integrity_check();

    if (network_ok && sensors_ok && nvs_ok) {
        ota_confirm_current_image();   /* writes confirmed = true to NVS */
        LOG_INFO("OTA: image confirmed, version %s", fw_version_string());
    } else {
        LOG_ERROR("OTA: startup self-test failed, requesting rollback");
        ota_request_rollback();        /* writes confirmed = false, triggers reboot */
    }
}
```

---

### Question F2
**Why is it unsafe to erase and re-flash the currently running firmware slot during an OTA update (single-bank OTA)?**

**Answer:**

Single-bank OTA erases the flash sector(s) containing the running application to write the new image. This is unsafe for two reasons:

**1. Power loss during update bricks the device.**

An erase/write operation on NOR flash is not atomic. If power is lost while the sector containing the reset vector (address 0x0) or the application code is erased, the device boots to erased flash (all 0xFF). On Cortex-M, reading 0xFFFFFFFF as the reset vector handler is undefined behaviour and typically causes an immediate HardFault. There is no path to recovery without physical re-programming via JTAG/SWD.

**2. Executing code from flash that is simultaneously being erased.**

On most NOR flash architectures (single die, single bus), a flash erase operation locks the entire flash array. The CPU cannot fetch instructions during an erase. This either:
- Stalls the CPU until the erase completes (blocking for 100 ms – 2 s per sector).
- Causes a bus fault if the flash controller asserts a bus error during an active fetch.

The standard workaround is to copy the flash update routine into RAM (`__attribute__((section(".ramfunc")))`) and run it from there, but this does not solve the power-loss bricking problem.

**Why dual-bank is correct:** The inactive slot is always separate from the running slot. No code is being executed from the slot being erased/written. Power loss during a dual-bank OTA write leaves the running slot intact — the device simply reboots into the unmodified active slot.

---

### Question F3
**What is a firmware manifest, and how does it differ from the image header embedded in the firmware binary?**

**Answer:**

A **firmware manifest** is a metadata document delivered by the update server alongside (or before) the firmware binary. It describes the update and allows the device to make decisions about the update before downloading the full binary.

Typical manifest contents:
- Target device type and hardware version (prevents installing wrong firmware).
- Firmware version and sequence number.
- Size and CRC of the firmware binary.
- Download URL(s) and optional CDN fallback URLs.
- Minimum required bootloader version (prevents installing firmware incompatible with old bootloader).
- Rollout percentage (for staged rollouts: only update X% of devices this week).
- The manifest itself is signed by the update server's private key.

An **image header** is embedded at the start of the firmware binary (in flash). It is verified by the bootloader at runtime and contains:
- Magic number, size, version.
- CRC-32 and SHA-256 of the firmware payload.
- ECDSA signature over the payload hash.
- Sequence number for rollback prevention.

**Key difference:** The manifest is processed by the application's OTA manager over the network, before and during download. The image header is processed by the bootloader after the image has been written to flash. Both may contain overlapping fields (version, CRC), but the manifest is network-transport metadata and the image header is the embedded security credential.

---

## Tier 2 — Intermediate

### Question I1
**Describe how to implement resume-capable OTA download for a device with an unreliable network connection. What state must be persisted?**

**Answer:**

Resume-capable OTA download restarts from the last successfully written chunk rather than from the beginning when a connection is interrupted.

**State that must be persisted in NVS on every chunk write:**

```c
typedef struct {
    uint32_t  update_in_progress;   /* 1 = download started but not complete     */
    uint32_t  target_slot;          /* which flash slot is being written to       */
    uint32_t  bytes_written;        /* how many bytes of the image are in flash   */
    uint32_t  total_image_size;     /* from manifest, so we know when done        */
    uint32_t  running_crc;          /* CRC-32 of bytes_written so far             */
    uint8_t   image_url_hash[32];   /* SHA-256 of the URL/version being downloaded */
                                    /* detects if a new update supersedes the     */
                                    /* in-progress one (prevents mixing chunks)   */
    uint32_t  nvs_crc;              /* CRC of this struct to detect partial write */
} ota_resume_state_t;
```

**HTTP range request for resume:**

HTTP/1.1 supports range requests, allowing download to resume from a byte offset:

```
GET /firmware/v1.4.0.bin HTTP/1.1
Host: updates.example.com
Range: bytes=49152-           <-- resume from byte 49152 (48 KB already written)
Authorization: Bearer <device_token>
```

The server responds with:
```
HTTP/1.1 206 Partial Content
Content-Range: bytes 49152-307200/307200
Content-Length: 258048
```

**Resume sequence:**

```
On connection after interruption:
1. Read ota_resume_state from NVS.
2. Verify nvs_crc. If corrupt: restart from byte 0 (safe default).
3. Compute CRC-32 of the bytes already in flash (bytes 0..bytes_written-1).
   Compare to running_crc in NVS. If mismatch: flash was corrupted; restart.
4. Issue HTTP range request starting at bytes_written.
5. Append incoming chunks to flash starting at (slot_base + IMAGE_HDR_SIZE + bytes_written).
6. After each chunk: update bytes_written and running_crc in NVS.
7. When bytes_written == total_image_size: download complete.
   Verify final CRC against manifest. Write image header. Schedule reboot.
```

**NVS write frequency trade-off:** Writing NVS after every 256-byte chunk is safe but slow and increases NVS wear. Writing after every 4 KB sector boundary reduces writes by 16x with acceptable risk (at most 4 KB of re-download on failure). The correct granularity matches the flash erase sector size.

---

### Question I2
**How should an OTA manager authenticate the update server and protect the firmware image in transit? Describe the full security chain from server to flash.**

**Answer:**

**Full security chain:**

```
Update Server
    |
    |  [1] TLS 1.3 connection
    |      - Server presents X.509 certificate
    |      - Device verifies: cert signed by pinned CA root cert
    |      - Mutual TLS (mTLS): device also presents its own certificate
    |        (provisioned at manufacturing)
    |      - Result: encrypted channel; both parties authenticated
    |
    |  [2] Manifest download over authenticated TLS
    |      - Manifest contains: version, size, CRC, download URL
    |      - Manifest signed with server's ED25519 private key
    |      - Device verifies manifest signature using server's embedded public key
    |
    |  [3] Firmware binary download
    |      - Same TLS session (or resumed session)
    |      - Data encrypted in transit by TLS
    |
    v
OTA Manager (application task)
    |
    |  [4] Runtime integrity check during download
    |      - Compute running CRC-32 as chunks arrive
    |      - On completion: compare computed CRC to manifest CRC
    |      - Mismatch: discard the partial image, retry
    |
    |  [5] Write to inactive flash slot
    |
    v
Flash (inactive slot: raw image payload, no header yet)
    |
    |  [6] Write image header
    |      - After all payload bytes written and CRC verified
    |      - Header contains: magic, size, version, CRC, SHA-256, ECDSA signature
    |      - Header written last: the bootloader only trusts a slot if the header
    |        has a valid magic number, so writing last makes write atomic from
    |        the bootloader's perspective
    |
    v
Bootloader (after reboot)
    |
    |  [7] Independent validation of flash content
    |      - Re-reads image from flash (not relying on OTA manager's computation)
    |      - Verifies: magic -> CRC -> SHA-256 -> ECDSA signature
    |      - ECDSA signature was created by the build/release pipeline
    |        using the firmware signing private key (separate from server TLS key)
    |
    v
Application boots
```

**Why two separate keys (server TLS key vs firmware signing key):**

The TLS key authenticates the update server and protects the download channel. The firmware signing key authenticates the firmware binary itself, independently of how it was delivered. Separating the keys means:
- Rotating the TLS certificate (common, annual) does not require re-signing all firmware.
- Firmware signed with the private key can be verified by any device holding the corresponding public key, enabling CDN delivery, offline updates via SD card, and factory programming — all without needing a TLS connection at the verification point.
- Compromise of the TLS private key (on the server) does not allow an attacker to install malicious firmware on devices, because they do not have the firmware signing private key.

---

### Question I3
**A device reports that it repeatedly downloads an OTA update, reboots, but always reverts to the old firmware. Describe your diagnostic approach.**

**Answer:**

This symptom — download succeeds, reboot triggers, rollback occurs — means the new firmware is either failing bootloader validation, failing to start, or failing to confirm. Work through each layer:

**Step 1: Determine where the rollback is happening.**

Add logging at the boundary between each layer. If the bootloader has UART output (common in debug builds), check:
```
[BOOT] Slot B: magic OK, size OK, CRC OK, SHA-256 OK, signature FAIL -> boot Slot A
```
This indicates signature validation is failing.

Without UART output, check the NVS boot descriptor after a failed boot:
- `boot_attempt_count` > 0 but < threshold: app is starting but not confirming.
- `boot_attempt_count` >= threshold: app is crashing or not reaching confirmation.
- `preferred_slot` immediately equals `last_good_slot`: bootloader is rejecting before trying.

**Step 2: Common failure causes.**

| Symptom | Probable cause |
|---|---|
| Signature fails, CRC passes | Signing key in build pipeline does not match public key in bootloader |
| CRC fails | Incomplete download; flash write error; wrong image being validated |
| App never confirms, watchdog resets | Application crashes during init; startup self-test fails |
| App confirms but next NVS read shows unconfirmed | NVS write wear-out; wrong NVS address offset in new firmware |
| Works in development, fails in production | Production bootloader has different public key than development; correct — this is desired but confusing if not anticipated |

**Step 3: Systematic checks.**

```
1. Build: re-sign the firmware image and verify the signature locally
   against the public key embedded in the production bootloader binary.

2. Transport: inspect the downloaded image in the inactive slot:
   - Read back the flash slot via JTAG/SWD.
   - Compute SHA-256 of the payload manually.
   - Compare to the SHA-256 in the image header.
   If they differ: the flash write is corrupted (ECC error, write cycle failure).

3. Bootloader: add verbose logging to the validation path (if build permits).
   Enable debug UART in the bootloader and capture output on the next boot cycle.

4. Application: add a log message in the first line after reset handler.
   If this message never appears: bootloader is not jumping to the new firmware.
   If it appears but confirmation code is never reached: crash before confirmation.

5. NVS: dump the boot descriptor over debug UART or JTAG and compare
   attempt_count, confirmed flag, and preferred_slot on successive reboots.
```

---

## Tier 3 — Advanced

### Question A1
**Design a delta (differential) OTA update system for a device with 512 KB flash and a poor network connection that makes downloading a full 300 KB image impractical. What are the components and trade-offs?**

**Answer:**

Delta OTA transmits only the binary difference between the old and new firmware, dramatically reducing download size. A typical firmware update that changes 20% of code produces a delta of 30–60 KB rather than 300 KB.

**System components:**

```
Build Pipeline (server side):
  1. Build new firmware image: v1.4.0.bin (300 KB).
  2. Retrieve previous firmware image: v1.3.0.bin (290 KB).
  3. Run binary differencer: bsdiff, Janpatch, or Xdelta3.
     Output: v1.3.0_to_v1.4.0.patch (e.g., 45 KB).
  4. Sign the patch file (same ECDSA process as full images).
  5. Upload patch to update server.

Device (OTA Manager):
  1. Receive update notification: patch from v1.3.0 to v1.4.0, size 45 KB.
  2. Verify running version == v1.3.0 (the base for this patch).
     If not: download full image instead.
  3. Download 45 KB patch.
  4. Verify patch CRC and signature.
  5. Apply patch: read v1.3.0 from active slot, apply patch in streaming
     fashion, write reconstructed v1.4.0 to inactive slot.
  6. Verify reconstructed image SHA-256 matches manifest.
  7. Proceed as normal OTA: write header, reboot, bootloader validates, confirm.
```

**Streaming patch application — the key challenge:**

The device cannot hold both the source image and reconstructed destination in RAM simultaneously (300 KB + 300 KB >> typical RAM). It must stream the operation:

```
Streaming bspatch on Cortex-M4 (Janpatch library approach):

  Source:  Active slot (read-only, accessed via flash read)
  Patch:   Downloaded into a RAM buffer (45 KB) or streamed from NVS
  Dest:    Inactive slot (written sector by sector)

  Algorithm operates on three input streams simultaneously:
    - CTRL stream: (x, y, z) tuples describing copy, add, and extra operations
    - DIFF stream: byte deltas to add to source bytes
    - EXTRA stream: bytes with no correspondence in source (new insertions)

  Memory requirement: O(patch_size) working RAM, not O(image_size).
  Typical RAM usage for Janpatch: 4–8 KB working buffer + patch in NVS.
```

**Trade-offs:**

| Aspect | Full Image OTA | Delta OTA |
|---|---|---|
| Download size | Full image (300 KB) | Patch only (30–60 KB typical) |
| Implementation complexity | Low | High (differ + patcher) |
| Version dependency | None (any version can receive) | Patch is version-specific; need separate patches per base version |
| Patch failure recovery | N/A | If patch application fails (e.g., power loss mid-apply), must fall back to full image download |
| RAM requirement during apply | Low (chunk-by-chunk write) | Medium (Janpatch: 4–8 KB working buffer) |
| Applicable to highly fragmented diffs | N/A | Poor: large ctrl stream for scattered changes; delta size approaches full image |
| Server infrastructure | Simple file hosting | Requires patch generation per version pair; patch database grows quadratically with versions |

**When delta OTA is justified:** LPWAN networks (LoRaWAN, NB-IoT) with typical throughput of 0.3–50 kbps, or cellular devices billed per byte. A 45 KB patch at 10 kbps takes 36 seconds vs 240 seconds for a 300 KB full image.

---

### Question A2
**A fleet of 10,000 devices is receiving a critical security patch. Describe a staged rollout strategy and the server-side and device-side mechanisms that implement it safely.**

**Answer:**

A staged rollout (also called a canary deployment) distributes a firmware update to a fraction of devices first, monitors their health, and widens the rollout only if the initial cohort is stable.

**Rollout phases:**

```
Phase 0 — Internal devices (1% of fleet = ~100 devices):
  Target: engineering-team and QA devices.
  Duration: 24 hours.
  Success criteria: no crash reports, all devices confirm boot,
                    all devices check in with server within 1 hour of update.

Phase 1 — Canary (5% = 500 devices):
  Target: randomly selected from fleet, excluding critical installations.
  Duration: 48 hours.
  Success criteria: boot confirmation rate > 99.5%, no regression in
                    telemetry (sensor readings, connectivity uptime).

Phase 2 — Broad rollout (50% = 5,000 devices):
  Duration: 72 hours.
  Automatic if Phase 1 KPIs met; manual approval gate otherwise.

Phase 3 — Full fleet (100%):
  Duration: ongoing until all online devices updated.
  Devices that are offline receive update on next connection.
```

**Server-side mechanisms:**

```
1. Rollout groups stored in the device management database:
   SELECT * FROM devices
   WHERE group_id = 'canary'
     AND firmware_version < '1.4.0'
   LIMIT 500;

2. Update eligibility check: when device polls for updates, server
   evaluates:
   - Is the device in an eligible group for the current rollout phase?
   - Is the device's current version the correct base for this update?
   - Has the rollout been paused due to error threshold breach?

3. Automatic pause trigger (server-side):
   Monitor: boot_confirmation_rate over 1-hour sliding window.
   If boot_confirmation_rate < 98%: pause rollout, alert on-call engineer.
   Threshold prevents a bad update from propagating to the full fleet.
```

**Device-side mechanisms:**

```c
/* OTA manager: check whether this device should apply the update */

typedef struct {
    uint8_t  rollout_percentage;   /* 0-100: fraction of fleet to update         */
    uint32_t device_cohort_seed;   /* random 32-bit value set at manufacturing   */
} ota_rollout_policy_t;

bool ota_device_is_eligible(const ota_rollout_policy_t *policy)
{
    /* Deterministic cohort assignment: hash the device serial number or
     * cohort seed, take modulo 100. Device is eligible if result <
     * rollout_percentage. The same device always gets the same cohort
     * assignment, preventing devices from flipping in and out of rollout. */
    uint32_t cohort = policy->device_cohort_seed % 100;
    return (cohort < policy->rollout_percentage);
}
```

**Why per-device cohort seed (not pure random at update time):**

If eligibility were re-evaluated randomly at every poll, a device might be told "not eligible" 10 times and then "eligible" — but meanwhile the rollout would appear to be stuck. The deterministic seed ensures each device has a fixed position in the 0–100 cohort space, so eligibility follows a clean CDF as rollout_percentage increases.

**Rollback for the fleet (cloud-initiated):**

If a critical bug is discovered after a 50% rollout, the server pushes a rollback manifest:

```json
{
  "action": "rollback",
  "rollback_to_version": "1.3.0",
  "sequence_num": 15,
  "reason": "CVE-2026-XXXX: remote code execution in v1.4.0 parser",
  "signature": "<ECDSA signature over this manifest>"
}
```

The device OTA manager receives the rollback manifest, verifies its signature, then instructs the bootloader (via NVS) to switch preferred_slot back to the slot holding v1.3.0 and triggers a reset. This is secure because the rollback manifest is signed — an attacker cannot forge a rollback to an even older vulnerable version without the server's private key.

---

### Question A3
**Explain encrypted OTA firmware delivery. When is image encryption necessary, and what are the key management challenges on a constrained embedded device?**

**Answer:**

**When encryption is necessary:**

1. **IP protection:** The firmware binary contains proprietary algorithms, calibration data, or trade secrets. An unencrypted image downloadable from a CDN can be disassembled and reverse-engineered by competitors.

2. **Preventing cloning:** Without encryption, a manufacturer's competitor could extract a legitimate signed firmware image from one device and flash it to counterfeit hardware (assuming the counterfeit hardware passes the bootloader's signature check).

3. **Regulatory requirements:** Medical devices (FDA), automotive (ISO/SAE 21434), and some payment (PCI DSS) standards require confidentiality of firmware in certain threat scenarios.

**When encryption is NOT necessary (and adds cost without benefit):**

- Open-source firmware where the source code is publicly available.
- Devices where the attacker has physical access and can extract the decryption key anyway (breaking encryption provides no additional barrier beyond what signature verification provides).
- Devices where the primary threat is modification (not disclosure) — signature verification alone addresses this.

**Key management architecture for encrypted OTA:**

```
Per-device encryption approach (maximum security):

  Manufacturing:
  1. Generate a unique 256-bit AES-GCM key for each device (Device Encryption Key, DEK).
  2. Store DEK in the device's secure element / OTP / TrustZone secure storage.
     DEK never leaves the device after provisioning.
  3. Register DEK (encrypted with the HSM's master key) in the key management service.

  Update pipeline:
  1. Build and sign firmware: v1.4.0.bin (same as non-encrypted process).
  2. For each device (or batch):
     a. Retrieve the device's DEK from KMS (decrypting with HSM master key).
     b. Encrypt v1.4.0.bin with AES-GCM-256 using the DEK.
        AES-GCM produces ciphertext + 16-byte authentication tag.
     c. Produce a personalised encrypted image: device_id.enc.bin.
  3. Deliver device_id.enc.bin to the specific device (or use a personalised URL).

  Device decryption (in bootloader or secure enclave):
  1. Bootloader receives encrypted image in flash.
  2. Load DEK from secure storage (TrustZone SPE / secure element).
  3. Decrypt with AES-GCM. Verify authentication tag.
     Authentication tag failure = decryption error or wrong key: reject image.
  4. Write decrypted plaintext to the verified-image buffer.
  5. Proceed with normal CRC and signature validation on decrypted content.
```

**Simpler alternative: symmetric fleet key (lower security, lower complexity):**

```
Single AES-128 fleet encryption key embedded in the bootloader.
All devices share the same key.
Firmware binary is encrypted with the fleet key before distribution.
Bootloader decrypts before validation.

Risk: if the fleet key is extracted from one device (via fault injection or
      physical decap), all devices are compromised.
Use case: IP protection against casual reverse engineering, not state-level actors.
```

**Key management challenges on constrained devices:**

1. **Secure key storage:** A Cortex-M0 without TrustZone has no hardware-enforced isolation. The DEK must be stored in flash (accessible to any code running on the device). Mitigation: enable read-out protection (RDP Level 2 on STM32: disables JTAG, locks flash reads) at manufacturing. This prevents key extraction via debug interfaces but also prevents legitimate debugging.

2. **Key rotation:** If a fleet key is compromised, all devices need a new key. Rotating keys over OTA is a chicken-and-egg problem: the new key must be delivered encrypted with the old key, which is compromised. Solution: use asymmetric key encapsulation (ECIES or RSA-OAEP) — the device's public key encrypts a new symmetric key, so only the specific device can decrypt it with its private key.

3. **Boot-time decryption latency:** AES-GCM-256 decryption of a 300 KB image on a Cortex-M4 at 80 MHz without hardware AES:
   - Software AES-GCM throughput ~10 MB/s on Cortex-M4.
   - 300 KB / 10 MB/s = ~30 ms — acceptable.
   - With hardware AES (e.g., STM32 CRYP peripheral, DMA-fed): ~1 ms.

4. **Secure element availability and cost:** An external secure element (ATECC608, SE050) adds $0.30–$1.00 per unit and an I2C transaction at every boot. For high-volume IoT devices this may be unacceptable. ARM TrustZone provides a software equivalent without added cost if the MCU supports it (Cortex-M23/M33/M85).
