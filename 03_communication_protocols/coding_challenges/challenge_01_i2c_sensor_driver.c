/*
 * Challenge 01: I2C Sensor Driver
 *
 * Task: implement a driver for the TMP102 digital temperature sensor.
 *
 * TMP102 (Texas Instruments) overview:
 *   - I2C address: 0x48 (ADD0 pin to GND), 0x49, 0x4A, or 0x4B
 *   - Registers:
 *       0x00  Temperature Register (read-only, 12-bit or 13-bit, 2's complement)
 *       0x01  Configuration Register (read/write, 16-bit)
 *       0x02  T_LOW  register (alert low threshold)
 *       0x03  T_HIGH register (alert high threshold)
 *   - Temperature register format (12-bit normal mode):
 *       Byte 0 (MSB): bits [11:4] of temperature
 *       Byte 1 (LSB): bits [3:0] in upper nibble, lower nibble = 0
 *       Temperature = (raw_value >> 4) * 0.0625 °C
 *       Negative temperatures use 12-bit two's complement.
 *
 * This driver implements a HAL abstraction layer:
 *   - A platform-independent driver layer (tmp102.c)
 *   - A HAL interface (i2c_hal.h) the caller must provide for their platform
 *   - A host-side simulation (stub_i2c_hal.c) for unit-testing on a PC
 *
 * Compile and run tests:
 *   gcc -std=c11 -Wall -Wextra -o challenge_01 challenge_01_i2c_sensor_driver.c && ./challenge_01
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>

/* =========================================================================
 * I2C HAL interface
 *
 * Real targets provide an implementation that calls the MCU's I2C peripheral
 * (e.g., HAL_I2C_Master_Transmit / HAL_I2C_Master_Receive on STM32).
 *
 * Rules:
 *   - All functions return 0 on success, negative error code on failure.
 *   - timeout_ms: how long to wait for the I2C bus before giving up.
 *   - The HAL is responsible for: generating START, address byte, data bytes,
 *     STOP, and repeated START (for write-then-read sequences).
 * =========================================================================*/

typedef struct I2cHal I2cHal;

/* Read 'len' bytes from 'dev_addr' (7-bit) into 'buf'.
 * Performs: START | addr<<1|1 | data[0..len-1] | STOP */
typedef int (*I2cHalRead)(I2cHal *hal, uint8_t dev_addr,
                           uint8_t *buf, uint8_t len, uint32_t timeout_ms);

/* Write 'len' bytes from 'buf' to 'dev_addr'.
 * Performs: START | addr<<1|0 | data[0..len-1] | STOP */
typedef int (*I2cHalWrite)(I2cHal *hal, uint8_t dev_addr,
                            const uint8_t *buf, uint8_t len, uint32_t timeout_ms);

/* Write 'wlen' bytes then read 'rlen' bytes using a repeated START.
 * Performs: START | addr<<1|0 | wdata[0..wlen-1] | Sr | addr<<1|1 | rdata[0..rlen-1] | STOP
 * Used for register-addressed reads. */
typedef int (*I2cHalWriteRead)(I2cHal *hal, uint8_t dev_addr,
                                const uint8_t *wbuf, uint8_t wlen,
                                uint8_t *rbuf,  uint8_t rlen,
                                uint32_t timeout_ms);

struct I2cHal {
    I2cHalRead      read;
    I2cHalWrite     write;
    I2cHalWriteRead write_read;
    void           *platform_data;  /* opaque pointer (e.g., I2C_HandleTypeDef *) */
};

/* =========================================================================
 * TMP102 Driver
 * =========================================================================*/

/* TMP102 register addresses */
#define TMP102_REG_TEMP    0x00
#define TMP102_REG_CONFIG  0x01
#define TMP102_REG_TLOW    0x02
#define TMP102_REG_THIGH   0x03

/* Configuration register bit masks */
#define TMP102_CFG_SD      (1 << 8)    /* Shutdown mode */
#define TMP102_CFG_TM      (1 << 9)    /* Thermostat mode: 0=comparator, 1=interrupt */
#define TMP102_CFG_POL     (1 << 10)   /* Alert polarity: 0=active-low */
#define TMP102_CFG_F0      (1 << 11)   /* Fault queue: bits F1:F0 */
#define TMP102_CFG_F1      (1 << 12)
#define TMP102_CFG_R0      (1 << 13)   /* Conversion resolution (read-only) */
#define TMP102_CFG_R1      (1 << 14)
#define TMP102_CFG_OS      (1 << 15)   /* One-shot / conversion ready */
#define TMP102_CFG_EM      (1 << 4)    /* Extended mode (13-bit) */

typedef struct {
    I2cHal  *hal;
    uint8_t  dev_addr;     /* 7-bit I2C address */
    bool     extended_mode; /* true = 13-bit temperature (EM bit) */
} Tmp102;

/* -------------------------------------------------------------------------
 * Internal helper: read a 16-bit register (MSB first)
 * -------------------------------------------------------------------------*/
static int tmp102_read_reg16(Tmp102 *dev, uint8_t reg, uint16_t *value)
{
    uint8_t reg_buf[1]  = { reg };
    uint8_t data_buf[2] = { 0, 0 };

    int err = dev->hal->write_read(dev->hal,
                                   dev->dev_addr,
                                   reg_buf,  1,
                                   data_buf, 2,
                                   10);
    if (err != 0) return err;

    *value = ((uint16_t)data_buf[0] << 8) | data_buf[1];
    return 0;
}

/* -------------------------------------------------------------------------
 * Internal helper: write a 16-bit register (MSB first)
 * -------------------------------------------------------------------------*/
static int tmp102_write_reg16(Tmp102 *dev, uint8_t reg, uint16_t value)
{
    uint8_t buf[3] = {
        reg,
        (uint8_t)(value >> 8),
        (uint8_t)(value & 0xFF)
    };
    return dev->hal->write(dev->hal, dev->dev_addr, buf, 3, 10);
}

/* -------------------------------------------------------------------------
 * tmp102_init
 *
 * Initialises the driver struct and configures the sensor.
 * Returns 0 on success, negative on I2C error.
 * -------------------------------------------------------------------------*/
int tmp102_init(Tmp102 *dev, I2cHal *hal, uint8_t addr)
{
    if (!dev || !hal) return -1;

    dev->hal            = hal;
    dev->dev_addr       = addr;
    dev->extended_mode  = false;

    /* Read config register to verify device responds */
    uint16_t config = 0;
    int err = tmp102_read_reg16(dev, TMP102_REG_CONFIG, &config);
    if (err != 0) return err;

    /* Default config: normal mode, comparator, 4 consecutive fault readings,
     * continuous conversion, active-low alert. No changes needed for basic use. */
    return 0;
}

/* -------------------------------------------------------------------------
 * tmp102_read_raw
 *
 * Reads the 12-bit (normal) or 13-bit (extended) raw temperature value.
 * The raw value is a two's-complement integer in units of 0.0625 °C.
 *
 * Returns 0 on success.
 * -------------------------------------------------------------------------*/
int tmp102_read_raw(Tmp102 *dev, int16_t *raw_out)
{
    uint16_t reg_val = 0;
    int err = tmp102_read_reg16(dev, TMP102_REG_TEMP, &reg_val);
    if (err != 0) return err;

    if (dev->extended_mode) {
        /* 13-bit two's complement: bits [15:3], shift right by 3 */
        int16_t raw = (int16_t)(reg_val) >> 3;
        *raw_out = raw;
    } else {
        /* 12-bit two's complement: bits [15:4], shift right by 4 */
        int16_t raw = (int16_t)(reg_val) >> 4;
        *raw_out = raw;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * tmp102_read_celsius_x16
 *
 * Returns temperature as integer in units of 1/16 °C to avoid floating-point.
 * Divide by 16 to get °C.
 *
 * Example: return value 200 => 200/16 = 12.5 °C
 *          return value -32 => -32/16 = -2.0 °C
 * -------------------------------------------------------------------------*/
int tmp102_read_celsius_x16(Tmp102 *dev, int32_t *celsius_x16_out)
{
    int16_t raw;
    int err = tmp102_read_raw(dev, &raw);
    if (err != 0) return err;

    /*
     * Each LSB of the 12-bit value equals 0.0625 °C = 1/16 °C.
     * So the raw value directly IS the temperature in 1/16 °C units.
     * No scaling needed.
     */
    *celsius_x16_out = (int32_t)raw;
    return 0;
}

/* -------------------------------------------------------------------------
 * tmp102_set_alert_thresholds
 *
 * Sets T_LOW and T_HIGH alert thresholds in integer degrees Celsius.
 * The alert pin goes active when temperature exceeds T_HIGH;
 * deasserts when temperature falls below T_LOW (in comparator mode).
 * -------------------------------------------------------------------------*/
int tmp102_set_alert_thresholds(Tmp102 *dev, int8_t t_low_c, int8_t t_high_c)
{
    /* Convert integer °C to 12-bit register format (left-justified in 16 bits) */
    /* raw = degrees * 16 (LSB = 0.0625°C) */
    uint16_t t_low_reg  = (uint16_t)((int16_t)t_low_c  * 16) << 4;
    uint16_t t_high_reg = (uint16_t)((int16_t)t_high_c * 16) << 4;

    int err = tmp102_write_reg16(dev, TMP102_REG_TLOW,  t_low_reg);
    if (err != 0) return err;
    return      tmp102_write_reg16(dev, TMP102_REG_THIGH, t_high_reg);
}

/* -------------------------------------------------------------------------
 * tmp102_set_shutdown
 *
 * Puts the sensor into shutdown mode to save power.
 * In shutdown mode, set one_shot=true to request a single conversion.
 * -------------------------------------------------------------------------*/
int tmp102_set_shutdown(Tmp102 *dev, bool shutdown)
{
    uint16_t config = 0;
    int err = tmp102_read_reg16(dev, TMP102_REG_CONFIG, &config);
    if (err != 0) return err;

    if (shutdown) {
        config |=  TMP102_CFG_SD;
    } else {
        config &= ~TMP102_CFG_SD;
    }
    return tmp102_write_reg16(dev, TMP102_REG_CONFIG, config);
}

/* =========================================================================
 * Stub HAL — simulates I2C bus with a fake TMP102 for PC unit testing
 * =========================================================================*/

/* Simulated TMP102 register state */
typedef struct {
    uint8_t  pointer;   /* current register pointer (set by last write) */
    uint16_t regs[4];   /* registers 0x00-0x03 */
} StubTmp102State;

static StubTmp102State stub_state;

static void stub_init_state(int16_t temp_raw)
{
    memset(&stub_state, 0, sizeof(stub_state));
    /* Temperature register: left-justify the 12-bit value */
    stub_state.regs[TMP102_REG_TEMP]   = (uint16_t)((uint16_t)temp_raw << 4);
    /* Default config: 0x60A0 (continuous, 12-bit, active-low alert, comparator) */
    stub_state.regs[TMP102_REG_CONFIG] = 0x60A0;
    stub_state.regs[TMP102_REG_TLOW]   = (uint16_t)(75 * 16) << 4;   /* 75 °C */
    stub_state.regs[TMP102_REG_THIGH]  = (uint16_t)(80 * 16) << 4;   /* 80 °C */
}

static int stub_write(I2cHal *hal, uint8_t dev_addr,
                       const uint8_t *buf, uint8_t len, uint32_t timeout_ms)
{
    (void)hal; (void)timeout_ms;
    if (dev_addr != 0x48) return -1;   /* simulate "no device" for other addresses */
    if (len < 1) return -1;

    stub_state.pointer = buf[0] & 0x03;   /* register pointer */
    if (len == 3) {
        /* Writing 16-bit register */
        stub_state.regs[stub_state.pointer] =
            ((uint16_t)buf[1] << 8) | buf[2];
    }
    return 0;
}

static int stub_read(I2cHal *hal, uint8_t dev_addr,
                      uint8_t *buf, uint8_t len, uint32_t timeout_ms)
{
    (void)hal; (void)timeout_ms;
    if (dev_addr != 0x48) return -1;
    if (len < 2) return -1;

    uint16_t val = stub_state.regs[stub_state.pointer];
    buf[0] = (uint8_t)(val >> 8);
    buf[1] = (uint8_t)(val & 0xFF);
    return 0;
}

static int stub_write_read(I2cHal *hal, uint8_t dev_addr,
                            const uint8_t *wbuf, uint8_t wlen,
                            uint8_t *rbuf,  uint8_t rlen,
                            uint32_t timeout_ms)
{
    /* Write phase: sets register pointer */
    int err = stub_write(hal, dev_addr, wbuf, wlen, timeout_ms);
    if (err != 0) return err;
    /* Read phase: reads from current pointer */
    return stub_read(hal, dev_addr, rbuf, rlen, timeout_ms);
}

static I2cHal make_stub_hal(void)
{
    I2cHal hal = {
        .read        = stub_read,
        .write       = stub_write,
        .write_read  = stub_write_read,
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

/*
 * Temperature raw-to-celsius conversion table for verification:
 *
 * Raw (12-bit)  |  Decimal  | Temperature (°C)
 * ------------------------------------------------
 *  0x7FF (2047) |  +127.9375| Maximum positive
 *  0x190 (400)  |  +25.0    | Room temperature
 *  0x000 (0)    |   0.0     | Freezing
 *  0xFF0 (-16)  |  -1.0     | Below zero (two's complement)
 *  0xC90 (-880) |  -55.0    | Minimum
 */
static void test_temperature_conversion(void)
{
    printf("=== Temperature raw-to-celsius conversion ===\n");

    I2cHal  hal = make_stub_hal();
    Tmp102  dev;

    /* --- 0 °C --- */
    stub_init_state(0);
    tmp102_init(&dev, &hal, 0x48);
    int32_t celsius_x16 = 0;
    tmp102_read_celsius_x16(&dev, &celsius_x16);
    check("0 °C -> raw=0 -> celsius_x16=0", celsius_x16 == 0);

    /* --- +25.0 °C (raw = 400) --- */
    stub_init_state(400);
    tmp102_init(&dev, &hal, 0x48);
    tmp102_read_celsius_x16(&dev, &celsius_x16);
    /* 400 * 0.0625 = 25.0 °C; in x16 units: 400 */
    check("+25.0 °C -> celsius_x16=400", celsius_x16 == 400);
    printf("         actual: %d / 16 = %d.%04d °C\n",
           (int)celsius_x16,
           (int)(celsius_x16 / 16),
           (int)((celsius_x16 % 16) * 625));  /* fractional part in 1/10000 */

    /* --- +127.9375 °C (raw = 2047 = 0x7FF) --- */
    stub_init_state(2047);
    tmp102_init(&dev, &hal, 0x48);
    tmp102_read_celsius_x16(&dev, &celsius_x16);
    check("+127.9375 °C -> celsius_x16=2047", celsius_x16 == 2047);

    /* --- -1.0 °C (raw = -16 in 12-bit two's complement) --- */
    /* In 12-bit 2's complement: -16 = 0xFF0 stored in upper 12 bits of 16-bit reg */
    /* As int16_t: 0xFF00 >> 4 = -16 */
    stub_init_state(-16);
    tmp102_init(&dev, &hal, 0x48);
    tmp102_read_celsius_x16(&dev, &celsius_x16);
    /* -16 * 0.0625 = -1.0 °C; in x16 units: -16 */
    check("-1.0 °C -> celsius_x16=-16", celsius_x16 == -16);
    printf("         actual: %d / 16 = %s%d.%04d °C\n",
           (int)celsius_x16,
           celsius_x16 < 0 ? "-" : "",
           (int)(celsius_x16 < 0 ? (-celsius_x16) / 16 : celsius_x16 / 16),
           (int)(celsius_x16 < 0 ? ((-celsius_x16) % 16) * 625
                                  : (celsius_x16 % 16) * 625));

    /* --- -55.0 °C (raw = -880 = 0xC90 in two's complement) --- */
    stub_init_state(-880);
    tmp102_init(&dev, &hal, 0x48);
    tmp102_read_celsius_x16(&dev, &celsius_x16);
    /* -880 * 0.0625 = -55.0 °C */
    check("-55.0 °C -> celsius_x16=-880", celsius_x16 == -880);
}

static void test_alert_thresholds(void)
{
    printf("\n=== Alert threshold register write ===\n");

    I2cHal  hal = make_stub_hal();
    Tmp102  dev;
    stub_init_state(400);   /* +25 °C */
    tmp102_init(&dev, &hal, 0x48);

    /* Set alert: warn below 0 °C, alert above 50 °C */
    int err = tmp102_set_alert_thresholds(&dev, 0, 50);
    check("set_alert_thresholds returns 0", err == 0);

    /* Verify registers were written correctly:
     * T_LOW  =  0 °C  -> raw 0    -> register 0x0000
     * T_HIGH = 50 °C  -> raw 800  -> left-justified: 800 << 4 = 12800 = 0x3200 */
    check("T_LOW  register = 0x0000",
          stub_state.regs[TMP102_REG_TLOW] == 0x0000);
    check("T_HIGH register = 0x3200",
          stub_state.regs[TMP102_REG_THIGH] == 0x3200);
}

static void test_shutdown(void)
{
    printf("\n=== Shutdown mode ===\n");

    I2cHal  hal = make_stub_hal();
    Tmp102  dev;
    stub_init_state(0);
    tmp102_init(&dev, &hal, 0x48);

    /* Enable shutdown */
    tmp102_set_shutdown(&dev, true);
    check("SD bit set in config",
          (stub_state.regs[TMP102_REG_CONFIG] & TMP102_CFG_SD) != 0);

    /* Disable shutdown */
    tmp102_set_shutdown(&dev, false);
    check("SD bit cleared in config",
          (stub_state.regs[TMP102_REG_CONFIG] & TMP102_CFG_SD) == 0);
}

static void test_device_not_found(void)
{
    printf("\n=== Device not found (wrong address) ===\n");

    I2cHal  hal = make_stub_hal();
    Tmp102  dev;
    stub_init_state(0);

    /* Attempt to init with wrong address */
    int err = tmp102_init(&dev, &hal, 0x55);
    check("init with wrong address returns error", err != 0);
}

int main(void)
{
    test_temperature_conversion();
    test_alert_thresholds();
    test_shutdown();
    test_device_not_found();

    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
