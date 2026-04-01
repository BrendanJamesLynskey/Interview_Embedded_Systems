/*
 * Challenge 02: SPI Flash Driver
 *
 * Task: implement a driver for a W25Qxx-compatible NOR flash chip.
 *
 * W25Q64 (Winbond) overview:
 *   - 8 MB (64 Mbit) NOR flash
 *   - SPI Mode 0 (CPOL=0, CPHA=0), up to 104 MHz
 *   - Erase sizes: sector (4 KB), half-block (32 KB), block (64 KB), chip
 *   - Page size: 256 bytes (write must not cross page boundary)
 *   - Status register: BUSY bit, WEL bit, protect bits
 *
 * Key commands:
 *   0x06  WRITE_ENABLE   (must be sent before every write or erase)
 *   0x04  WRITE_DISABLE
 *   0x05  READ_STATUS_REGISTER_1
 *   0x03  READ_DATA         (addr[23:0] then Nx data bytes, any length)
 *   0x02  PAGE_PROGRAM      (addr[23:0] then 1-256 bytes; must not cross 256B boundary)
 *   0x20  SECTOR_ERASE_4KB  (addr[23:0])
 *   0x52  BLOCK_ERASE_32KB  (addr[23:0])
 *   0xD8  BLOCK_ERASE_64KB  (addr[23:0])
 *   0xC7  CHIP_ERASE
 *   0x9F  READ_JEDEC_ID     (returns 3 bytes: manufacturer, memory type, capacity)
 *
 * Status Register 1:
 *   Bit 0 (BUSY): 1 = device is busy (write or erase in progress)
 *   Bit 1 (WEL):  1 = write enable latch is set
 *
 * Compile and run tests:
 *   gcc -std=c11 -Wall -Wextra -o challenge_02 challenge_02_spi_flash_driver.c && ./challenge_02
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>

/* =========================================================================
 * SPI HAL interface
 *
 * Real implementations call the MCU's SPI peripheral:
 *   e.g., HAL_SPI_TransmitReceive() on STM32.
 *
 * The SPI flash always operates full-duplex: every byte clocked out on MOSI
 * produces a byte on MISO simultaneously.  For write-only phases the MISO
 * byte is discarded; for read-only phases 0xFF (or 0x00) is clocked out on
 * MOSI.
 * =========================================================================*/

typedef struct SpiHal SpiHal;

/* Transfer 'len' bytes full-duplex.
 * tx: bytes to send (may be NULL -> send 0xFF)
 * rx: buffer for received bytes (may be NULL -> discard)
 * Returns 0 on success. */
typedef int (*SpiHalTransfer)(SpiHal *hal,
                               const uint8_t *tx, uint8_t *rx,
                               uint32_t len);

/* Assert chip-select (cs=true -> CS=LOW) */
typedef void (*SpiHalCsSet)(SpiHal *hal, bool cs);

struct SpiHal {
    SpiHalTransfer  transfer;
    SpiHalCsSet     cs_set;
    void           *platform_data;
};

/* =========================================================================
 * W25Qxx Flash Driver
 * =========================================================================*/

#define FLASH_CMD_WRITE_ENABLE     0x06
#define FLASH_CMD_WRITE_DISABLE    0x04
#define FLASH_CMD_READ_STATUS1     0x05
#define FLASH_CMD_READ_DATA        0x03
#define FLASH_CMD_PAGE_PROGRAM     0x02
#define FLASH_CMD_SECTOR_ERASE_4K  0x20
#define FLASH_CMD_BLOCK_ERASE_32K  0x52
#define FLASH_CMD_BLOCK_ERASE_64K  0xD8
#define FLASH_CMD_CHIP_ERASE       0xC7
#define FLASH_CMD_READ_JEDEC_ID    0x9F

#define FLASH_STATUS_BUSY_BIT      0x01
#define FLASH_STATUS_WEL_BIT       0x02

#define FLASH_PAGE_SIZE            256u
#define FLASH_SECTOR_SIZE          (4u   * 1024u)
#define FLASH_BLOCK_32K_SIZE       (32u  * 1024u)
#define FLASH_BLOCK_64K_SIZE       (64u  * 1024u)

/* Timeout constants (ms) for waiting on BUSY — increase for real hardware */
#define FLASH_TIMEOUT_PAGE_PROG_MS    10
#define FLASH_TIMEOUT_SECTOR_ERASE_MS 400
#define FLASH_TIMEOUT_BLOCK_ERASE_MS  2000
#define FLASH_TIMEOUT_CHIP_ERASE_MS   25000

typedef struct {
    SpiHal  *hal;
    uint32_t capacity_bytes;   /* total device capacity */
} W25q;

/* -------------------------------------------------------------------------
 * Internal: send a command byte with no data phase
 * -------------------------------------------------------------------------*/
static int flash_cmd(W25q *dev, uint8_t cmd)
{
    dev->hal->cs_set(dev->hal, true);
    int err = dev->hal->transfer(dev->hal, &cmd, NULL, 1);
    dev->hal->cs_set(dev->hal, false);
    return err;
}

/* -------------------------------------------------------------------------
 * Internal: read status register 1
 * -------------------------------------------------------------------------*/
static int flash_read_status(W25q *dev, uint8_t *status)
{
    uint8_t tx[2] = { FLASH_CMD_READ_STATUS1, 0xFF };
    uint8_t rx[2] = { 0 };

    dev->hal->cs_set(dev->hal, true);
    int err = dev->hal->transfer(dev->hal, tx, rx, 2);
    dev->hal->cs_set(dev->hal, false);

    if (err == 0) *status = rx[1];
    return err;
}

/* -------------------------------------------------------------------------
 * Internal: poll BUSY bit with a tick counter.
 * tick_fn: called each iteration; returns elapsed_ms; pass NULL if not needed.
 * timeout_ms: total time limit.
 * Returns 0 when not busy, -1 on timeout.
 * -------------------------------------------------------------------------*/
static int flash_wait_not_busy(W25q *dev, uint32_t timeout_ms)
{
    /* In bare-metal firmware, replace this with a hardware timer or SysTick.
     * For the stub (PC test), we loop until the stub clears BUSY. */
    for (uint32_t i = 0; i < timeout_ms * 10; i++) {
        uint8_t status = 0;
        if (flash_read_status(dev, &status) != 0) return -1;
        if (!(status & FLASH_STATUS_BUSY_BIT))    return 0;
    }
    return -1;   /* timeout */
}

/* -------------------------------------------------------------------------
 * w25q_init
 *
 * Reads the JEDEC ID to verify the device is present.
 * Returns 0 on success, negative on error.
 * -------------------------------------------------------------------------*/
int w25q_init(W25q *dev, SpiHal *hal)
{
    if (!dev || !hal) return -1;
    dev->hal            = hal;
    dev->capacity_bytes = 0;

    /* Read JEDEC ID: 1 command byte + 3 response bytes */
    uint8_t tx[4] = { FLASH_CMD_READ_JEDEC_ID, 0xFF, 0xFF, 0xFF };
    uint8_t rx[4] = { 0 };

    hal->cs_set(hal, true);
    int err = hal->transfer(hal, tx, rx, 4);
    hal->cs_set(hal, false);
    if (err != 0) return err;

    /*
     * rx[1] = manufacturer (Winbond = 0xEF)
     * rx[2] = memory type  (SPI NOR = 0x40 for standard density)
     * rx[3] = capacity     (0x17 = 64 Mbit = 8 MB)
     *                      (0x16 = 32 Mbit = 4 MB)
     *                      (0x15 = 16 Mbit = 2 MB)
     */
    if (rx[1] == 0xEF && rx[3] >= 0x11 && rx[3] <= 0x1B) {
        dev->capacity_bytes = 1u << rx[3];   /* 2^capacity_code bytes */
        return 0;
    }
    return -2;   /* unrecognised device */
}

/* -------------------------------------------------------------------------
 * w25q_read
 *
 * Reads 'len' bytes from flash address 'addr' into 'buf'.
 * No alignment or size restrictions.
 * Returns 0 on success.
 * -------------------------------------------------------------------------*/
int w25q_read(W25q *dev, uint32_t addr, uint8_t *buf, uint32_t len)
{
    if (!buf || len == 0) return -1;
    if (addr + len > dev->capacity_bytes) return -2;  /* out of range */

    /* Wait for any pending operation */
    if (flash_wait_not_busy(dev, FLASH_TIMEOUT_CHIP_ERASE_MS) != 0) return -3;

    /* Build command: 0x03 + 24-bit address */
    uint8_t cmd[4] = {
        FLASH_CMD_READ_DATA,
        (uint8_t)(addr >> 16),
        (uint8_t)(addr >>  8),
        (uint8_t)(addr      )
    };

    dev->hal->cs_set(dev->hal, true);
    int err = dev->hal->transfer(dev->hal, cmd, NULL, 4);   /* send command+addr */
    if (err == 0) {
        err = dev->hal->transfer(dev->hal, NULL, buf, len); /* read data */
    }
    dev->hal->cs_set(dev->hal, false);
    return err;
}

/* -------------------------------------------------------------------------
 * w25q_write_enable
 *
 * Sends WRITE_ENABLE and verifies the WEL bit is set.
 * Must be called before every page program or erase.
 * Returns 0 on success.
 * -------------------------------------------------------------------------*/
int w25q_write_enable(W25q *dev)
{
    int err = flash_cmd(dev, FLASH_CMD_WRITE_ENABLE);
    if (err != 0) return err;

    /* Verify WEL bit is set */
    uint8_t status = 0;
    err = flash_read_status(dev, &status);
    if (err != 0) return err;
    if (!(status & FLASH_STATUS_WEL_BIT)) return -1;  /* write enable failed */
    return 0;
}

/* -------------------------------------------------------------------------
 * w25q_page_program
 *
 * Programs up to 256 bytes starting at 'addr'.
 * Caller must ensure:
 *   1. The target region has been erased (all 0xFF) beforehand.
 *   2. The write does NOT cross a 256-byte page boundary.
 *      (i.e., (addr & 0xFF) + len <= 256)
 *   3. WRITE_ENABLE was sent before this call (or call w25q_write_enable here).
 *
 * Returns 0 on success.
 * -------------------------------------------------------------------------*/
int w25q_page_program(W25q *dev, uint32_t addr, const uint8_t *data, uint32_t len)
{
    if (!data || len == 0 || len > FLASH_PAGE_SIZE) return -1;
    if (addr + len > dev->capacity_bytes) return -2;
    /* Check page boundary: start + len must not cross 256-byte boundary */
    if (((addr & 0xFF) + len) > FLASH_PAGE_SIZE) return -3;

    /* WRITE_ENABLE must be set before page program */
    int err = w25q_write_enable(dev);
    if (err != 0) return err;

    /* Build header: command + 3-byte address */
    uint8_t header[4] = {
        FLASH_CMD_PAGE_PROGRAM,
        (uint8_t)(addr >> 16),
        (uint8_t)(addr >>  8),
        (uint8_t)(addr      )
    };

    dev->hal->cs_set(dev->hal, true);
    err = dev->hal->transfer(dev->hal, header, NULL, 4);   /* command+addr */
    if (err == 0) {
        err = dev->hal->transfer(dev->hal, data, NULL, len);  /* data */
    }
    dev->hal->cs_set(dev->hal, false);
    if (err != 0) return err;

    /* Wait for programming to complete (W25Q64: max 3 ms per page) */
    return flash_wait_not_busy(dev, FLASH_TIMEOUT_PAGE_PROG_MS);
}

/* -------------------------------------------------------------------------
 * w25q_write
 *
 * High-level write: writes 'len' bytes to 'addr', automatically splitting
 * across page boundaries. Handles the case where len > 256 or addr is not
 * page-aligned.
 *
 * Does NOT erase — the caller must erase the region first.
 * Returns 0 on success.
 * -------------------------------------------------------------------------*/
int w25q_write(W25q *dev, uint32_t addr, const uint8_t *data, uint32_t len)
{
    if (!data || len == 0) return -1;
    if (addr + len > dev->capacity_bytes) return -2;

    uint32_t remaining = len;
    uint32_t offset    = 0;

    while (remaining > 0) {
        /* How many bytes can we write in the current page? */
        uint32_t page_offset   = addr & (FLASH_PAGE_SIZE - 1);
        uint32_t bytes_in_page = FLASH_PAGE_SIZE - page_offset;
        uint32_t chunk         = (remaining < bytes_in_page) ? remaining : bytes_in_page;

        int err = w25q_page_program(dev, addr, data + offset, chunk);
        if (err != 0) return err;

        addr      += chunk;
        offset    += chunk;
        remaining -= chunk;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * w25q_erase_sector_4k
 *
 * Erases one 4 KB sector containing 'addr'.
 * The address is aligned down to the nearest 4 KB boundary internally.
 * After erase, all bytes in the sector read as 0xFF.
 * -------------------------------------------------------------------------*/
int w25q_erase_sector_4k(W25q *dev, uint32_t addr)
{
    addr &= ~(FLASH_SECTOR_SIZE - 1);   /* align to 4KB boundary */
    if (addr >= dev->capacity_bytes) return -1;

    int err = w25q_write_enable(dev);
    if (err != 0) return err;

    uint8_t cmd[4] = {
        FLASH_CMD_SECTOR_ERASE_4K,
        (uint8_t)(addr >> 16),
        (uint8_t)(addr >>  8),
        (uint8_t)(addr      )
    };
    dev->hal->cs_set(dev->hal, true);
    err = dev->hal->transfer(dev->hal, cmd, NULL, 4);
    dev->hal->cs_set(dev->hal, false);
    if (err != 0) return err;

    return flash_wait_not_busy(dev, FLASH_TIMEOUT_SECTOR_ERASE_MS);
}

/* -------------------------------------------------------------------------
 * w25q_erase_block_64k
 *
 * Erases a 64 KB block containing 'addr'.
 * -------------------------------------------------------------------------*/
int w25q_erase_block_64k(W25q *dev, uint32_t addr)
{
    addr &= ~(FLASH_BLOCK_64K_SIZE - 1);
    if (addr >= dev->capacity_bytes) return -1;

    int err = w25q_write_enable(dev);
    if (err != 0) return err;

    uint8_t cmd[4] = {
        FLASH_CMD_BLOCK_ERASE_64K,
        (uint8_t)(addr >> 16),
        (uint8_t)(addr >>  8),
        (uint8_t)(addr      )
    };
    dev->hal->cs_set(dev->hal, true);
    err = dev->hal->transfer(dev->hal, cmd, NULL, 4);
    dev->hal->cs_set(dev->hal, false);
    if (err != 0) return err;

    return flash_wait_not_busy(dev, FLASH_TIMEOUT_BLOCK_ERASE_MS);
}

/* =========================================================================
 * Stub SPI HAL — simulates a W25Q64 in an 8 MB RAM buffer for testing
 * =========================================================================*/

#define STUB_FLASH_SIZE (8u * 1024u * 1024u)   /* 8 MB */

typedef struct {
    uint8_t  memory[STUB_FLASH_SIZE];
    uint8_t  status1;          /* bits: BUSY(0), WEL(1) */
    uint32_t busy_countdown;   /* simulate async erase/program completion */
    bool     cs_active;
    /* State machine for tracking the current command */
    uint8_t  current_cmd;
    uint32_t current_addr;
    uint32_t byte_count;       /* bytes transferred since CS assert */
    uint8_t  write_buf[256];   /* page program buffer */
    uint32_t write_buf_idx;
} StubFlash;

static StubFlash stub_flash;

static void stub_flash_init(void)
{
    memset(&stub_flash, 0, sizeof(stub_flash));     /* zero all fields */
    memset(stub_flash.memory, 0xFF, STUB_FLASH_SIZE); /* erased state = 0xFF */
    stub_flash.status1       = 0x00;
    stub_flash.cs_active     = false;
    stub_flash.byte_count    = 0;
    stub_flash.write_buf_idx = 0;
}

static void stub_cs_set(SpiHal *hal, bool cs)
{
    (void)hal;
    StubFlash *f = &stub_flash;

    if (cs && !f->cs_active) {
        /* CS assert: start new transaction */
        f->cs_active   = true;
        f->byte_count  = 0;
        f->current_cmd = 0;
        f->write_buf_idx = 0;
    } else if (!cs && f->cs_active) {
        /* CS deassert: finish transaction */
        f->cs_active = false;

        if (f->current_cmd == FLASH_CMD_PAGE_PROGRAM && f->write_buf_idx > 0) {
            /* Commit page program to memory */
            /* NOR flash: can only write 0 bits (cannot set 0->1 without erase) */
            for (uint32_t i = 0; i < f->write_buf_idx; i++) {
                f->memory[f->current_addr + i] &= f->write_buf[i];
            }
            f->status1 &= ~FLASH_STATUS_WEL_BIT;   /* WEL cleared after program */
            f->status1 &= ~FLASH_STATUS_BUSY_BIT;  /* immediately done for stub */
        } else if (f->current_cmd == FLASH_CMD_SECTOR_ERASE_4K) {
            /* Erase sector: set all bytes to 0xFF */
            uint32_t aligned = f->current_addr & ~(FLASH_SECTOR_SIZE - 1);
            memset(&f->memory[aligned], 0xFF, FLASH_SECTOR_SIZE);
            f->status1 &= ~FLASH_STATUS_WEL_BIT;
            f->status1 &= ~FLASH_STATUS_BUSY_BIT;
        } else if (f->current_cmd == FLASH_CMD_BLOCK_ERASE_64K) {
            uint32_t aligned = f->current_addr & ~(FLASH_BLOCK_64K_SIZE - 1);
            memset(&f->memory[aligned], 0xFF, FLASH_BLOCK_64K_SIZE);
            f->status1 &= ~FLASH_STATUS_WEL_BIT;
            f->status1 &= ~FLASH_STATUS_BUSY_BIT;
        } else if (f->current_cmd == FLASH_CMD_CHIP_ERASE) {
            memset(f->memory, 0xFF, STUB_FLASH_SIZE);
            f->status1 &= ~FLASH_STATUS_WEL_BIT;
            f->status1 &= ~FLASH_STATUS_BUSY_BIT;
        }
    }
}

static int stub_spi_transfer(SpiHal *hal, const uint8_t *tx, uint8_t *rx, uint32_t len)
{
    (void)hal;
    StubFlash *f = &stub_flash;

    for (uint32_t i = 0; i < len; i++) {
        uint8_t tx_byte = tx ? tx[i] : 0xFF;
        uint8_t rx_byte = 0xFF;

        if (f->byte_count == 0) {
            /* First byte is always the command opcode */
            f->current_cmd  = tx_byte;
            f->current_addr = 0;

            /* Handle zero-data-phase commands immediately */
            if (f->current_cmd == FLASH_CMD_WRITE_ENABLE) {
                f->status1 |= FLASH_STATUS_WEL_BIT;
            } else if (f->current_cmd == FLASH_CMD_WRITE_DISABLE) {
                f->status1 &= ~FLASH_STATUS_WEL_BIT;
            } else if (f->current_cmd == FLASH_CMD_CHIP_ERASE) {
                /* Handled at CS deassert */
            }
        } else {
            /* Determine if this command uses a 3-byte address header */
            bool has_addr = (f->current_cmd == FLASH_CMD_READ_DATA        ||
                             f->current_cmd == FLASH_CMD_PAGE_PROGRAM     ||
                             f->current_cmd == FLASH_CMD_SECTOR_ERASE_4K  ||
                             f->current_cmd == FLASH_CMD_BLOCK_ERASE_32K  ||
                             f->current_cmd == FLASH_CMD_BLOCK_ERASE_64K);

            if (has_addr && f->byte_count <= 3) {
                /* Bytes 1-3: build the 24-bit address (MSB first) */
                f->current_addr = (f->current_addr << 8) | tx_byte;
            } else {
                /* Data phase (byte_count >= 4 for addr-commands, >= 1 for others) */
                switch (f->current_cmd) {
                case FLASH_CMD_READ_STATUS1:
                    /* byte_count 1 = dummy; reply is status byte */
                    rx_byte = f->status1;
                    break;

                case FLASH_CMD_READ_DATA:
                    rx_byte = f->memory[f->current_addr];
                    f->current_addr++;
                    break;

                case FLASH_CMD_PAGE_PROGRAM:
                    if (f->write_buf_idx < FLASH_PAGE_SIZE) {
                        f->write_buf[f->write_buf_idx++] = tx_byte;
                    }
                    break;

                case FLASH_CMD_READ_JEDEC_ID:
                    /* 3 response bytes: manufacturer, type, capacity */
                    if      (f->byte_count == 1) rx_byte = 0xEF;  /* Winbond */
                    else if (f->byte_count == 2) rx_byte = 0x40;  /* SPI NOR */
                    else if (f->byte_count == 3) rx_byte = 0x17;  /* 64 Mbit */
                    break;

                default:
                    break;
                }
            }
        }

        if (rx) rx[i] = rx_byte;
        f->byte_count++;
    }
    return 0;
}

static SpiHal make_stub_hal(void)
{
    SpiHal hal = {
        .transfer      = stub_spi_transfer,
        .cs_set        = stub_cs_set,
        .platform_data = NULL
    };
    return hal;
}

/* =========================================================================
 * Test harness
 * =========================================================================*/

static int tests_run    = 0;
static int tests_passed = 0;

static void check(const char *label, int cond)
{
    tests_run++;
    if (cond) {
        tests_passed++;
        printf("  PASS  %s\n", label);
    } else {
        printf("  FAIL  %s\n", label);
    }
}

static void test_init(void)
{
    printf("=== Initialisation (JEDEC ID) ===\n");
    stub_flash_init();
    SpiHal hal = make_stub_hal();
    W25q flash;
    int err = w25q_init(&flash, &hal);
    check("init returns 0", err == 0);
    check("capacity = 8 MB", flash.capacity_bytes == 8u * 1024u * 1024u);
}

static void test_read_erased(void)
{
    printf("\n=== Read erased flash (all 0xFF) ===\n");
    stub_flash_init();
    SpiHal hal = make_stub_hal();
    W25q flash;
    w25q_init(&flash, &hal);

    uint8_t buf[16];
    memset(buf, 0xAA, sizeof(buf));   /* fill with non-0xFF */
    int err = w25q_read(&flash, 0x000000, buf, sizeof(buf));
    check("read returns 0", err == 0);

    int all_ff = 1;
    for (int i = 0; i < 16; i++) if (buf[i] != 0xFF) { all_ff = 0; break; }
    check("erased region reads as 0xFF", all_ff);
}

static void test_write_and_read(void)
{
    printf("\n=== Write (page program) and read ===\n");
    stub_flash_init();
    SpiHal hal = make_stub_hal();
    W25q flash;
    w25q_init(&flash, &hal);

    uint8_t write_buf[16];
    for (int i = 0; i < 16; i++) write_buf[i] = (uint8_t)(i * 10);

    int err = w25q_page_program(&flash, 0x001000, write_buf, sizeof(write_buf));
    check("page_program returns 0", err == 0);

    uint8_t read_buf[16];
    memset(read_buf, 0x00, sizeof(read_buf));
    err = w25q_read(&flash, 0x001000, read_buf, sizeof(read_buf));
    check("read back returns 0", err == 0);
    check("read back matches written data", memcmp(write_buf, read_buf, 16) == 0);
}

static void test_erase_sector(void)
{
    printf("\n=== Sector erase (4 KB) ===\n");
    stub_flash_init();
    SpiHal hal = make_stub_hal();
    W25q flash;
    w25q_init(&flash, &hal);

    /* Write a pattern to sector 1 (0x001000) */
    uint8_t pattern[64];
    memset(pattern, 0x5A, sizeof(pattern));
    w25q_page_program(&flash, 0x001000, pattern, sizeof(pattern));

    /* Verify written */
    uint8_t verify[64];
    w25q_read(&flash, 0x001000, verify, sizeof(verify));
    check("data written before erase", memcmp(pattern, verify, 64) == 0);

    /* Erase sector containing 0x001000 (sector 1: 0x001000-0x001FFF) */
    int err = w25q_erase_sector_4k(&flash, 0x001000);
    check("erase_sector_4k returns 0", err == 0);

    /* Verify erased */
    memset(verify, 0xAA, sizeof(verify));
    w25q_read(&flash, 0x001000, verify, sizeof(verify));
    int all_ff = 1;
    for (int i = 0; i < 64; i++) if (verify[i] != 0xFF) { all_ff = 0; break; }
    check("sector reads 0xFF after erase", all_ff);

    /* Verify adjacent sector is untouched */
    uint8_t adj[16];
    w25q_read(&flash, 0x000FF0, adj, sizeof(adj));  /* last 16 bytes of sector 0 */
    int adj_ff = 1;
    for (int i = 0; i < 16; i++) if (adj[i] != 0xFF) { adj_ff = 0; break; }
    check("adjacent sector not erased", adj_ff);
}

static void test_write_spans_page_boundary(void)
{
    printf("\n=== Write spanning page boundary (w25q_write auto-split) ===\n");
    stub_flash_init();
    SpiHal hal = make_stub_hal();
    W25q flash;
    w25q_init(&flash, &hal);

    /* Write 300 bytes starting at offset 200 within a page.
     * Page 0: bytes 0-255. Offset 200 means 56 bytes fit in page 0,
     * remaining 244 bytes go into page 1 (bytes 256-499). */
    uint8_t large_buf[300];
    for (int i = 0; i < 300; i++) large_buf[i] = (uint8_t)(i & 0xFF);

    int err = w25q_write(&flash, 0x0000C8, large_buf, 300);
    check("w25q_write (spanning pages) returns 0", err == 0);

    uint8_t readback[300];
    w25q_read(&flash, 0x0000C8, readback, 300);
    check("readback matches across page boundary", memcmp(large_buf, readback, 300) == 0);
}

static void test_nor_write_semantics(void)
{
    printf("\n=== NOR flash write semantics (can only clear bits, not set) ===\n");
    stub_flash_init();
    SpiHal hal = make_stub_hal();
    W25q flash;
    w25q_init(&flash, &hal);

    /* Write 0xFF (all 1s) — no change to erased flash */
    uint8_t all_ones[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    w25q_page_program(&flash, 0x000000, all_ones, 4);

    uint8_t r[4];
    w25q_read(&flash, 0x000000, r, 4);
    check("writing 0xFF to erased flash leaves 0xFF", r[0] == 0xFF);

    /* Write 0xAA (clears bits 1,3,5,7) */
    uint8_t half_zeros[4] = {0xAA, 0xAA, 0xAA, 0xAA};
    w25q_page_program(&flash, 0x000000, half_zeros, 4);
    w25q_read(&flash, 0x000000, r, 4);
    check("writing 0xAA to 0xFF gives 0xAA", r[0] == 0xAA);

    /* Write 0x55 (attempts to set bits 1,3,5,7 which are 0) — NOR cannot set bits */
    uint8_t other_half[4] = {0x55, 0x55, 0x55, 0x55};
    w25q_page_program(&flash, 0x000000, other_half, 4);
    w25q_read(&flash, 0x000000, r, 4);
    /* 0xAA & 0x55 = 0x00: both patterns applied, all bits cleared */
    check("NOR AND semantics: 0xAA & 0x55 = 0x00 (must erase to set bits)", r[0] == 0x00);
}

static void test_out_of_range(void)
{
    printf("\n=== Out-of-range address checks ===\n");
    stub_flash_init();
    SpiHal hal = make_stub_hal();
    W25q flash;
    w25q_init(&flash, &hal);

    uint8_t buf[8] = {0};
    /* Read past end of flash */
    int err = w25q_read(&flash, 0x7FFFFF, buf, 8);
    check("read past end of flash returns error", err != 0);

    /* Page program past end */
    err = w25q_page_program(&flash, 0x800000, buf, 1);
    check("page_program at capacity returns error", err != 0);
}

int main(void)
{
    test_init();
    test_read_erased();
    test_write_and_read();
    test_erase_sector();
    test_write_spans_page_boundary();
    test_nor_write_semantics();
    test_out_of_range();

    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
