/*
 * Challenge 01: GPIO Driver with Register-Level Access
 *
 * Target: ARM Cortex-M4 (STM32F4 register layout)
 *
 * Implements a portable GPIO abstraction over raw memory-mapped registers.
 * The driver covers:
 *   - Pin mode configuration (input, output, alternate function, analog)
 *   - Output type (push-pull vs open-drain)
 *   - Pull-up/pull-down configuration
 *   - Atomic pin set/clear/toggle using BSRR
 *   - Digital input reading
 *   - A lightweight test harness that mocks the hardware registers in host SRAM
 *     so the logic can be verified on a host machine (gcc -std=c11)
 *
 * Register base addresses match STM32F4xx Reference Manual (RM0090).
 * All register offsets are standard AMBA APB2/AHB1 assignments.
 *
 * Compile (host):
 *   gcc -std=c11 -Wall -Wextra -DHOST_TEST -o gpio_driver challenge_01_gpio_driver.c
 *
 * Compile (target, arm-none-eabi):
 *   arm-none-eabi-gcc -std=c11 -mcpu=cortex-m4 -mthumb -O2 \
 *       -o gpio_driver.elf challenge_01_gpio_driver.c
 *   (add -nostdlib -T linker_script.ld for a real bare-metal build)
 */

#include <stdint.h>
#include <stddef.h>

/* =========================================================================
 * Platform abstraction
 *
 * In a real target build (no HOST_TEST), these map to the physical MMIO
 * addresses.  In HOST_TEST mode, they map to local SRAM mock registers so
 * the logic can be compiled and run on a desktop.
 * =========================================================================*/

#ifdef HOST_TEST

#include <stdio.h>
#include <string.h>
#include <assert.h>

/* Each GPIO port has 10 x 32-bit registers (0x28 bytes total per RM0090) */
typedef struct {
    volatile uint32_t MODER;     /* 0x00 Mode register */
    volatile uint32_t OTYPER;    /* 0x04 Output type register */
    volatile uint32_t OSPEEDR;   /* 0x08 Output speed register */
    volatile uint32_t PUPDR;     /* 0x0C Pull-up/pull-down register */
    volatile uint32_t IDR;       /* 0x10 Input data register (read-only) */
    volatile uint32_t ODR;       /* 0x14 Output data register */
    volatile uint32_t BSRR;      /* 0x18 Bit set/reset register (write-only) */
    volatile uint32_t LCKR;      /* 0x1C Configuration lock register */
    volatile uint32_t AFR[2];    /* 0x20, 0x24 Alternate function registers */
} GPIO_TypeDef;

/* Mock register banks (located in SRAM during host test) */
static GPIO_TypeDef mock_GPIOA;
static GPIO_TypeDef mock_GPIOB;
static GPIO_TypeDef mock_GPIOC;

#define GPIOA  (&mock_GPIOA)
#define GPIOB  (&mock_GPIOB)
#define GPIOC  (&mock_GPIOC)

#else /* --- real target --- */

typedef struct {
    volatile uint32_t MODER;
    volatile uint32_t OTYPER;
    volatile uint32_t OSPEEDR;
    volatile uint32_t PUPDR;
    volatile uint32_t IDR;
    volatile uint32_t ODR;
    volatile uint32_t BSRR;
    volatile uint32_t LCKR;
    volatile uint32_t AFR[2];
} GPIO_TypeDef;

#define GPIOA  ((GPIO_TypeDef *)0x40020000U)
#define GPIOB  ((GPIO_TypeDef *)0x40020400U)
#define GPIOC  ((GPIO_TypeDef *)0x40020800U)
/* ... additional ports as needed ... */

#endif /* HOST_TEST */

/* =========================================================================
 * Public API types and constants
 * =========================================================================*/

/* GPIO pin mode (MODER register, 2 bits per pin) */
typedef enum {
    GPIO_MODE_INPUT   = 0x00U,  /* digital input (reset state for most pins) */
    GPIO_MODE_OUTPUT  = 0x01U,  /* general-purpose output */
    GPIO_MODE_AF      = 0x02U,  /* alternate function (UART, SPI, I2C, timer ...) */
    GPIO_MODE_ANALOG  = 0x03U,  /* analog (ADC, DAC, comparator) */
} gpio_mode_t;

/* Output type (OTYPER register, 1 bit per pin) */
typedef enum {
    GPIO_OTYPE_PUSHPULL   = 0U, /* driven high and low (default) */
    GPIO_OTYPE_OPENDRAIN  = 1U, /* driven low only; pull-up needed for high */
} gpio_otype_t;

/* Output speed (OSPEEDR register, 2 bits per pin) */
typedef enum {
    GPIO_SPEED_LOW      = 0x00U,  /* 2 MHz max slew */
    GPIO_SPEED_MEDIUM   = 0x01U,  /* 25 MHz max */
    GPIO_SPEED_HIGH     = 0x02U,  /* 50 MHz max */
    GPIO_SPEED_VERYHIGH = 0x03U,  /* 100 MHz max (use for high-speed SDIO/etc.) */
} gpio_speed_t;

/* Pull-up / pull-down (PUPDR register, 2 bits per pin) */
typedef enum {
    GPIO_PUPD_NONE     = 0x00U,  /* floating */
    GPIO_PUPD_PULLUP   = 0x01U,  /* internal pull-up (~40 kOhm) */
    GPIO_PUPD_PULLDOWN = 0x02U,  /* internal pull-down (~40 kOhm) */
} gpio_pupd_t;

/* Alternate function selection (0-15, stored in AFR registers) */
typedef uint8_t gpio_af_t;      /* GPIO_AF0 .. GPIO_AF15 */

/* Pin configuration bundle */
typedef struct {
    gpio_mode_t  mode;
    gpio_otype_t otype;
    gpio_speed_t speed;
    gpio_pupd_t  pupd;
    gpio_af_t    af;     /* only used when mode == GPIO_MODE_AF */
} gpio_pin_cfg_t;

/* =========================================================================
 * Internal helpers
 * =========================================================================*/

/*
 * gpio_set_field_2bit -- write a 2-bit value to a field within a 32-bit register.
 *
 * @reg:   pointer to the register
 * @pin:   pin number (0-15); field occupies bits [2*pin + 1 : 2*pin]
 * @val:   2-bit value to write (0-3)
 */
static void gpio_set_field_2bit(volatile uint32_t *reg, uint8_t pin, uint32_t val)
{
    uint32_t shift = (uint32_t)pin * 2U;
    *reg = (*reg & ~(0x3U << shift)) | ((val & 0x3U) << shift);
}

/*
 * gpio_set_field_1bit -- write a 1-bit value at bit position `pin`.
 */
static void gpio_set_field_1bit(volatile uint32_t *reg, uint8_t pin, uint32_t val)
{
    uint32_t shift = (uint32_t)pin;
    *reg = (*reg & ~(0x1U << shift)) | ((val & 0x1U) << shift);
}

/* =========================================================================
 * Public API implementation
 * =========================================================================*/

/*
 * gpio_configure -- configure a single GPIO pin.
 *
 * @port: pointer to GPIO peripheral struct (GPIOA, GPIOB, etc.)
 * @pin:  pin number (0-15)
 * @cfg:  pointer to configuration struct
 *
 * This function performs a read-modify-write on each configuration register.
 * If called from an ISR, disable interrupts around the call site to avoid
 * a race with another caller modifying a different pin on the same port.
 */
void gpio_configure(GPIO_TypeDef *port, uint8_t pin, const gpio_pin_cfg_t *cfg)
{
    if (port == NULL || cfg == NULL || pin > 15U) {
        return;   /* defensive: invalid arguments */
    }

    /* 1. Mode register: 2 bits per pin */
    gpio_set_field_2bit(&port->MODER,   pin, (uint32_t)cfg->mode);

    /* 2. Output type: 1 bit per pin */
    gpio_set_field_1bit(&port->OTYPER,  pin, (uint32_t)cfg->otype);

    /* 3. Output speed: 2 bits per pin */
    gpio_set_field_2bit(&port->OSPEEDR, pin, (uint32_t)cfg->speed);

    /* 4. Pull-up / pull-down: 2 bits per pin */
    gpio_set_field_2bit(&port->PUPDR,   pin, (uint32_t)cfg->pupd);

    /* 5. Alternate function: 4 bits per pin, split across AFR[0] (pins 0-7)
          and AFR[1] (pins 8-15) */
    if (cfg->mode == GPIO_MODE_AF) {
        uint8_t  afr_idx = pin >> 3;             /* 0 for pins 0-7, 1 for 8-15 */
        uint8_t  afr_pin = pin & 0x7U;           /* bit position within AFR word */
        uint32_t shift   = (uint32_t)afr_pin * 4U;
        port->AFR[afr_idx] = (port->AFR[afr_idx] & ~(0xFU << shift))
                           | ((cfg->af & 0xFU)  <<  shift);
    }
}

/*
 * gpio_set -- atomically drive a pin HIGH.
 *
 * Uses BSRR bits [15:0] (set bits).  Single 32-bit write -- no read needed.
 * ISR-safe: no read-modify-write of ODR.
 */
void gpio_set(GPIO_TypeDef *port, uint8_t pin)
{
    port->BSRR = (1U << (pin & 0xFU));
}

/*
 * gpio_clear -- atomically drive a pin LOW.
 *
 * Uses BSRR bits [31:16] (reset bits).  Single 32-bit write -- ISR-safe.
 */
void gpio_clear(GPIO_TypeDef *port, uint8_t pin)
{
    port->BSRR = (1U << ((pin & 0xFU) + 16U));
}

/*
 * gpio_toggle -- toggle a pin's output state.
 *
 * NOTE: This is NOT atomic.  It reads ODR, flips one bit, and writes back.
 * Do not use this from both main code and an ISR for the same port without
 * interrupt masking.  Prefer gpio_set / gpio_clear from ISR context.
 */
void gpio_toggle(GPIO_TypeDef *port, uint8_t pin)
{
    uint32_t odr = port->ODR;
    uint32_t mask = (1U << (pin & 0xFU));

    if (odr & mask) {
        /* Currently high -- clear using BSRR upper half */
        port->BSRR = (mask << 16U);
    } else {
        /* Currently low -- set using BSRR lower half */
        port->BSRR = mask;
    }
}

/*
 * gpio_read -- read the current logic level of a pin.
 *
 * Reads IDR for input pins.  For output pins, IDR reflects the driven value
 * on most GPIO peripherals (check device datasheet for exceptions).
 *
 * Returns: 0 if pin is low, 1 if pin is high.
 */
uint8_t gpio_read(const GPIO_TypeDef *port, uint8_t pin)
{
    return (uint8_t)((port->IDR >> (pin & 0xFU)) & 0x1U);
}

/*
 * gpio_write -- drive a pin to a specified level (0 = low, non-zero = high).
 * ISR-safe (uses BSRR).
 */
void gpio_write(GPIO_TypeDef *port, uint8_t pin, uint8_t level)
{
    if (level != 0U) {
        gpio_set(port, pin);
    } else {
        gpio_clear(port, pin);
    }
}

/* =========================================================================
 * Test harness (HOST_TEST mode only)
 * =========================================================================*/
#ifdef HOST_TEST

static int tests_run    = 0;
static int tests_passed = 0;

#define TEST(label, cond) do {                                        \
    tests_run++;                                                       \
    if (cond) {                                                        \
        tests_passed++;                                                \
        printf("  PASS  %s\n", (label));                              \
    } else {                                                           \
        printf("  FAIL  %s\n", (label));                              \
    }                                                                  \
} while (0)

/*
 * Apply BSRR side-effects to ODR.
 *
 * On real hardware, writing BSRR automatically updates ODR.
 * In the mock, BSRR is a plain register -- we simulate the hardware logic.
 *
 * BSRR bits [15:0]  -> set corresponding ODR bits
 * BSRR bits [31:16] -> clear corresponding ODR bits
 * Set takes priority over clear if the same bit appears in both halves.
 */
static void apply_bsrr(GPIO_TypeDef *port)
{
    uint32_t set_mask   = port->BSRR & 0x0000FFFFU;
    uint32_t clear_mask = (port->BSRR >> 16U) & 0x0000FFFFU;

    /* Set overrides clear for the same bit */
    port->ODR  = (port->ODR & ~clear_mask) | set_mask;
    port->IDR  = port->ODR;   /* mock: IDR reflects ODR for outputs */
    port->BSRR = 0U;          /* BSRR is self-clearing on hardware */
}

int main(void)
{
    /* Reset all mock registers */
    memset(&mock_GPIOA, 0, sizeof(mock_GPIOA));
    memset(&mock_GPIOB, 0, sizeof(mock_GPIOB));
    memset(&mock_GPIOC, 0, sizeof(mock_GPIOC));

    printf("=== gpio_configure ===\n");
    {
        /* Configure PA5 as push-pull output, medium speed, no pull */
        gpio_pin_cfg_t cfg = {
            .mode  = GPIO_MODE_OUTPUT,
            .otype = GPIO_OTYPE_PUSHPULL,
            .speed = GPIO_SPEED_MEDIUM,
            .pupd  = GPIO_PUPD_NONE,
            .af    = 0U,
        };
        gpio_configure(GPIOA, 5, &cfg);

        /* MODER: pin 5 field = bits [11:10], expected 0b01 = 1 */
        TEST("PA5 MODER = output (0b01)",
             ((GPIOA->MODER >> 10U) & 0x3U) == (uint32_t)GPIO_MODE_OUTPUT);

        /* OTYPER: bit 5 = 0 (push-pull) */
        TEST("PA5 OTYPER = push-pull (0)",
             ((GPIOA->OTYPER >> 5U) & 0x1U) == 0U);

        /* OSPEEDR: pin 5 field = bits [11:10], expected 0b01 (MEDIUM) */
        TEST("PA5 OSPEEDR = medium (0b01)",
             ((GPIOA->OSPEEDR >> 10U) & 0x3U) == (uint32_t)GPIO_SPEED_MEDIUM);

        /* PUPDR: pin 5 field = bits [11:10], expected 0b00 (no pull) */
        TEST("PA5 PUPDR = none (0b00)",
             ((GPIOA->PUPDR >> 10U) & 0x3U) == (uint32_t)GPIO_PUPD_NONE);

        /* Configure PA8 as alternate function 7 (USART1_TX on STM32F4) */
        gpio_pin_cfg_t af_cfg = {
            .mode  = GPIO_MODE_AF,
            .otype = GPIO_OTYPE_PUSHPULL,
            .speed = GPIO_SPEED_HIGH,
            .pupd  = GPIO_PUPD_NONE,
            .af    = 7U,  /* AF7 = USART1/2/3 */
        };
        gpio_configure(GPIOA, 8, &af_cfg);

        /* MODER: pin 8 field = bits [17:16], expected 0b10 (AF) */
        TEST("PA8 MODER = AF (0b10)",
             ((GPIOA->MODER >> 16U) & 0x3U) == (uint32_t)GPIO_MODE_AF);

        /* AFR[1]: pin 8 is in AFR[1] (pins 8-15), at bits [3:0] of AFR[1] */
        TEST("PA8 AFR[1][3:0] = 7",
             (GPIOA->AFR[1] & 0xFU) == 7U);
    }

    printf("\n=== gpio_set / gpio_clear ===\n");
    {
        /* Start with all ODR bits cleared */
        GPIOA->ODR  = 0U;
        GPIOA->BSRR = 0U;

        gpio_set(GPIOA, 5);
        apply_bsrr(GPIOA);   /* simulate hardware BSRR -> ODR effect */
        TEST("PA5 set: ODR bit 5 = 1", (GPIOA->ODR >> 5U) & 0x1U);

        gpio_clear(GPIOA, 5);
        apply_bsrr(GPIOA);
        TEST("PA5 clear: ODR bit 5 = 0", !((GPIOA->ODR >> 5U) & 0x1U));

        /* Set multiple pins and verify no side-effects on others */
        GPIOA->ODR = 0U;
        gpio_set(GPIOA, 0);
        apply_bsrr(GPIOA);
        gpio_set(GPIOA, 15);
        apply_bsrr(GPIOA);
        TEST("PA0 and PA15 set independently",
             (GPIOA->ODR & 0x8001U) == 0x8001U);
        TEST("No other pins affected",
             (GPIOA->ODR & ~0x8001U) == 0U);

        /* Clear one pin without affecting the other */
        gpio_clear(GPIOA, 0);
        apply_bsrr(GPIOA);
        TEST("PA0 cleared, PA15 unchanged",
             (GPIOA->ODR & 0x8001U) == 0x8000U);
    }

    printf("\n=== gpio_toggle ===\n");
    {
        GPIOA->ODR = 0U;
        GPIOA->IDR = 0U;

        gpio_toggle(GPIOA, 3);
        apply_bsrr(GPIOA);
        TEST("Toggle PA3 low->high: ODR bit 3 = 1",
             (GPIOA->ODR >> 3U) & 0x1U);

        gpio_toggle(GPIOA, 3);
        apply_bsrr(GPIOA);
        TEST("Toggle PA3 high->low: ODR bit 3 = 0",
             !((GPIOA->ODR >> 3U) & 0x1U));
    }

    printf("\n=== gpio_read ===\n");
    {
        /* Simulate external signals on IDR */
        GPIOC->IDR = 0x00A5U;   /* pins 0, 2, 5, 7 are high */

        TEST("PC0 = 1 (bit 0 of 0xA5)",  gpio_read(GPIOC, 0)  == 1U);
        TEST("PC1 = 0 (bit 1 of 0xA5)",  gpio_read(GPIOC, 1)  == 0U);
        TEST("PC2 = 1 (bit 2 of 0xA5)",  gpio_read(GPIOC, 2)  == 1U);
        TEST("PC3 = 0 (bit 3 of 0xA5)",  gpio_read(GPIOC, 3)  == 0U);
        TEST("PC5 = 1 (bit 5 of 0xA5)",  gpio_read(GPIOC, 5)  == 1U);
        TEST("PC7 = 1 (bit 7 of 0xA5)",  gpio_read(GPIOC, 7)  == 1U);
        TEST("PC8 = 0 (upper byte zero)", gpio_read(GPIOC, 8)  == 0U);
        TEST("PC15 = 0",                  gpio_read(GPIOC, 15) == 0U);
    }

    printf("\n=== gpio_write ===\n");
    {
        GPIOB->ODR = 0U;

        gpio_write(GPIOB, 7, 1);
        apply_bsrr(GPIOB);
        TEST("gpio_write PB7 = 1", (GPIOB->ODR >> 7U) & 0x1U);

        gpio_write(GPIOB, 7, 0);
        apply_bsrr(GPIOB);
        TEST("gpio_write PB7 = 0", !((GPIOB->ODR >> 7U) & 0x1U));

        gpio_write(GPIOB, 7, 0xFF);  /* non-zero value should set pin */
        apply_bsrr(GPIOB);
        TEST("gpio_write PB7 = 0xFF (non-zero -> high)", (GPIOB->ODR >> 7U) & 0x1U);
    }

    printf("\n=== edge cases ===\n");
    {
        /* NULL port -- must not crash */
        gpio_pin_cfg_t cfg = {GPIO_MODE_OUTPUT, GPIO_OTYPE_PUSHPULL,
                              GPIO_SPEED_LOW, GPIO_PUPD_NONE, 0};
        gpio_configure(NULL, 0, &cfg);
        tests_run++;
        tests_passed++;
        printf("  PASS  NULL port in gpio_configure (no crash)\n");

        /* Pin number out of range -- must not crash */
        gpio_configure(GPIOA, 16, &cfg);  /* pin 16 is invalid (0-15 valid) */
        tests_run++;
        tests_passed++;
        printf("  PASS  Out-of-range pin in gpio_configure (no crash)\n");

        /* BSRR simultaneous set and clear -- set takes priority (per STM32 spec) */
        GPIOA->ODR  = 0U;
        GPIOA->BSRR = 0U;
        /* Manually write BSRR to set AND clear bit 3 in same write */
        GPIOA->BSRR = (1U << 3U) | (1U << (3U + 16U));  /* set and clear bit 3 */
        apply_bsrr(GPIOA);
        /* Per STM32 RM: BSRR set (lower 16) overrides reset (upper 16) */
        TEST("BSRR simultaneous set+clear: set wins (ODR bit 3 = 1)",
             (GPIOA->ODR >> 3U) & 0x1U);
    }

    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}

#endif /* HOST_TEST */
