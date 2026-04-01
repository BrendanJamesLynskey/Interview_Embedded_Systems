/*
 * Challenge 02: UART Polling Driver
 *
 * Target: ARM Cortex-M4 (STM32F4 USART register layout, RM0090)
 *
 * Implements a polled (blocking) UART driver with:
 *   - Baud rate configuration from system clock and divider calculation
 *   - Single-byte TX (polled on TXE flag)
 *   - Single-byte RX (polled on RXNE flag, with timeout)
 *   - String transmit
 *   - Integer-to-string formatting (no printf dependency)
 *   - Error detection (framing error, noise flag, overrun)
 *   - A lightweight test harness with mocked registers for host compilation
 *
 * Key interview points demonstrated:
 *   - Why every register access uses volatile
 *   - Why baud rate divider is rounded (not truncated) for accuracy
 *   - Why a timeout is essential on any polled RX
 *   - How to detect and recover from USART overrun errors
 *
 * Compile (host):
 *   gcc -std=c11 -Wall -Wextra -DHOST_TEST -o uart_polling challenge_02_uart_polling.c
 *
 * Compile (target):
 *   arm-none-eabi-gcc -std=c11 -mcpu=cortex-m4 -mthumb -O2 \
 *       -o uart.elf challenge_02_uart_polling.c
 */

#include <stdint.h>
#include <stddef.h>

/* =========================================================================
 * Register definitions (STM32F4 USART)
 * =========================================================================*/

/* USART Status Register (SR) bit positions */
#define USART_SR_PE     (1U << 0)   /* parity error */
#define USART_SR_FE     (1U << 1)   /* framing error */
#define USART_SR_NF     (1U << 2)   /* noise flag */
#define USART_SR_ORE    (1U << 3)   /* overrun error */
#define USART_SR_IDLE   (1U << 4)   /* idle line detected */
#define USART_SR_RXNE   (1U << 5)   /* read data register not empty (byte received) */
#define USART_SR_TC     (1U << 6)   /* transmission complete */
#define USART_SR_TXE    (1U << 7)   /* transmit data register empty (ready for next byte) */

/* USART Control Register 1 (CR1) bit positions */
#define USART_CR1_SBK   (1U << 0)   /* send break */
#define USART_CR1_RE    (1U << 2)   /* receiver enable */
#define USART_CR1_TE    (1U << 3)   /* transmitter enable */
#define USART_CR1_RXNEIE (1U << 5)  /* RXNE interrupt enable */
#define USART_CR1_TCIE  (1U << 6)   /* TC interrupt enable */
#define USART_CR1_TXEIE (1U << 7)   /* TXE interrupt enable */
#define USART_CR1_UE    (1U << 13)  /* USART enable */
#define USART_CR1_M     (1U << 12)  /* word length: 0=8 bits, 1=9 bits */
#define USART_CR1_PCE   (1U << 10)  /* parity control enable */

/* USART Control Register 2 (CR2) bit positions */
#define USART_CR2_STOP_MASK  (0x3U << 12)
#define USART_CR2_STOP_1     (0x0U << 12)  /* 1 stop bit */
#define USART_CR2_STOP_2     (0x2U << 12)  /* 2 stop bits */

/* USART BRR register: mantissa in [15:4], fraction in [3:0] */
#define USART_BRR_MANTISSA_SHIFT  4U
#define USART_BRR_FRACTION_MASK   0x0FU

/* USART data mask (only bits [8:0] are data; bit 8 used in 9-bit mode) */
#define USART_DR_DATA_MASK  0x1FFU

/* =========================================================================
 * Platform abstraction
 * =========================================================================*/

typedef struct {
    volatile uint32_t SR;    /* 0x00 Status register */
    volatile uint32_t DR;    /* 0x04 Data register */
    volatile uint32_t BRR;   /* 0x08 Baud rate register */
    volatile uint32_t CR1;   /* 0x0C Control register 1 */
    volatile uint32_t CR2;   /* 0x10 Control register 2 */
    volatile uint32_t CR3;   /* 0x14 Control register 3 */
    volatile uint32_t GTPR;  /* 0x18 Guard time and prescaler */
} USART_TypeDef;

#ifdef HOST_TEST

#include <stdio.h>
#include <string.h>
#include <assert.h>

static USART_TypeDef mock_USART1;
#define USART1  (&mock_USART1)

/* Simulated TX output buffer for test verification */
static char  tx_capture[256];
static size_t tx_capture_len = 0;

/* Simulated RX input queue for feeding test bytes into the driver */
static uint8_t rx_inject[256];
static size_t  rx_inject_head = 0;
static size_t  rx_inject_tail = 0;

static void rx_inject_byte(uint8_t b)
{
    rx_inject[rx_inject_head % sizeof(rx_inject)] = b;
    rx_inject_head++;
}

#else

#define USART1  ((USART_TypeDef *)0x40011000U)

#endif /* HOST_TEST */

/* =========================================================================
 * Return codes
 * =========================================================================*/

typedef enum {
    UART_OK      =  0,  /* success */
    UART_ERR_TIMEOUT,   /* polled wait exceeded timeout count */
    UART_ERR_FRAME,     /* framing error detected in received byte */
    UART_ERR_OVERRUN,   /* overrun: RXNE set before previous byte was read */
    UART_ERR_NOISE,     /* noise flag set */
    UART_ERR_PARAM,     /* invalid parameter (e.g., zero baud rate) */
} uart_err_t;

/* =========================================================================
 * Driver state
 * =========================================================================*/

typedef struct {
    USART_TypeDef *usart;
    uint32_t       sysclk_hz;   /* peripheral clock feeding this USART */
    uint32_t       baud;
    uint32_t       rx_timeout;  /* number of busy-wait iterations before timeout */
} uart_handle_t;

/* =========================================================================
 * Baud rate divider calculation
 *
 * STM32F4 USART BRR formula (oversampling by 16, the default):
 *
 *   USARTDIV = f_CK / (16 * baud)
 *
 * USARTDIV is a fixed-point number: integer part in BRR[15:4], fractional
 * part (0/16 to 15/16) in BRR[3:0].
 *
 * To avoid floating-point, multiply both sides by 16:
 *
 *   BRR_scaled = (f_CK * 16) / (16 * baud)
 *              = f_CK / baud
 *
 * Then extract mantissa and fraction:
 *   mantissa  = BRR_scaled / 16   -> stored in BRR[15:4]
 *   fraction  = BRR_scaled % 16   -> stored in BRR[3:0]
 *
 * For better accuracy, round rather than truncate:
 *   BRR_scaled = (f_CK + baud/2) / baud  <-- +baud/2 before integer division
 *
 * Example: f_CK = 84 MHz, baud = 115200
 *   BRR_scaled = (84000000 + 57600) / 115200 = 84057600 / 115200 = 729.6 -> 729
 *   mantissa   = 729 / 16 = 45   (0x2D)
 *   fraction   = 729 % 16 =  9   (0x9)
 *   BRR        = (45 << 4) |  9  = 0x02D9
 *   Actual baud = 84000000 / (16 * (45 + 9/16))
 *              = 84000000 / (16 * 45.5625) = 84000000 / 729 ≈ 115207 baud
 *   Error: (115207 - 115200) / 115200 * 100 = 0.006% (well within UART spec of ~2%)
 * =========================================================================*/

static uint32_t uart_calc_brr(uint32_t sysclk_hz, uint32_t baud)
{
    /* Add (baud/2) before dividing to implement rounding */
    uint32_t div_scaled = (sysclk_hz + (baud / 2U)) / baud;
    uint32_t mantissa   = div_scaled / 16U;
    uint32_t fraction   = div_scaled % 16U;
    return (mantissa << USART_BRR_MANTISSA_SHIFT) | (fraction & USART_BRR_FRACTION_MASK);
}

/* =========================================================================
 * API implementation
 * =========================================================================*/

/*
 * uart_init -- initialise and enable the USART peripheral.
 *
 * Configures for 8N1 (8 data bits, no parity, 1 stop bit), oversampling x16.
 * The caller must have already enabled the peripheral clock (RCC register).
 */
uart_err_t uart_init(uart_handle_t *h, USART_TypeDef *usart,
                     uint32_t sysclk_hz, uint32_t baud, uint32_t rx_timeout)
{
    if (h == NULL || usart == NULL || baud == 0U || sysclk_hz == 0U) {
        return UART_ERR_PARAM;
    }

    h->usart      = usart;
    h->sysclk_hz  = sysclk_hz;
    h->baud       = baud;
    h->rx_timeout = rx_timeout;

    /* 1. Disable USART before configuration changes */
    usart->CR1 &= ~USART_CR1_UE;

    /* 2. Set baud rate */
    usart->BRR = uart_calc_brr(sysclk_hz, baud);

    /* 3. Set 8N1: clear M (8-bit word), clear PCE (no parity) */
    usart->CR1 &= ~(USART_CR1_M | USART_CR1_PCE);

    /* 4. Set 1 stop bit (CR2[13:12] = 00) */
    usart->CR2 = (usart->CR2 & ~USART_CR2_STOP_MASK) | USART_CR2_STOP_1;

    /* 5. Enable transmitter and receiver */
    usart->CR1 |= (USART_CR1_TE | USART_CR1_RE);

    /* 6. Enable USART -- must be last */
    usart->CR1 |= USART_CR1_UE;

    return UART_OK;
}

/*
 * uart_tx_byte -- transmit one byte (polled).
 *
 * Waits for TXE (transmit data register empty) before writing DR.
 * This function does NOT wait for TC (transmission complete).  If the
 * application needs to ensure all bits are on the wire before shutdown,
 * poll TC separately.
 *
 * On a target with 8 MHz clock and 9600 baud, each byte takes ~1.04 ms.
 * The CPU spins for this duration.  Use DMA or interrupt-driven TX for
 * any non-trivial throughput requirement.
 */
uart_err_t uart_tx_byte(const uart_handle_t *h, uint8_t byte)
{
    if (h == NULL || h->usart == NULL) {
        return UART_ERR_PARAM;
    }

    /* Wait for transmit data register empty */
    while (!(h->usart->SR & USART_SR_TXE)) {
        /* busy wait */
    }

    /*
     * Write the byte.  Writing DR clears TXE.
     * Only bits [7:0] are used in 8-bit mode.
     */
    h->usart->DR = (uint32_t)byte & 0xFFU;

#ifdef HOST_TEST
    /* Capture transmitted bytes for test verification */
    if (tx_capture_len < sizeof(tx_capture) - 1U) {
        tx_capture[tx_capture_len++] = (char)byte;
        tx_capture[tx_capture_len]   = '\0';
    }
#endif

    return UART_OK;
}

/*
 * uart_rx_byte -- receive one byte (polled, with timeout).
 *
 * Returns UART_OK and writes received byte to *out on success.
 * Returns UART_ERR_TIMEOUT if no byte arrives within h->rx_timeout iterations.
 * Returns UART_ERR_FRAME, UART_ERR_OVERRUN, or UART_ERR_NOISE on error.
 *
 * Error handling: on any error, the DR register is read to clear the error
 * flags (on STM32F4, error flags are cleared by reading SR then DR in sequence).
 */
uart_err_t uart_rx_byte(const uart_handle_t *h, uint8_t *out)
{
    if (h == NULL || h->usart == NULL || out == NULL) {
        return UART_ERR_PARAM;
    }

    uint32_t timeout = h->rx_timeout;

    while (!(h->usart->SR & USART_SR_RXNE)) {
#ifdef HOST_TEST
        /* Inject a test byte if one is queued */
        if (rx_inject_tail < rx_inject_head) {
            uint8_t b = rx_inject[rx_inject_tail % sizeof(rx_inject)];
            rx_inject_tail++;
            mock_USART1.DR = b;
            mock_USART1.SR |= USART_SR_RXNE;
        }
#endif
        if (timeout == 0U) {
            return UART_ERR_TIMEOUT;
        }
        if (timeout != UINT32_MAX) {
            timeout--;
        }
    }

    /*
     * Check for errors BEFORE reading DR.
     * On STM32F4, the sequence to clear error flags is:
     *   1. Read SR (captures current error bits).
     *   2. Read DR (clears the error bits and RXNE).
     * Reading DR without first reading SR loses the error information.
     */
    uint32_t sr = h->usart->SR;

    /* Read DR regardless (clears RXNE and error flags) */
    uint8_t received = (uint8_t)(h->usart->DR & 0xFFU);

#ifdef HOST_TEST
    h->usart->SR &= ~USART_SR_RXNE;   /* mock: clear flag after read */
#endif

    *out = received;

    if (sr & USART_SR_FE)  { return UART_ERR_FRAME;   }
    if (sr & USART_SR_ORE) { return UART_ERR_OVERRUN;  }
    if (sr & USART_SR_NF)  { return UART_ERR_NOISE;    }

    return UART_OK;
}

/*
 * uart_tx_string -- transmit a NUL-terminated string.
 *
 * Returns UART_OK if all bytes were sent, or the first error encountered.
 */
uart_err_t uart_tx_string(const uart_handle_t *h, const char *str)
{
    if (h == NULL || str == NULL) {
        return UART_ERR_PARAM;
    }

    while (*str != '\0') {
        uart_err_t err = uart_tx_byte(h, (uint8_t)*str);
        if (err != UART_OK) {
            return err;
        }
        str++;
    }
    return UART_OK;
}

/*
 * uart_tx_uint32 -- transmit a uint32_t as a decimal ASCII string.
 *
 * No printf dependency.  The output is a right-justified decimal string
 * with no leading zeros (except for the value 0, which outputs "0").
 *
 * Maximum output: 10 digits for UINT32_MAX (4294967295) + NUL = 11 bytes.
 */
uart_err_t uart_tx_uint32(const uart_handle_t *h, uint32_t value)
{
    char     buf[11];   /* max 10 decimal digits + NUL */
    int8_t   pos = (int8_t)(sizeof(buf) - 1);
    uart_err_t err;

    buf[pos] = '\0';
    pos--;

    if (value == 0U) {
        buf[pos] = '0';
    } else {
        while (value > 0U && pos >= 0) {
            buf[pos] = (char)('0' + (value % 10U));
            value   /= 10U;
            pos--;
        }
        pos++;   /* point to first digit */
    }

    err = uart_tx_string(h, &buf[pos]);
    return err;
}

/*
 * uart_tx_hex32 -- transmit a uint32_t as an 8-digit hexadecimal ASCII string.
 *
 * Always outputs exactly 8 hex digits (zero-padded).
 * Useful for printing register values and memory addresses.
 */
uart_err_t uart_tx_hex32(const uart_handle_t *h, uint32_t value)
{
    static const char hex_chars[] = "0123456789ABCDEF";
    char      buf[9];  /* 8 hex digits + NUL */
    uart_err_t err;

    for (int8_t i = 7; i >= 0; i--) {
        buf[i] = hex_chars[value & 0xFU];
        value >>= 4U;
    }
    buf[8] = '\0';

    err = uart_tx_string(h, buf);
    return err;
}

/*
 * uart_wait_tc -- wait for transmission complete (TC flag).
 *
 * Use this after the last uart_tx_byte() call if you need to ensure all
 * bits (including stop bit) have left the shift register before:
 *   - Entering a low-power stop mode
 *   - Disabling the USART
 *   - Turning off the peripheral clock
 *   - Driving a direction-control pin low for RS-485
 */
uart_err_t uart_wait_tc(const uart_handle_t *h)
{
    if (h == NULL || h->usart == NULL) {
        return UART_ERR_PARAM;
    }
    while (!(h->usart->SR & USART_SR_TC)) {
        /* busy wait */
    }
    return UART_OK;
}

/* =========================================================================
 * Test harness (HOST_TEST only)
 * =========================================================================*/
#ifdef HOST_TEST

static int tests_run    = 0;
static int tests_passed = 0;

#define TEST(label, cond) do {                                  \
    tests_run++;                                                 \
    if (cond) {                                                  \
        tests_passed++;                                          \
        printf("  PASS  %s\n", (label));                        \
    } else {                                                     \
        printf("  FAIL  %s  (line %d)\n", (label), __LINE__);  \
    }                                                            \
} while (0)

#define TEST_STREQ(label, got, expected) do {             \
    tests_run++;                                           \
    if (strcmp((got), (expected)) == 0) {                 \
        tests_passed++;                                    \
        printf("  PASS  %s\n", (label));                  \
    } else {                                               \
        printf("  FAIL  %s: got \"%s\", expected \"%s\"\n", \
               (label), (got), (expected));               \
    }                                                      \
} while (0)

int main(void)
{
    uart_handle_t h;
    uart_err_t err;

    memset(&mock_USART1, 0, sizeof(mock_USART1));

    /* -------------------------------------------------------------------------
     * BRR calculation tests (no hardware needed)
     * -------------------------------------------------------------------------*/
    printf("=== BRR calculation ===\n");

    /* f_CK = 84 MHz, baud = 115200 -> BRR should be 0x02D9 */
    uint32_t brr = uart_calc_brr(84000000U, 115200U);
    TEST("84MHz/115200 BRR = 0x02D9", brr == 0x02D9U);

    /* f_CK = 16 MHz, baud = 9600 -> USARTDIV = 104.1667, scaled = 1666.7 -> 1667
       mantissa = 1667/16 = 104, fraction = 1667%16 = 3
       BRR = (104<<4)|3 = 0x0683 */
    brr = uart_calc_brr(16000000U, 9600U);
    TEST("16MHz/9600 BRR = 0x0683", brr == 0x0683U);

    /* f_CK = 72 MHz, baud = 115200 -> USARTDIV = 39.0625
       scaled = 72000000/115200 = 625 (with rounding: (72000000+57600)/115200 = 625)
       mantissa = 625/16 = 39, fraction = 625%16 = 1
       BRR = (39<<4)|1 = 0x0271 */
    brr = uart_calc_brr(72000000U, 115200U);
    TEST("72MHz/115200 BRR = 0x0271", brr == 0x0271U);

    /* -------------------------------------------------------------------------
     * uart_init tests
     * -------------------------------------------------------------------------*/
    printf("\n=== uart_init ===\n");

    /* Mock: pre-set TXE so the driver doesn't spin */
    mock_USART1.SR = USART_SR_TXE | USART_SR_TC;

    err = uart_init(&h, USART1, 84000000U, 115200U, 100000U);
    TEST("uart_init returns OK", err == UART_OK);
    TEST("USART UE bit set",     (USART1->CR1 & USART_CR1_UE)  != 0U);
    TEST("USART TE bit set",     (USART1->CR1 & USART_CR1_TE)  != 0U);
    TEST("USART RE bit set",     (USART1->CR1 & USART_CR1_RE)  != 0U);
    TEST("USART M bit clear",    (USART1->CR1 & USART_CR1_M)   == 0U);
    TEST("USART BRR correct",    USART1->BRR == 0x02D9U);

    err = uart_init(NULL, USART1, 84000000U, 115200U, 100000U);
    TEST("uart_init NULL handle returns error", err == UART_ERR_PARAM);

    err = uart_init(&h, USART1, 84000000U, 0U, 100000U);
    TEST("uart_init zero baud returns error", err == UART_ERR_PARAM);

    /* -------------------------------------------------------------------------
     * uart_tx_byte / uart_tx_string tests
     * -------------------------------------------------------------------------*/
    printf("\n=== uart_tx_byte / uart_tx_string ===\n");

    /* Reset capture buffer */
    memset(tx_capture, 0, sizeof(tx_capture));
    tx_capture_len = 0;
    mock_USART1.SR = USART_SR_TXE | USART_SR_TC;

    err = uart_tx_byte(&h, 'H');
    TEST("tx_byte 'H' returns OK", err == UART_OK);
    TEST("tx_byte 'H' captured", tx_capture[0] == 'H');

    memset(tx_capture, 0, sizeof(tx_capture));
    tx_capture_len = 0;
    mock_USART1.SR = USART_SR_TXE | USART_SR_TC;

    err = uart_tx_string(&h, "Hello\r\n");
    TEST("tx_string returns OK", err == UART_OK);
    TEST_STREQ("tx_string output", tx_capture, "Hello\r\n");

    /* -------------------------------------------------------------------------
     * uart_tx_uint32 / uart_tx_hex32 tests
     * -------------------------------------------------------------------------*/
    printf("\n=== uart_tx_uint32 / uart_tx_hex32 ===\n");

    struct { uint32_t val; const char *expected; } dec_tests[] = {
        {0U,          "0"},
        {1U,          "1"},
        {42U,         "42"},
        {4294967295U, "4294967295"},   /* UINT32_MAX */
    };
    for (size_t i = 0; i < sizeof(dec_tests)/sizeof(dec_tests[0]); i++) {
        memset(tx_capture, 0, sizeof(tx_capture));
        tx_capture_len = 0;
        mock_USART1.SR = USART_SR_TXE | USART_SR_TC;

        char label[64];
        snprintf(label, sizeof(label), "uart_tx_uint32(%u)", dec_tests[i].val);
        uart_tx_uint32(&h, dec_tests[i].val);
        TEST_STREQ(label, tx_capture, dec_tests[i].expected);
    }

    struct { uint32_t val; const char *expected; } hex_tests[] = {
        {0x00000000U, "00000000"},
        {0xDEADBEEFU, "DEADBEEF"},
        {0x0000FFFFU, "0000FFFF"},
        {0xFFFFFFFFU, "FFFFFFFF"},
    };
    for (size_t i = 0; i < sizeof(hex_tests)/sizeof(hex_tests[0]); i++) {
        memset(tx_capture, 0, sizeof(tx_capture));
        tx_capture_len = 0;
        mock_USART1.SR = USART_SR_TXE | USART_SR_TC;

        char label[64];
        snprintf(label, sizeof(label), "uart_tx_hex32(0x%08X)", hex_tests[i].val);
        uart_tx_hex32(&h, hex_tests[i].val);
        TEST_STREQ(label, tx_capture, hex_tests[i].expected);
    }

    /* -------------------------------------------------------------------------
     * uart_rx_byte tests
     * -------------------------------------------------------------------------*/
    printf("\n=== uart_rx_byte ===\n");

    uint8_t received;

    /* Normal receive */
    rx_inject_head = 0;
    rx_inject_tail = 0;
    rx_inject_byte(0x55U);
    mock_USART1.SR = 0U;   /* RXNE cleared, will be set by inject logic */

    err = uart_rx_byte(&h, &received);
    TEST("rx_byte returns OK", err == UART_OK);
    TEST("rx_byte correct value (0x55)", received == 0x55U);

    /* Receive with framing error */
    rx_inject_head = 0;
    rx_inject_tail = 0;
    rx_inject_byte(0xAAU);
    mock_USART1.SR = 0U;
    mock_USART1.SR |= USART_SR_FE;   /* pre-set framing error */
    mock_USART1.SR |= USART_SR_RXNE;

    err = uart_rx_byte(&h, &received);
    TEST("rx_byte framing error detected", err == UART_ERR_FRAME);

    /* Receive with overrun error */
    mock_USART1.SR = USART_SR_ORE | USART_SR_RXNE;
    mock_USART1.DR = 0xBBU;

    err = uart_rx_byte(&h, &received);
    TEST("rx_byte overrun detected", err == UART_ERR_OVERRUN);

    /* Timeout: no byte available, timeout should expire */
    uart_handle_t h_short;
    uart_init(&h_short, USART1, 84000000U, 115200U, 5U);  /* very short timeout */
    mock_USART1.SR = 0U;   /* RXNE never set */
    rx_inject_head = 0;
    rx_inject_tail = 0;

    err = uart_rx_byte(&h_short, &received);
    TEST("rx_byte timeout returns UART_ERR_TIMEOUT", err == UART_ERR_TIMEOUT);

    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}

#endif /* HOST_TEST */
