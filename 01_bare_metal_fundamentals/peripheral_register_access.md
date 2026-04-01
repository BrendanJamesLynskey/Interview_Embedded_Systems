# Peripheral Register Access

## Prerequisites
- ARM Cortex-M memory map and memory-mapped I/O concepts
- C pointers, pointer casting, and type punning
- `volatile` keyword (see `volatile_and_memory_barriers.md`)
- Basic peripheral knowledge: GPIO, UART, timer, DMA

---

## Concept Reference

### Memory-Mapped Peripheral Access

On Cortex-M (and all ARM-based) devices, peripheral registers live in the processor's address space. Reading from or writing to a peripheral register is identical to a normal memory read/write at the register's assigned address.

```
STM32F4 memory map (partial):
  0x40000000 - 0x400233FF   APB1 peripherals (TIM2-7, USART2-5, SPI2-3, I2C1-3 ...)
  0x40010000 - 0x40014BFF   APB2 peripherals (USART1, SPI1, SYSCFG, TIM1, TIM8 ...)
  0x40020000 - 0x400287FF   AHB1 peripherals (DMA1, DMA2, RCC, GPIO ports ...)
  0x50000000 - 0x500607FF   AHB2 peripherals (USB OTG FS ...)
  0xE0000000 - 0xE00FFFFF   Cortex-M core peripherals (NVIC, SCB, SysTick, DWT ...)
```

### The Three Patterns for Peripheral Register Access

**Pattern 1: Direct pointer cast (raw, educational)**

```c
#define GPIOA_MODER  (*(volatile uint32_t *)0x40020000U)

/* Set PA5 as output (bits [11:10] = 0b01) */
GPIOA_MODER &= ~(0x3U << 10);   /* clear mode bits for pin 5 */
GPIOA_MODER |=  (0x1U << 10);   /* set output mode */
```

**Pattern 2: Struct overlay (CMSIS style, industry standard)**

```c
typedef struct {
    volatile uint32_t MODER;    /* 0x00: Mode register */
    volatile uint32_t OTYPER;   /* 0x04: Output type register */
    volatile uint32_t OSPEEDR;  /* 0x08: Output speed register */
    volatile uint32_t PUPDR;    /* 0x0C: Pull-up/pull-down register */
    volatile uint32_t IDR;      /* 0x10: Input data register */
    volatile uint32_t ODR;      /* 0x14: Output data register */
    volatile uint32_t BSRR;     /* 0x18: Bit set/reset register */
    volatile uint32_t LCKR;     /* 0x1C: Configuration lock register */
    volatile uint32_t AFR[2];   /* 0x20: Alternate function registers */
} GPIO_TypeDef;

#define GPIOA  ((GPIO_TypeDef *)0x40020000U)

/* Set PA5 as output */
GPIOA->MODER &= ~(0x3U << 10);
GPIOA->MODER |=  (0x1U << 10);
```

**Pattern 3: Bit-band access (Cortex-M3/M4 only)**

Bit-banding provides atomic single-bit read/modify/write access via alias addresses.

### Bit-Banding

Cortex-M3 and M4 provide two bit-band regions:

```
Region                Base          Alias Base     Size
------                ----          ----------     ----
SRAM bit-band         0x20000000    0x22000000     1 MB
Peripheral bit-band   0x40000000    0x42000000     1 MB

Alias address formula:
  alias = alias_base + (byte_offset * 32) + (bit_number * 4)

Example: bit 5 of register at 0x40020014 (GPIOA_ODR):
  byte_offset = 0x40020014 - 0x40000000 = 0x20014
  alias = 0x42000000 + (0x20014 * 32) + (5 * 4)
        = 0x42000000 + 0x400280 + 0x14
        = 0x42400294

Writing 1 to address 0x42400294 atomically sets bit 5 of GPIOA_ODR.
Writing 0 to address 0x42400294 atomically clears bit 5 of GPIOA_ODR.
```

### DMA with Peripheral Triggers

DMA (Direct Memory Access) engines transfer data between memory and peripherals without CPU intervention. A peripheral "trigger" (or "request") signal tells the DMA engine when to perform a transfer beat.

```
DMA channel configuration for UART RX:
  Source address:      &USART1->DR    (peripheral FIFO register address)
  Destination address: &rx_buffer[0]  (SRAM buffer)
  Transfer count:      N bytes
  Peripheral request:  USART1_RX      (fires when RXNE flag is set)
  Direction:           peripheral -> memory
  Increment:           source = no (always read same register), dest = yes

How it works:
  1. UART receives a byte, sets RXNE, asserts DMA request line.
  2. DMA engine reads USART1->DR (clearing RXNE) and writes to rx_buffer[N].
  3. DMA increments destination pointer, decrements count.
  4. When count reaches 0, DMA asserts its own interrupt.
  5. CPU processes the full rx_buffer, not individual bytes.
```

---

## Tier 1 -- Fundamentals

### Question F1
**Why must peripheral register accesses use pointer casts to `volatile uint32_t *` rather than plain `uint32_t *`? What can go wrong if the `volatile` qualifier is omitted?**

**Answer:**

Without `volatile`, the C compiler is allowed to:
1. **Cache the value:** After reading a register once, the compiler may assume it cannot change (no store in the C code modifies it) and use the cached register value for subsequent reads, never issuing a second memory access.
2. **Eliminate writes:** If the compiler determines that a write to a location is not read by subsequent C code, it may omit the write as a "dead store" optimisation.
3. **Re-order accesses:** The compiler can reorder reads and writes to non-volatile objects as long as the observable behaviour (within the abstract machine model) is unchanged.

**Concrete failure scenarios:**

```c
/* WRONG: compiler may load STATUS once and spin on the register value */
uint32_t *status_reg = (uint32_t *)0x40010008U;  /* no volatile */

while ((*status_reg & 0x1U) == 0U) {   /* Wait for TX_EMPTY */
    /* compiler sees: while ((constant & 1) == 0) {} -- infinite loop or no loop */
}

/* RIGHT: volatile forces a bus transaction on every read */
volatile uint32_t *status_reg = (volatile uint32_t *)0x40010008U;
while ((*status_reg & 0x1U) == 0U) {   /* Each loop iteration reads from the bus */
}
```

**Dead store elimination:**

```c
volatile uint32_t *uart_tx = (volatile uint32_t *)0x40010000U;
/* Without volatile, compiler may eliminate this "unused" write */
*uart_tx = 0x41U;   /* 'A' -- side effect is transmitting a byte */
```

**`volatile` tells the compiler:** "This memory location has side effects I cannot see. Issue a real bus transaction every time this is accessed, in program order."

---

### Question F2
**Why is the BSRR (Bit Set/Reset Register) on STM32 GPIO preferred over directly reading and writing the ODR for toggling individual output pins in an ISR?**

**Answer:**

**ODR (Output Data Register) approach -- read-modify-write:**

```c
/* Set PA5 high */
GPIOA->ODR |= (1U << 5);   /* read ODR, OR with mask, write ODR back */

/* Clear PA5 */
GPIOA->ODR &= ~(1U << 5);  /* read ODR, AND with inverse mask, write ODR back */
```

This is a three-instruction sequence (load, modify, store). On a single-core Cortex-M there is no hardware-level race between two threads, but there is a race with interrupts:

```
Main code:                     ISR (fires between load and store):
  load  R0, [GPIOA_ODR]          ; R0 = current ODR value
  ; << IRQ fires HERE >>
                                  load  R1, [GPIOA_ODR]
                                  bic   R1, R1, #(1<<3)  ; clear PA3
                                  str   R1, [GPIOA_ODR]  ; PA3 now low
  ; ISR returns
  orr   R0, R0, #(1<<5)         ; R0 is STALE (pre-ISR value, PA3 was high)
  str   R0, [GPIOA_ODR]          ; PA3 accidentally driven HIGH again!
```

**BSRR approach -- atomic set or clear:**

```c
/* Set PA5 high (bits [15:0] set the corresponding ODR bit) */
GPIOA->BSRR = (1U << 5);

/* Clear PA5 (bits [31:16] clear the corresponding ODR bit) */
GPIOA->BSRR = (1U << (5 + 16));
```

A write to BSRR is a single 32-bit store instruction. The hardware atomically applies the set/reset to the ODR without a read. There is no window in which an interrupt can observe an intermediate state.

**Additional benefit:** BSRR allows simultaneous set and reset of different pins in a single write:

```c
/* Set PA5 and PA7, clear PA3 and PA4 in one atomic operation */
GPIOA->BSRR = (1U << 5) | (1U << 7)          /* set bits [15:0] */
            | (1U << (3+16)) | (1U << (4+16)); /* reset bits [31:16] */
```

---

### Question F3
**What is memory-mapped I/O, and how does the processor distinguish between a memory access and a peripheral register access at the hardware level?**

**Answer:**

Memory-mapped I/O (MMIO) means peripheral registers occupy addresses in the processor's normal address space. There is no separate I/O address space or special I/O instructions (unlike x86 with its IN/OUT instructions). A CPU load or store to a peripheral register address is indistinguishable from a load or store to SRAM from the instruction's perspective.

**Hardware distinction via the interconnect (bus fabric):**

```
CPU issues: LDR R0, [0x40020014]   ; read from address 0x40020014

Cortex-M AHB bus fabric:
  1. Address decoder examines bits [31:0] of the address.
  2. 0x40020014 falls in the range 0x40020000-0x400203FF (GPIOA AHB1 slave).
  3. Decoder asserts the HSEL (select) signal for the GPIOA peripheral slave.
  4. The GPIOA peripheral receives the read transaction and drives HRDATA
     with the current ODR register value.
  5. CPU receives HRDATA and loads it into R0.

If address were 0x20000014 (SRAM):
  Step 2: Falls in range 0x20000000-0x2001FFFF (SRAM block).
  Step 3: SRAM controller selected instead.
  Steps 4-5: SRAM returns stored byte pattern.
```

The distinction is entirely in the address decoder -- a combinational logic block in the AHB interconnect that routes transactions to the correct slave based on address ranges.

**Implication for software:** Software does not need to do anything special to access peripheral registers. The same load/store instructions used for memory work for peripherals. The only software difference is the `volatile` qualifier, which is a compiler directive, not a hardware feature.

---

### Question F4
**A colleague writes `GPIOA->ODR = 0xFFFFU;` intending to set all 16 GPIO output pins high. What is the problem with this approach and what is the correct alternative?**

**Answer:**

**The problem:**

`ODR` is a 32-bit register. Writing 0xFFFFU sets bits [15:0] of ODR (pins 0-15) to the pattern 0xFFFF -- this appears correct at first glance.

However, `ODR` is a direct write to all output pin states simultaneously. If some pins are currently set to a value the code should not disturb (for example, pin 3 is being held high as a chip-select for an SPI device), this write will force all pins to the pattern in the write, overwriting any previous state for all 16 pins.

More specifically: ODR bits [31:16] are reserved. Writing them as 0 is correct per the datasheet, but the intent of "set all 16 pins high" is achieved by writing 0x0000FFFFU to ODR -- only if the intent is to set all 16 pins high *regardless of their current state*.

The more fundamental issue: directly assigning ODR affects all output pins in one write. In a system where different subsystems control different pins, this is a problem:

```c
/* Main code sets SPI CS on PA4 */
GPIOA->ODR |= (1U << 4);      /* PA4 high */

/* Later: ISR fires and does: */
GPIOA->ODR = 0xFFFFU;         /* Accidentally leaves PA4 high (no problem here) */
                               /* but also forces all other pins -- any PA3 that
                                  was intentionally low is now accidentally high */
```

**Correct alternatives:**

1. Use BSRR for atomic set/clear of individual pins without affecting others.
2. If truly setting all pins high is the intent, use `GPIOA->ODR = 0x0000FFFFU;` and document clearly that all pins are intentionally driven.
3. For partial pin control, always use BSRR or the read-modify-write on ODR with proper critical section protection.

---

## Tier 2 -- Intermediate

### Question I1
**Explain how to implement bit-banding for peripheral register access. Write a macro to compute the bit-band alias address and demonstrate its use for atomically toggling a single LED pin.**

**Answer:**

```c
/*
 * Bit-band alias address calculation for peripheral space.
 *
 * Peripheral bit-band region: 0x40000000 - 0x400FFFFF (1 MB)
 * Alias region base:           0x42000000
 *
 * Formula:
 *   alias = 0x42000000 + ((peripheral_addr - 0x40000000) * 32) + (bit_num * 4)
 *
 * Each word in the alias region corresponds to one bit in the bit-band region.
 * Writing 1 to the alias word sets the corresponding bit.
 * Writing 0 to the alias word clears the corresponding bit.
 * The write is atomic at the bus level (single 32-bit store).
 */

#define PERIPH_BB_ALIAS_ADDR(addr, bit)  \
    (0x42000000U + (((uint32_t)(addr) - 0x40000000U) << 5) + ((uint32_t)(bit) << 2))

#define PERIPH_BB(addr, bit)  \
    (*(volatile uint32_t *)PERIPH_BB_ALIAS_ADDR(addr, bit))

/*
 * Example: PA5 is connected to an LED on a Nucleo-F4 board.
 * GPIOA_ODR is at 0x40020014.
 * PA5 corresponds to bit 5 of ODR.
 *
 * Bit-band alias for PA5:
 *   alias = 0x42000000 + ((0x40020014 - 0x40000000) * 32) + (5 * 4)
 *         = 0x42000000 + (0x20014 * 32) + 20
 *         = 0x42000000 + 0x400280 + 0x14
 *         = 0x42400294
 */
#define GPIOA_ODR_ADDR  0x40020014U
#define PA5_ODR_BIT     5U

/* Atomic LED operations -- no read-modify-write, ISR-safe */
static inline void led_on(void)
{
    PERIPH_BB(GPIOA_ODR_ADDR, PA5_ODR_BIT) = 1U;  /* set bit: PA5 = 1 */
}

static inline void led_off(void)
{
    PERIPH_BB(GPIOA_ODR_ADDR, PA5_ODR_BIT) = 0U;  /* clear bit: PA5 = 0 */
}

static inline void led_toggle(void)
{
    uint32_t current = PERIPH_BB(GPIOA_ODR_ADDR, PA5_ODR_BIT);
    PERIPH_BB(GPIOA_ODR_ADDR, PA5_ODR_BIT) = current ^ 1U;
}
```

**Important limitation:** `led_toggle()` is still a read-modify-write at the C level (read alias, XOR, write alias), even though each individual step is atomic. An ISR between the read and the write of the alias could observe the intermediate state. For a true atomic toggle, use BSRR or `__LDREX`/`__STREX` exclusive access.

**Bit-banding for SRAM:**

The same principle applies to SRAM bit-band region (0x20000000 base, 0x22000000 alias). Useful for atomic flag manipulation between main and ISR without disabling interrupts:

```c
#define SRAM_BB(addr, bit) \
    (*(volatile uint32_t *)(0x22000000U + (((uint32_t)(addr) - 0x20000000U) << 5) + ((uint32_t)(bit) << 2)))

volatile uint8_t event_flags;    /* in SRAM at some address, say 0x20000100 */

/* Atomically set event bit 0 from ISR */
SRAM_BB(0x20000100U, 0U) = 1U;
```

---

### Question I2
**Describe how to configure a DMA channel to perform memory-to-peripheral transfers for a UART transmit operation. What fields must be configured and in what order must they be set?**

**Answer:**

```c
/*
 * Configure DMA1 Stream 7, Channel 4 for USART2 TX (STM32F4)
 *
 * DMA flow:
 *   Memory (tx_buffer in SRAM) -> Peripheral (USART2->DR)
 *   Trigger: USART2 TX data register empty (TXE flag -> DMA request)
 */

#define DMA_STREAM  DMA1_Stream7
#define DMA_CHANNEL 4U           /* USART2_TX mapped to DMA1 Stream7 Channel4 */

void uart_dma_tx_init(const uint8_t *tx_buf, uint16_t len)
{
    /* Step 1: Disable the stream before configuring it.
       Changing configuration while the stream is enabled is undefined behaviour. */
    DMA_STREAM->CR &= ~DMA_SxCR_EN;
    while (DMA_STREAM->CR & DMA_SxCR_EN) {}   /* wait until hardware confirms disabled */

    /* Step 2: Clear all interrupt flags for this stream in DMA_HIFCR/LIFCR */
    DMA1->HIFCR = DMA_HIFCR_CTCIF7 | DMA_HIFCR_CHTIF7 |
                  DMA_HIFCR_CTEIF7 | DMA_HIFCR_CDMEIF7 | DMA_HIFCR_CFEIF7;

    /* Step 3: Configure peripheral address (UART TX data register) */
    DMA_STREAM->PAR = (uint32_t)&USART2->DR;

    /* Step 4: Configure memory address (source buffer in SRAM) */
    DMA_STREAM->M0AR = (uint32_t)tx_buf;

    /* Step 5: Configure transfer count */
    DMA_STREAM->NDTR = len;

    /* Step 6: Configure control register:
         - Channel 4 selection (CH[2:0])
         - Memory data size: byte (MSIZE = 00)
         - Peripheral data size: byte (PSIZE = 00)
         - Memory increment: yes (MINC = 1, pointer advances after each byte)
         - Peripheral increment: no (PINC = 0, always write to same DR address)
         - Direction: memory to peripheral (DIR = 01)
         - Transfer complete interrupt enable (TCIE = 1)
    */
    DMA_STREAM->CR = (DMA_CHANNEL << DMA_SxCR_CHSEL_Pos)
                   | DMA_SxCR_MINC          /* memory increment */
                   | (0x01U << DMA_SxCR_DIR_Pos)  /* mem->periph */
                   | DMA_SxCR_TCIE;         /* TC interrupt */

    /* Step 7: Enable USART2 DMA transmit request */
    USART2->CR3 |= USART_CR3_DMAT;

    /* Step 8: Enable the DMA stream -- must be last */
    DMA_STREAM->CR |= DMA_SxCR_EN;
}

/* DMA transfer-complete ISR */
void DMA1_Stream7_IRQHandler(void)
{
    if (DMA1->HISR & DMA_HISR_TCIF7) {
        DMA1->HIFCR = DMA_HIFCR_CTCIF7;     /* clear flag */
        USART2->CR3 &= ~USART_CR3_DMAT;      /* disable DMA request */
        /* Signal application that TX is complete */
        tx_dma_complete_flag = 1;
    }
}
```

**Critical ordering requirements:**

1. Always disable the stream (`EN=0`) and wait for hardware confirmation before changing any configuration register.
2. Clear the FIFO error and transfer error interrupt flags before re-enabling; stale error flags from a previous transaction would immediately trigger the error interrupt.
3. Set all configuration (PAR, M0AR, NDTR, CR) before setting `EN=1`. Setting `EN` is the "go" signal -- the DMA engine begins as soon as both `EN` and the peripheral request are asserted.
4. Enable the UART DMA request (`DMAT`) only after the DMA channel is fully configured; premature enabling can trigger a DMA burst before the destination address is set.

---

### Question I3
**What is the difference between a DMA transfer error and a FIFO error on STM32 DMA? What conditions cause each and how should the ISR handle them?**

**Answer:**

STM32 DMA streams have a FIFO (First-In-First-Out buffer) that buffers data between the memory and peripheral buses, allowing burst transfers that do not stall the bus.

**Transfer Error (TEIF -- Transfer Error Interrupt Flag):**

Occurs when the DMA engine attempts a bus transaction and receives a bus error response (e.g., AHB HRESP = ERROR):
- DMA attempts to read from or write to an invalid address (outside any valid memory region).
- The source or destination address is misaligned relative to the data size (e.g., 32-bit transfer to an address not divisible by 4).
- The DMA master port cannot access the target slave (e.g., wrong AHB bus, access permission violation).

**FIFO Error (FEIF -- FIFO Error Interrupt Flag):**

Occurs when there is a mismatch between the FIFO and the configured transfer sizes, or when the FIFO threshold condition cannot be met:
- FIFO underrun: the FIFO empties before the burst to the peripheral is complete (MINC was wrong, or FIFO threshold too high for the transfer size).
- FIFO overrun: data arrives from memory faster than the peripheral can accept it.
- Invalid FIFO configuration: combinations of MBURST, MSIZE, and FIFO threshold that violate the FIFO organisation rules.

**ISR error handling pattern:**

```c
void DMA1_Stream7_IRQHandler(void)
{
    uint32_t hisr = DMA1->HISR;

    /* Transfer complete */
    if (hisr & DMA_HISR_TCIF7) {
        DMA1->HIFCR = DMA_HIFCR_CTCIF7;
        on_tx_complete();
    }

    /* Transfer error -- bus fault */
    if (hisr & DMA_HISR_TEIF7) {
        DMA1->HIFCR = DMA_HIFCR_CTEIF7;
        /* Disable the stream */
        DMA_STREAM->CR &= ~DMA_SxCR_EN;
        /* Log error, attempt recovery or reset peripheral */
        dma_error_handler(DMA_ERROR_TRANSFER);
    }

    /* FIFO error -- configuration mismatch */
    if (hisr & DMA_HISR_FEIF7) {
        DMA1->HIFCR = DMA_HIFCR_CFEIF7;
        /* FIFO errors are often non-fatal for UART (data may still transfer)
           but indicate a configuration issue that should be investigated */
        dma_error_handler(DMA_ERROR_FIFO);
    }

    /* Direct mode error */
    if (hisr & DMA_HISR_DMEIF7) {
        DMA1->HIFCR = DMA_HIFCR_CDMEIF7;
        dma_error_handler(DMA_ERROR_DIRECT_MODE);
    }
}
```

**What would you do if...** a DMA transfer to UART was silently corrupting the first few bytes? Check: (1) the memory address is not in a region that requires cache maintenance (Cortex-M7: D-cache must be cleaned before DMA reads from cached SRAM), (2) the NDTR value is set before EN=1 (if set after, the first beat may use an old count), and (3) the UART DMAT bit is enabled -- without it, no DMA requests are generated and the DMA may complete immediately with 0 transfers.

---

### Question I4
**Explain the difference between DMA circular mode and normal (one-shot) mode. When is each appropriate, and what happens if the CPU does not process data fast enough in circular mode?**

**Answer:**

**Normal (one-shot) mode:**

```
Transfer starts at M0AR.
After NDTR transfers complete:
  - DMA disables itself (EN automatically cleared).
  - Transfer complete interrupt fires.
  - CPU must re-configure and re-enable for the next transfer.

Use cases: Single packet TX, single burst RX of known length.
```

**Circular mode:**

```
Transfer starts at M0AR.
After NDTR transfers complete:
  - NDTR automatically reloads to its initial value.
  - M0AR resets to its initial value.
  - DMA continues without stopping.
  - Half-transfer interrupt fires at NDTR/2 point.
  - Transfer complete interrupt fires at NDTR == 0 (before reload).

Memory layout (double buffer in software):
  [   first half   |   second half  ]  <- total circular buffer
   ^- Half-TC fires    ^- Full-TC fires

CPU processes first half while DMA fills second half, and vice versa.
```

**Use cases for circular mode:**

- Continuous ADC sampling (DMA fills a ring buffer indefinitely).
- UART RX with unknown message length (DMA places bytes in a circular buffer; CPU scans for message delimiters).
- I2S/SAI audio streaming (DMA continuously feeds the DAC from a ping-pong audio buffer).

**What happens if the CPU does not keep up:**

In circular mode, the DMA wraps around and starts overwriting the oldest data. There is no hardware interlocking -- the DMA does not stop or pause waiting for the CPU.

```
Buffer: [ byte 0, byte 1, byte 2, ... byte 255 ]
DMA writes: byte 0 ... byte 255, then wraps to byte 0 again.

If CPU processes only up to byte 100 before DMA wraps:
  DMA begins overwriting byte 0 while CPU is still reading byte 100-255.
  Bytes 0-100 are now corrupted (overwritten).
  This is an overrun condition.
```

**Detecting and handling overrun:**

```c
/* Use the half-TC and TC interrupts to maintain a software watermark */

static volatile uint32_t dma_write_ptr = 0;  /* updated by ISR */
static uint32_t           cpu_read_ptr  = 0;  /* updated by main */

void DMA1_Stream1_IRQHandler(void)
{
    if (DMA1->LISR & DMA_LISR_HTIF1) {
        DMA1->LIFCR = DMA_LIFCR_CHTIF1;
        dma_write_ptr = DMA_BUF_SIZE / 2;  /* DMA is filling second half */
    }
    if (DMA1->LISR & DMA_LISR_TCIF1) {
        DMA1->LIFCR = DMA_LIFCR_CTCIF1;
        dma_write_ptr = DMA_BUF_SIZE;       /* DMA wrapped, filling first half */
    }
}

/* In main: before reading, check for overrun */
uint32_t bytes_pending = (dma_write_ptr - cpu_read_ptr + DMA_BUF_SIZE) % DMA_BUF_SIZE;
if (bytes_pending > DMA_BUF_SIZE / 2) {
    /* Overrun: more than half a buffer behind -- some data was overwritten */
    handle_overrun_error();
}
```

---

## Tier 3 -- Advanced

### Question A1
**A system uses DMA to transfer data from SRAM to a peripheral on a Cortex-M7 with a data cache. Describe all the cache coherency issues that can arise and the exact sequence of cache maintenance operations required for each transfer direction.**

**Answer:**

The Cortex-M7's D-cache sits between the CPU and the AHB bus. DMA accesses bypass the cache entirely -- the DMA master writes directly to physical SRAM, and the CPU's cache may hold stale copies.

**Case 1: CPU writes to a buffer, DMA reads it (memory-to-peripheral, e.g., UART TX)**

```
Timeline:
  1. CPU writes tx_buffer[] -- data enters the D-cache (dirty cache lines).
  2. Cache does NOT immediately write back to SRAM (write-back policy).
  3. DMA begins reading from physical SRAM address of tx_buffer[].
  4. DMA reads STALE data (the version in SRAM before the CPU's writes).
  5. UART transmits wrong bytes.

Required cache maintenance BEFORE starting DMA:
  SCB_CleanDCache_by_Addr((uint32_t *)tx_buffer, sizeof(tx_buffer));
  /* or: SCB_CleanDCache() for entire cache */
  __DSB();   /* ensure clean operation completes */
  /* Now SRAM holds the CPU's writes. DMA can safely read. */
```

**Case 2: DMA writes to a buffer, CPU reads it (peripheral-to-memory, e.g., UART RX)**

```
Timeline:
  1. CPU pre-reads or previously accessed rx_buffer[] -- stale data in cache.
  2. DMA writes new data to physical SRAM address of rx_buffer[].
  3. Cache lines covering rx_buffer[] are still "valid" (not invalid).
  4. CPU reads rx_buffer[] -- cache hit, returns old (pre-DMA) data.
  5. CPU processes stale data instead of received bytes.

Required cache maintenance AFTER DMA transfer completes (in DMA TC ISR):
  SCB_InvalidateDCache_by_Addr((uint32_t *)rx_buffer, sizeof(rx_buffer));
  __DSB();
  /* Now cache lines are invalidated. Next CPU read fetches from SRAM
     (which holds the DMA-written data). */
```

**Alignment requirement:**

Cache maintenance operations work on full cache lines (32 bytes on Cortex-M7). The buffer must be aligned to 32 bytes and its size must be a multiple of 32 bytes; otherwise the clean/invalidate operation affects partial cache lines, potentially corrupting adjacent data:

```c
/* Correct: 32-byte aligned buffer, size padded to cache line multiple */
__attribute__((aligned(32)))
static uint8_t tx_buffer[256];   /* 256 is a multiple of 32: OK */

__attribute__((aligned(32)))
static uint8_t rx_buffer[300];   /* 300 is NOT a multiple of 32: danger */
/* Use: static uint8_t rx_buffer[320]; -- round up to next multiple of 32 */
```

**Complete sequence for a TX (CPU->DMA) operation:**

```c
void start_uart_tx_dma(const uint8_t *data, uint16_t len)
{
    /* 1. Ensure buffer is 32-byte aligned (checked at compile time or asserted) */

    /* 2. Clean D-cache: flush CPU's dirty cache lines to SRAM */
    SCB_CleanDCache_by_Addr((uint32_t *)data, len);

    /* 3. DSB: ensure clean completes before DMA is enabled */
    __DSB();

    /* 4. Configure DMA and enable */
    DMA_STREAM->M0AR = (uint32_t)data;
    DMA_STREAM->NDTR = len;
    DMA_STREAM->CR  |= DMA_SxCR_EN;
}
```

**Complete sequence for an RX (DMA->CPU) operation:**

```c
void start_uart_rx_dma(uint8_t *buf, uint16_t len)
{
    /* 1. Invalidate D-cache BEFORE starting DMA -- ensures no stale lines exist */
    /* Doing it before (not after) avoids the race where CPU reads stale data
       between DMA starting and ISR-driven invalidation */
    SCB_InvalidateDCache_by_Addr((uint32_t *)buf, len);
    __DSB();

    /* 2. Configure DMA and enable */
    DMA_STREAM->M0AR = (uint32_t)buf;
    DMA_STREAM->NDTR = len;
    DMA_STREAM->CR  |= DMA_SxCR_EN;
}

/* In the DMA TC ISR -- data is now in SRAM, cache is already invalidated */
void DMA1_Stream5_IRQHandler(void)
{
    if (DMA1->HISR & DMA_HISR_TCIF5) {
        DMA1->HIFCR = DMA_HIFCR_CTCIF5;
        /* No further cache maintenance needed (invalidated before DMA started) */
        process_received_data(rx_buffer, transfer_len);
    }
}
```

**Alternative: mark DMA buffers as non-cacheable using MPU:**

Configure the MPU to mark the DMA buffer region as Device or Normal Non-Cacheable memory. All accesses to that region bypass the cache entirely:

```c
/* MPU region for DMA buffers: 0x24000000, 64KB, Non-Cacheable */
MPU->RNR  = 0;                      /* region 0 */
MPU->RBAR = 0x24000000U;            /* base address */
MPU->RASR = MPU_RASR_ENABLE_Msk
          | (0x0FU << MPU_RASR_SIZE_Pos)  /* 64 KB */
          | (0x03U << MPU_RASR_AP_Pos)    /* full access */
          | (0x1U << MPU_RASR_TEX_Pos)    /* TEX=1, C=0, B=0 = non-cacheable */
          | MPU_RASR_XN_Msk;              /* not executable */
MPU->CTRL = MPU_CTRL_ENABLE_Msk | MPU_CTRL_PRIVDEFENA_Msk;
```

With non-cacheable DMA buffers, no software cache maintenance is required at all -- the correct choice for high-throughput DMA-heavy designs.

---

### Question A2
**What is the purpose of write buffers on Cortex-M and how can they cause subtle bugs in peripheral register sequences? Give a concrete example and explain the fix.**

**Answer:**

Cortex-M3/M4/M7 implement store buffers (write buffers) that allow stores to complete "from the CPU's perspective" before the actual bus transaction is committed. This optimises CPU pipeline throughput -- the CPU does not stall waiting for each store to propagate to the bus.

For Normal cacheable memory, this is transparent and beneficial. For Device memory (peripheral registers), it can cause ordering violations.

**Concrete example -- UART enable sequence:**

The application note for a specific UART requires this exact sequence:
1. Write the baud rate divisor to BRR.
2. Write the enable bits to CR1.
3. The first write to DR (transmit) must not occur until CR1 reflects the enabled state.

```c
/* UART init -- appears correct, but may fail due to write buffering */
USART1->BRR = 0x0683U;   /* step 1: baud rate */
USART1->CR1 = 0x200CU;   /* step 2: enable (UE, TE, RE bits) */
/* CPU continues, store buffer may still be draining */

/* Some other code runs here, generating more stores */

USART1->DR = 0x41U;      /* step 3: transmit 'A' */
/* BUG: If the CR1 store is still in the write buffer and has not reached
   the peripheral yet, the peripheral may not be enabled when DR is written.
   The byte may be lost or the peripheral may behave unpredictably. */
```

**Why this happens:**

The store buffer allows the CPU to proceed past the `USART1->CR1 = ...` store without waiting for the AHB bus transaction to complete. A subsequent store to `USART1->DR` may be issued on the bus before the `CR1` store has been acknowledged by the peripheral.

**The fix: DSB (Data Synchronisation Barrier):**

```c
USART1->BRR = 0x0683U;
USART1->CR1 = 0x200CU;
__DSB();                 /* stall CPU until all pending stores have completed the bus */
/* Now CR1 is guaranteed to be committed to the peripheral */
USART1->DR = 0x41U;     /* safe: peripheral is definitely enabled */
```

`__DSB()` drains the store buffer: execution does not proceed past the DSB until all pending memory transactions (including stores to peripheral registers) have completed on the AHB bus.

**When is this an issue in practice?**

On Cortex-M3/M4 with Device-type memory (which peripheral space always is), the hardware already enforces some ordering. However, when multiple write operations to different peripheral registers must appear in a specific order at the peripheral, DSB is the correct mechanism to enforce that ordering. This is especially important in:

- Clock enable then peripheral configure sequences (RCC_AHB1ENR then GPIOx->MODER).
- DMA configuration then enable (`EN=1` must be the last write).
- Any sequence described in a silicon errata as requiring specific ordering.

**Note on ARMv7-M architecture:** The architecture does not guarantee that writes to Device memory are completed before subsequent writes, unless a DSB (or a load from any address) is used as a barrier. The distinction between write buffer behaviour on Cortex-M3 vs M7 is implementation-specific, making DSB the portable and safe choice.
