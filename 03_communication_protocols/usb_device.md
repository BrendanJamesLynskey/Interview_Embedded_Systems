# USB Device Implementation

## Overview

USB is one of the most complex protocols an embedded engineer encounters. Its complexity lies
not in the wire protocol but in the layered abstraction: descriptors, device classes,
enumeration state machines, and the interrupt-driven endpoint model. Interviews at companies
building USB peripherals (HID peripherals, USB audio, industrial instruments, motor controllers
with USB config interfaces) regularly probe this domain.

This file covers USB 2.0 full-speed and high-speed device implementation: descriptors,
enumeration, endpoint types, the four standard device classes (CDC, HID, MSC, Audio), and
the driver architecture patterns used in STM32 HAL/LL and equivalent.

---

## Fundamentals

### Q1. What is USB enumeration and what happens during it?

**Question:** Describe the USB enumeration process from device insertion to the device being
usable. List every request the host sends to the device.

**Answer:**

**Enumeration** is the process by which the host discovers a newly attached device, reads
its descriptors, assigns it an address, and loads the correct driver.

**Physical connection:**

When a USB device is plugged in, the device pulls up either D+ or D- through a 1.5 kΩ
resistor:
- D+ pull-up: full-speed (12 Mbps) or high-speed device
- D- pull-up: low-speed (1.5 Mbps) device

The host (or hub) detects this pull-up and signals an attach event to the USB host controller.

**Enumeration sequence:**

```
1. Host detects device attach
   - Resets the bus (SE0 for > 2.5 µs, 50 ms for root hub)

2. Host reads device descriptor at address 0
   GET_DESCRIPTOR(Device, length=8)  <- only first 8 bytes (bMaxPacketSize0)
   Response: first 8 bytes of device descriptor including bMaxPacketSize0

3. Host resets bus again

4. Host assigns address
   SET_ADDRESS(address=N)   <- e.g., N=1
   Device: ACK, then respond to address N from now on

5. Host reads full device descriptor
   GET_DESCRIPTOR(Device, length=18)
   Response: 18-byte device descriptor

6. Host reads configuration descriptor
   GET_DESCRIPTOR(Configuration, length=9)  <- just header first
   Response: 9-byte configuration descriptor (includes wTotalLength)

   GET_DESCRIPTOR(Configuration, length=wTotalLength)  <- full tree
   Response: configuration + interface + endpoint descriptors (variable length)

7. Host reads string descriptors (optional, if iManufacturer/iProduct != 0)
   GET_DESCRIPTOR(String, index=0)          <- language ID list
   GET_DESCRIPTOR(String, index=iManufacturer)
   GET_DESCRIPTOR(String, index=iProduct)
   GET_DESCRIPTOR(String, index=iSerialNumber)

8. Host reads class-specific descriptors (e.g., HID report descriptor)
   GET_DESCRIPTOR(HID_REPORT, ...)

9. Host selects configuration
   SET_CONFIGURATION(bConfigurationValue=1)

10. Device is now configured — class driver is loaded and endpoints are active
```

**Why the two-step configuration read?**

The host reads only 9 bytes first to get `wTotalLength`, which tells it the total size of
the configuration descriptor tree (including all interface and endpoint descriptors). It then
reads the full tree in a second request.

---

### Q2. Describe the USB descriptor hierarchy.

**Question:** Draw the USB descriptor hierarchy for a USB CDC device. What are the mandatory
fields in each descriptor?

**Answer:**

**Descriptor hierarchy (CDC ACM device):**

```
Device Descriptor (1)
  |
  +-- Configuration Descriptor (1)
        |
        +-- Interface Association Descriptor (optional, for composite devices)
        |
        +-- Interface Descriptor [0] (CDC Control Interface)
        |     |
        |     +-- CDC Header Functional Descriptor
        |     +-- CDC Call Management Functional Descriptor
        |     +-- CDC Abstract Control Management Functional Descriptor
        |     +-- CDC Union Functional Descriptor
        |     |
        |     +-- Endpoint Descriptor [0x81] (IN, Interrupt, Notification)
        |
        +-- Interface Descriptor [1] (CDC Data Interface)
              |
              +-- Endpoint Descriptor [0x02] (OUT, Bulk, Host->Device)
              +-- Endpoint Descriptor [0x82] (IN,  Bulk, Device->Host)
```

**Device Descriptor (18 bytes):**

```c
typedef struct __attribute__((packed)) {
    uint8_t  bLength;            /* 18 */
    uint8_t  bDescriptorType;    /* 0x01 = DEVICE */
    uint16_t bcdUSB;             /* USB spec: 0x0200 = USB 2.0 */
    uint8_t  bDeviceClass;       /* 0x02 = CDC (or 0xEF for composite with IAD) */
    uint8_t  bDeviceSubClass;    /* 0x00 for CDC */
    uint8_t  bDeviceProtocol;    /* 0x00 for CDC */
    uint8_t  bMaxPacketSize0;    /* max packet size for EP0: 8, 16, 32, or 64 */
    uint16_t idVendor;           /* USB Vendor ID (assigned by USB-IF) */
    uint16_t idProduct;          /* Product ID (vendor-assigned) */
    uint16_t bcdDevice;          /* Device release number (BCD) */
    uint8_t  iManufacturer;      /* index into string descriptor table */
    uint8_t  iProduct;           /* index into string descriptor table */
    uint8_t  iSerialNumber;      /* index into string descriptor table */
    uint8_t  bNumConfigurations; /* 1 for most devices */
} UsbDeviceDescriptor;
```

**Configuration Descriptor (9 bytes, followed by interfaces):**

```c
typedef struct __attribute__((packed)) {
    uint8_t  bLength;             /* 9 */
    uint8_t  bDescriptorType;     /* 0x02 = CONFIGURATION */
    uint16_t wTotalLength;        /* total bytes in this + all following descriptors */
    uint8_t  bNumInterfaces;      /* 2 for CDC ACM */
    uint8_t  bConfigurationValue; /* value for SET_CONFIGURATION (usually 1) */
    uint8_t  iConfiguration;      /* string descriptor index (0=no string) */
    uint8_t  bmAttributes;        /* 0x80 = bus-powered, 0xC0 = self-powered */
    uint8_t  bMaxPower;           /* in 2 mA units; 250 = 500 mA */
} UsbConfigDescriptor;
```

**Endpoint Descriptor (7 bytes):**

```c
typedef struct __attribute__((packed)) {
    uint8_t  bLength;           /* 7 */
    uint8_t  bDescriptorType;   /* 0x05 = ENDPOINT */
    uint8_t  bEndpointAddress;  /* bit7: 0=OUT, 1=IN; bits[3:0]: endpoint number */
    uint8_t  bmAttributes;      /* bits[1:0]: 00=Control, 01=Isoch, 10=Bulk, 11=Interrupt */
    uint16_t wMaxPacketSize;    /* max packet size for this endpoint */
    uint8_t  bInterval;        /* polling interval (ms for FS interrupt, 125µs units for HS) */
} UsbEndpointDescriptor;
```

---

### Q3. What are the four USB endpoint types and their characteristics?

**Question:** Describe the four endpoint types (Control, Bulk, Interrupt, Isochronous).
For each, state: guaranteed bandwidth, error detection/correction, and typical use case.

**Answer:**

**Control (EP0 — mandatory):**

- **Purpose:** Device enumeration and class-specific control requests. Every device has EP0.
- **Direction:** Bidirectional (setup, IN, and OUT phases).
- **Error detection:** Yes — CRC, retry up to 3 times, NAK allowed.
- **Bandwidth guarantee:** Guaranteed up to 10% of bus bandwidth by the host controller.
- **Max packet size:** 8/16/32/64 bytes (full-speed); 64 bytes (high-speed).
- **Use case:** GET_DESCRIPTOR, SET_ADDRESS, SET_CONFIGURATION, HID SET_REPORT.

**Bulk:**

- **Purpose:** Large data transfers where latency is unimportant but reliability is mandatory.
- **Direction:** Unidirectional (IN or OUT).
- **Error detection:** Yes — CRC, unlimited retries (host retries automatically on error).
- **Bandwidth guarantee:** None. Bulk transactions use remaining bandwidth after Control,
  Interrupt, and Isochronous are served.
- **Max packet size:** 64 bytes (full-speed); 512 bytes (high-speed).
- **Use case:** USB Mass Storage (file transfer), CDC data, printer, test equipment.

**Interrupt:**

- **Purpose:** Periodic, low-latency, small data with bounded delivery time.
- **Direction:** Unidirectional.
- **Error detection:** Yes — CRC, retry.
- **Bandwidth guarantee:** Yes — the host polls at the interval specified in bInterval.
  If no data is ready, the device returns NAK.
- **Max packet size:** 64 bytes (full-speed); 1024 bytes (high-speed).
- **Poll interval (bInterval):** 1-255 ms (full-speed); 1-16 (125 µs * 2^(n-1) for high-speed).
- **Use case:** HID keyboards/mice (1-8 ms poll), CDC notification endpoint, status updates.

**Isochronous:**

- **Purpose:** Continuous, time-sensitive streams where occasional data loss is acceptable
  but latency is not.
- **Direction:** Unidirectional.
- **Error detection:** CRC checked, but NO retries. If a packet is lost, it is gone.
- **Bandwidth guarantee:** Yes — bandwidth is reserved at enumeration. The host guarantees
  one transaction per frame (every 1 ms for full-speed, every 125 µs for high-speed).
- **Max packet size:** 1023 bytes (full-speed); 1024 bytes (high-speed), with up to 3
  transactions per microframe for high-speed (3072 bytes/125 µs = ~196 Mbps).
- **Use case:** USB audio (PCM samples), USB webcam (video frames), MIDI.

---

### Q4. What is a USB HID device and how does it report data?

**Question:** Describe the USB HID (Human Interface Device) class. What is a HID report
descriptor and how does the host parse it? Write a minimal HID descriptor for a 3-button
2-axis mouse.

**Answer:**

**HID class overview:**

HID is a USB device class for human interface devices. Its defining feature is the
**report descriptor**: a variable-length binary document that describes every field in every
data packet (report) the device will send or receive. The host parses the report descriptor
and learns the meaning of each bit without device-specific drivers.

**HID descriptor in the configuration tree:**

```
Interface Descriptor (bInterfaceClass=0x03 HID, bInterfaceSubClass=0x01 boot)
  |
  +-- HID Descriptor (special class-specific descriptor)
  |     bLength=9, bDescriptorType=0x21(HID)
  |     bcdHID=0x0111 (HID spec 1.11)
  |     bCountryCode=0 (not localised)
  |     bNumDescriptors=1
  |     bDescriptorType=0x22 (Report)
  |     wDescriptorLength=N (length of the report descriptor)
  |
  +-- Endpoint Descriptor [0x81] (IN, Interrupt, 1 ms poll)
```

**Report descriptor for a 3-button, 2-axis mouse:**

```c
/* HID Usage Tables (HID Usage Table 1.3) define the meanings */
static const uint8_t hid_mouse_report_desc[] = {
    0x05, 0x01,  /* Usage Page (Generic Desktop Controls)    */
    0x09, 0x02,  /* Usage (Mouse)                            */
    0xA1, 0x01,  /* Collection (Application)                 */

    0x09, 0x01,  /*   Usage (Pointer)                        */
    0xA1, 0x00,  /*   Collection (Physical)                  */

    /* 3 buttons (bits 0-2), 5 padding bits */
    0x05, 0x09,  /*     Usage Page (Buttons)                 */
    0x19, 0x01,  /*     Usage Minimum (Button 1)             */
    0x29, 0x03,  /*     Usage Maximum (Button 3)             */
    0x15, 0x00,  /*     Logical Minimum (0)                  */
    0x25, 0x01,  /*     Logical Maximum (1)                  */
    0x95, 0x03,  /*     Report Count (3)                     */
    0x75, 0x01,  /*     Report Size (1 bit)                  */
    0x81, 0x02,  /*     Input (Data, Variable, Absolute)     */
    0x95, 0x01,  /*     Report Count (1)                     */
    0x75, 0x05,  /*     Report Size (5 bits padding)         */
    0x81, 0x03,  /*     Input (Constant, Variable, Absolute) */

    /* X axis: signed 8-bit relative movement */
    0x05, 0x01,  /*     Usage Page (Generic Desktop)         */
    0x09, 0x30,  /*     Usage (X)                            */
    0x09, 0x31,  /*     Usage (Y)                            */
    0x15, 0x81,  /*     Logical Minimum (-127)               */
    0x25, 0x7F,  /*     Logical Maximum (+127)               */
    0x75, 0x08,  /*     Report Size (8 bits)                 */
    0x95, 0x02,  /*     Report Count (2)                     */
    0x81, 0x06,  /*     Input (Data, Variable, Relative)     */

    0xC0,        /*   End Collection (Physical)              */
    0xC0,        /* End Collection (Application)             */
};
/* Total report size: 1 byte (buttons+padding) + 1 byte (X) + 1 byte (Y) = 3 bytes */
```

**Sending a HID report (STM32 HAL):**

```c
/* Report structure matching the descriptor above */
typedef struct __attribute__((packed)) {
    uint8_t buttons;    /* bits 0-2: button 1/2/3; bits 7-3: padding (0) */
    int8_t  x;          /* relative X movement (-127 to +127) */
    int8_t  y;          /* relative Y movement (-127 to +127) */
} MouseReport;

void hid_send_mouse_report(int8_t dx, int8_t dy, uint8_t buttons)
{
    MouseReport report = {
        .buttons = buttons & 0x07,   /* mask to 3 button bits */
        .x       = dx,
        .y       = dy,
    };
    /* Send via interrupt IN endpoint at next poll interval */
    USBD_HID_SendReport(&hUsbDeviceFS, (uint8_t *)&report, sizeof(report));
}
```

---

## Intermediate

### Q5. How does the USB CDC class create a virtual serial port?

**Question:** Describe the CDC-ACM (Abstract Control Model) class. What class-specific requests
must the device handle? How does data flow between the host application and the device?

**Answer:**

**CDC-ACM (Communications Device Class – Abstract Control Model)** implements a virtual serial
port (COM port on Windows, `/dev/ttyACM` on Linux). No vendor driver is required on Linux
and macOS; Windows requires the built-in `usbser.sys` with a matching INF file.

**Interface structure:**

Two interfaces are required:
1. **CDC Control Interface** (bInterfaceClass=0x02): carries class-specific requests and
   the notification endpoint (for line state changes).
2. **CDC Data Interface** (bInterfaceClass=0x0A): carries bulk IN and OUT endpoints for
   the actual serial data.

**Mandatory class-specific requests:**

The host sends these via EP0 (control endpoint):

| Request | bRequest | Direction | Description |
|---------|----------|-----------|-------------|
| SET_LINE_CODING | 0x20 | Host->Device | Baud rate, data bits, parity, stop bits |
| GET_LINE_CODING | 0x21 | Device->Host | Current serial parameters |
| SET_CONTROL_LINE_STATE | 0x22 | Host->Device | DTR and RTS line state |

```c
/* LineCoding structure (7 bytes) */
typedef struct __attribute__((packed)) {
    uint32_t dwDTERate;      /* Baud rate (e.g., 115200) */
    uint8_t  bCharFormat;    /* Stop bits: 0=1, 1=1.5, 2=2 */
    uint8_t  bParityType;    /* 0=None, 1=Odd, 2=Even, 3=Mark, 4=Space */
    uint8_t  bDataBits;      /* 5, 6, 7, 8, or 16 */
} UsbCdcLineCoding;

static UsbCdcLineCoding line_coding = {
    .dwDTERate  = 115200,
    .bCharFormat = 0,   /* 1 stop bit */
    .bParityType = 0,   /* no parity */
    .bDataBits   = 8,
};

/* Handle SET_LINE_CODING in the USB class request handler */
USBD_StatusTypeDef CDC_Control_FS(uint8_t cmd, uint8_t *pbuf, uint16_t length)
{
    switch (cmd) {
    case CDC_SET_LINE_CODING:
        /* Host is configuring baud rate etc. (e.g., after minicom starts) */
        memcpy(&line_coding, pbuf, sizeof(line_coding));
        /* Optionally reconfigure the physical UART if bridging to hardware */
        uart_set_baudrate(line_coding.dwDTERate);
        break;

    case CDC_GET_LINE_CODING:
        memcpy(pbuf, &line_coding, sizeof(line_coding));
        break;

    case CDC_SET_CONTROL_LINE_STATE:
        /* pbuf[0] bit0 = DTR, bit1 = RTS */
        /* Useful for detecting when the host terminal opens/closes the port */
        cdc_dtr_active = (pbuf[0] & 0x01) != 0;
        break;
    }
    return USBD_OK;
}
```

**Data flow:**

```
Host application (Python, minicom, etc.)
  |
  v
OS USB driver (usbser.sys / cdc_acm.ko)
  |
  v
USB Bulk OUT transaction  ->  EP2 OUT  ->  Device receive buffer
                                              |
                                              v
                                         CDC_Receive_FS() callback
                                              |
                                              v
                                         Application processes data

Device generates data:
  Application -> CDC_Transmit_FS(buf, len) -> EP2 IN -> Host bulk IN transfer
```

**Zero-length packet (ZLP) handling:**

If a bulk transfer's last packet exactly fills the maximum packet size (64 bytes for
full-speed, 512 bytes for high-speed), the host cannot tell if the transfer is complete.
The device must send a Zero-Length Packet to signal end-of-transfer.

```c
/* STM32 HAL handles ZLP automatically in USBD_CDC_TransmitPacket */
/* But for raw LL access: */
if ((len % max_packet_size) == 0) {
    /* Send ZLP to signal end of transfer */
    usb_send_zlp(endpoint_in);
}
```

---

### Q6. How does USB Mass Storage (MSC) work at the protocol level?

**Question:** Describe the USB MSC Bulk-Only Transport (BOT) protocol. What is the CBW/CSW
structure and how does the device handle a SCSI READ(10) command?

**Answer:**

**USB MSC with Bulk-Only Transport (BOT):**

MSC uses two bulk endpoints (IN and OUT) to exchange SCSI commands wrapped in USB-specific
Command Block Wrappers (CBW) and Command Status Wrappers (CSW).

**CBW (Command Block Wrapper — 31 bytes, host to device):**

```c
typedef struct __attribute__((packed)) {
    uint32_t dCBWSignature;          /* 0x43425355 "USBC" */
    uint32_t dCBWTag;                /* host-assigned tag; echoed in CSW */
    uint32_t dCBWDataTransferLength; /* bytes of data to transfer */
    uint8_t  bmCBWFlags;             /* bit7: 0=OUT, 1=IN */
    uint8_t  bCBWLUN;                /* Logical Unit Number (0 for single LUN) */
    uint8_t  bCBWCBLength;           /* length of CBWCB (6-16 bytes) */
    uint8_t  CBWCB[16];              /* SCSI command block */
} MscCbw;
```

**CSW (Command Status Wrapper — 13 bytes, device to host):**

```c
typedef struct __attribute__((packed)) {
    uint32_t dCSWSignature;   /* 0x53425355 "USBS" */
    uint32_t dCSWTag;         /* mirrors dCBWTag from the CBW */
    uint32_t dCSWDataResidue; /* bytes not transferred (0 if all transferred) */
    uint8_t  bCSWStatus;      /* 0=success, 1=failed (SCSI error), 2=phase error */
} MscCsw;
```

**SCSI READ(10) transaction sequence:**

```
1. Host sends CBW (31 bytes via Bulk OUT):
   dCBWTag = 0x12345678
   dCBWDataTransferLength = 512  (one 512-byte sector)
   bmCBWFlags = 0x80             (IN direction: device -> host)
   bCBWCBLength = 10
   CBWCB = {0x28, 0, 0, 0, 0, LBA_MSB, LBA, LBA, LBA_LSB, 0, 1, 0, ...}
           ^ READ(10) opcode     ^ LBA (logical block address)   ^ transfer length (1 block)

2. Device processes command:
   - Validate CBW signature and fields
   - Read 512 bytes from storage (NAND flash, SD card, etc.)
   - Send 512 bytes via Bulk IN to host

3. Device sends CSW (13 bytes via Bulk IN):
   dCSWTag = 0x12345678    (mirrors CBW tag)
   dCSWDataResidue = 0     (all 512 bytes transferred)
   bCSWStatus = 0x00       (success)
```

**Device-side MSC handler (simplified STM32 pattern):**

```c
/* MSC state machine */
typedef enum {
    MSC_IDLE,
    MSC_CBW_RECEIVED,
    MSC_DATA_IN,
    MSC_DATA_OUT,
    MSC_CSW_SEND
} MscState;

static MscState  msc_state = MSC_IDLE;
static MscCbw    current_cbw;
static MscCsw    current_csw;
static uint32_t  data_remaining;
static uint32_t  current_lba;

void msc_handle_cbw(const uint8_t *buf, uint16_t len)
{
    memcpy(&current_cbw, buf, sizeof(MscCbw));

    if (current_cbw.dCBWSignature != 0x43425355U) {
        /* Invalid signature — stall both endpoints (phase error recovery) */
        usb_stall_endpoint(EP_BULK_IN);
        usb_stall_endpoint(EP_BULK_OUT);
        return;
    }

    /* Decode SCSI command */
    switch (current_cbw.CBWCB[0]) {
    case 0x28:  /* READ(10) */
        current_lba      = ((uint32_t)current_cbw.CBWCB[2] << 24) |
                           ((uint32_t)current_cbw.CBWCB[3] << 16) |
                           ((uint32_t)current_cbw.CBWCB[4] <<  8) |
                            (uint32_t)current_cbw.CBWCB[5];
        data_remaining   = current_cbw.dCBWDataTransferLength;
        msc_state        = MSC_DATA_IN;
        msc_read_and_send(current_lba, data_remaining / 512);
        break;

    case 0x12:  /* INQUIRY */
        msc_send_inquiry_response();
        break;

    case 0x00:  /* TEST UNIT READY */
        /* No data phase — send CSW directly */
        msc_send_csw(0x00);  /* success */
        break;

    default:
        /* Unsupported command — SCSI ILLEGAL REQUEST */
        msc_send_csw(0x01);  /* command failed */
        break;
    }
}
```

---

## Advanced

### Q7. What is USB high-speed and how does a device negotiate it?

**Question:** Explain the USB full-speed to high-speed negotiation (chirp sequence). What
hardware is required? What is the performance difference?

**Answer:**

**USB speed tiers:**

| Speed | Max bit rate | Typical application |
|-------|-------------|-------------------|
| Low-speed (LS) | 1.5 Mbps | HID keyboards, mice |
| Full-speed (FS) | 12 Mbps | CDC, HID, audio |
| High-speed (HS) | 480 Mbps | Mass storage, video |
| SuperSpeed (USB 3.x) | 5/10/20+ Gbps | Not covered here |

**Physical requirement for high-speed:**

High-speed requires the device to have a high-speed capable transceiver (not present on
all MCUs). The USB 2.0 HS transceiver uses different electrical levels to achieve 480 Mbps:
low-swing differential signalling (~400 mV differential vs ~3.3 V for FS).

**Chirp negotiation sequence:**

After bus reset (SE0 for > 2.5 µs), a HS-capable device announces its capability:

```
1. Bus reset (SE0 from host, >2.5 µs)
2. Device responds: asserts Chirp K (HS device-side: pulls D- ~400 mV differential)
   Chirp K is a single-ended K state held for 2-7 ms.
3. HS-capable hub/host responds with 3 pairs of Chirp K/J sequences (6 chirps total)
4. Device recognises the 3 K/J pairs: switches to HS mode.
5. If the host is only FS capable, it ignores the Chirp K. Device remains in FS mode.
```

This is entirely handled by the USB PHY hardware; the application firmware does not need
to manage the chirp sequence.

**Performance implications:**

At HS, bulk transfers achieve ~40 MB/s real throughput (480 Mbps gross, ~336 Mbps after
packet overhead, ~42 MB/s, reduced further by USB host scheduling).

At FS, bulk transfers achieve ~1 MB/s (12 Mbps gross, ~1 MB/s net for large blocks).

For mass storage:
- FS: copying a 1 GB file takes ~17 minutes
- HS: copying a 1 GB file takes ~25 seconds

**Descriptor change for HS (device_qualifier_descriptor):**

A HS-capable device must provide a **Device Qualifier Descriptor** describing its parameters
at the OTHER speed (i.e., what it would look like if connected to a FS host). The host
requests this to understand the device fully before negotiating speed.

```c
static const UsbDeviceQualifierDescriptor device_qualifier = {
    .bLength            = 10,
    .bDescriptorType    = 0x06,  /* DEVICE_QUALIFIER */
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = 64,    /* EP0 size at other speed */
    .bNumConfigurations = 1,
    .bReserved          = 0,
};
```

---

### Q8. How do you debug USB enumeration failures?

**Question:** A custom USB device fails to enumerate (device shows as "Unknown USB Device"
in Device Manager). Describe a systematic debugging approach.

**Answer:**

**Step 1: Check the pull-up resistor**

The host detects device presence by the 1.5 kΩ D+ pull-up (FS/HS) or D- pull-up (LS).
Common failure: the pull-up is not present, or the GPIO controlling it (soft-connect) is not
driven. Check with a multimeter: D+ should be approximately 3.0 V with the bus powered.

**Step 2: Capture with a USB protocol analyser**

A logic analyser with USB decoding (Saleae, Total Phase Beagle, or an FPGA-based tool)
captures every packet. The most informative captures for enumeration failures:

```
Look for:
  1. GET_DESCRIPTOR(Device) request from host — does the device respond?
  2. If response present: is bMaxPacketSize0 a valid value (8/16/32/64)?
  3. SET_ADDRESS — does the device ACK correctly?
  4. GET_DESCRIPTOR(Configuration) — is wTotalLength correct?
  5. Are all interface/endpoint descriptors present up to wTotalLength bytes?
  6. Any STALL on EP0 — indicates unhandled GET_DESCRIPTOR request
```

**Step 3: Verify descriptor consistency**

Common descriptor errors that cause silent failures:

```c
/* Error 1: wTotalLength does not match actual descriptor tree size */
config_desc.wTotalLength = 9 + 9 + 7;   /* config + interface + 1 endpoint */
/* But actually there are 2 endpoints: should be 9 + 9 + 7 + 7 = 32 */
/* Host reads 25 bytes, finds garbage in the last endpoint descriptor */

/* Error 2: bNumInterfaces does not match actual number of interface descriptors */
config_desc.bNumInterfaces = 2;   /* says 2 interfaces */
/* But only 1 interface descriptor follows */
/* Host tries to configure interface 1 and gets confused */

/* Error 3: Endpoint address conflicts */
/* Two endpoints both claim address 0x81 — hardware error or descriptor bug */

/* Error 4: bMaxPacketSize0 not valid */
device_desc.bMaxPacketSize0 = 48;   /* invalid: must be 8, 16, 32, or 64 */
```

**Step 4: Check D+ glitch on enumeration**

A common STM32 issue: after the USB peripheral is enabled, D+ briefly goes HIGH then LOW
before stabilising. The host may see this as a disconnect/reconnect and abort enumeration.

Fix: add a 5 ms delay between enabling the USB clock and enabling the D+ pull-up, or use
the soft-connect feature (set the DP_PULL bit only when ready).

**Step 5: Verify stack configuration limits**

Ensure the USB stack's configuration matches the actual descriptors:

```c
/* In usbd_conf.h (STM32 HAL) — must match descriptor */
#define USBD_MAX_NUM_INTERFACES    1   /* must be >= bNumInterfaces */
#define USBD_MAX_NUM_CONFIGURATION 1
#define USBD_MAX_STR_DESC_SIZ      64  /* must fit longest string descriptor */
#define USB_MAX_EP0_SIZE           64  /* must match bMaxPacketSize0 */
```

**Step 6: Test on Linux for verbose error messages**

Windows Device Manager provides minimal information. Linux `dmesg` output is far more
diagnostic:

```
[  42.123] usb 1-1: new full-speed USB device number 4 using xhci_hcd
[  42.345] usb 1-1: device descriptor read/64, error -32   <- STALL on GET_DESCRIPTOR
[  42.567] usb 1-1: can't set config #1, error -32          <- SET_CONFIGURATION failed
[  42.789] usb 1-1: config 1 has an invalid interface number: 1 but max is 0
           ^-- bNumInterfaces = 2 but only 1 interface descriptor present
```

---

### Q9. Implement a USB composite device with both HID and CDC interfaces.

**Question:** How do you create a USB composite device? What additional descriptor is required?
Describe the key configuration changes from a single-class device.

**Answer:**

A composite device presents multiple USB interfaces in a single device, so the host loads
different drivers for different interfaces simultaneously (e.g., a device that is both a
serial port and a joystick).

**Interface Association Descriptor (IAD):**

Required when a function spans multiple interfaces (CDC uses two interfaces). The IAD
groups them so the host knows they belong to the same function:

```c
/* IAD for CDC function (must precede CDC control interface) */
typedef struct __attribute__((packed)) {
    uint8_t bLength;              /* 8 */
    uint8_t bDescriptorType;      /* 0x0B = IAD */
    uint8_t bFirstInterface;      /* index of first interface in this function */
    uint8_t bInterfaceCount;      /* 2 for CDC (control + data) */
    uint8_t bFunctionClass;       /* 0x02 = CDC */
    uint8_t bFunctionSubClass;    /* 0x02 = ACM */
    uint8_t bFunctionProtocol;    /* 0x01 = AT commands */
    uint8_t iFunction;            /* string descriptor index */
} UsbIadDescriptor;
```

**Device descriptor change for composite:**

```c
/* Single class device: */
device_desc.bDeviceClass    = 0x02;   /* CDC */

/* Composite device: */
device_desc.bDeviceClass    = 0xEF;   /* Miscellaneous */
device_desc.bDeviceSubClass = 0x02;   /* Common Class */
device_desc.bDeviceProtocol = 0x01;   /* IAD protocol */
/* These values signal to the host that IADs are present */
```

**Configuration descriptor tree for HID + CDC composite:**

```
Configuration Descriptor (bNumInterfaces=3, wTotalLength=computed)
  |
  +-- Interface [0]: HID (mouse)
  |     Endpoint [0x81]: Interrupt IN, 4 bytes, 1ms
  |
  +-- IAD (bFirstInterface=1, bInterfaceCount=2, CDC class)
  |
  +-- Interface [1]: CDC Control
  |     CDC Functional Descriptors
  |     Endpoint [0x82]: Interrupt IN, 8 bytes, 32ms (notification)
  |
  +-- Interface [2]: CDC Data
        Endpoint [0x83]: Bulk IN
        Endpoint [0x03]: Bulk OUT
```

**Endpoint address allocation (no conflicts!):**

```c
/* HID */
#define HID_IN_EP     0x81   /* EP1 IN */
/* CDC notification */
#define CDC_CMD_EP    0x82   /* EP2 IN (interrupt, low frequency) */
/* CDC data */
#define CDC_IN_EP     0x83   /* EP3 IN (bulk) */
#define CDC_OUT_EP    0x03   /* EP3 OUT (bulk) */
```

**Request routing on EP0:**

With multiple interfaces, the USB stack must route EP0 class-specific requests to the
correct class handler. Route by `wIndex` (interface number in the setup packet):

```c
/* In USBD_Class_Setup() */
USBD_StatusTypeDef USBD_Composite_Setup(USBD_HandleTypeDef *pdev,
                                         USBD_SetupReqTypedef *req)
{
    uint8_t interface = req->wIndex & 0xFF;

    switch (interface) {
    case 0:   /* HID interface */
        return USBD_HID_Setup(pdev, req);

    case 1:   /* CDC Control interface */
    case 2:   /* CDC Data interface */
        return USBD_CDC_Setup(pdev, req);

    default:
        USBD_CtlError(pdev, req);
        return USBD_FAIL;
    }
}
```

---

## Summary Reference Table

| Feature | Control (EP0) | Bulk | Interrupt | Isochronous |
|---------|--------------|------|-----------|-------------|
| Max packet (FS) | 64 bytes | 64 bytes | 64 bytes | 1023 bytes |
| Max packet (HS) | 64 bytes | 512 bytes | 1024 bytes | 1024 bytes |
| Error correction | Yes, retry | Yes, retry | Yes, retry | No retry |
| Bandwidth guarantee | Yes (10%) | No (best effort) | Yes (polling) | Yes (reserved) |
| Latency guarantee | No | No | Yes (bInterval) | Yes (1 frame/µframe) |
| Typical use | Enumeration, class req | Mass storage, CDC data | HID, CDC notify | Audio, video |
| ZLP required | Yes (if exact multiple) | Yes (if exact multiple) | No | No |
| Class (CDC) | Endpoint 0 | CDC_IN / CDC_OUT | CDC_CMD | N/A |
| Class (HID) | Endpoint 0 | N/A | HID_IN | N/A |
| Class (MSC) | Endpoint 0 | MSC_IN / MSC_OUT | N/A | N/A |
| Class (Audio) | Endpoint 0 | N/A | N/A | Audio streaming |
