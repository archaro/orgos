#include <stdint.h>

#include "mmio.h"
#include "uart.h"

#define GPIO_BASE       (MMIO_BASE + 0x00200000UL)
#define UART0_BASE      (MMIO_BASE + 0x00201000UL)

#define GPFSEL1         (GPIO_BASE + 0x04)
#define GPPUPPDN0       (GPIO_BASE + 0xe4)

#define UART0_DR        (UART0_BASE + 0x00)
#define UART0_FR        (UART0_BASE + 0x18)
#define UART0_IBRD      (UART0_BASE + 0x24)
#define UART0_FBRD      (UART0_BASE + 0x28)
#define UART0_LCRH      (UART0_BASE + 0x2c)
#define UART0_CR        (UART0_BASE + 0x30)
#define UART0_ICR       (UART0_BASE + 0x44)

#define UART_FR_RXFE	(1U << 4)
#define UART_FR_TXFF    (1U << 5)

#define UART_CR_UARTEN  (1U << 0)
#define UART_CR_TXE     (1U << 8)
#define UART_CR_RXE     (1U << 9)

#define UART_LCRH_FEN   (1U << 4)
#define UART_LCRH_WLEN8 (3U << 5)

#define UART0_IFLS (UART0_BASE + 0x34)
#define UART0_IMSC (UART0_BASE + 0x38)
#define UART0_MIS  (UART0_BASE + 0x40)

#define UART_INT_RX (1U << 4)
#define UART_INT_RT (1U << 6)

// Interrupt-driven UART receive buffer.
// The interrupt handler owns rx_head.
// The foreground reader owns rx_tail.
// The buffer size must be a power of two.
#define UART_RX_BUFFER_SIZE 256U
#define RX_MASK (UART_RX_BUFFER_SIZE - 1U)
static uint8_t rx_buffer[UART_RX_BUFFER_SIZE];
static volatile uint32_t rx_head;
static volatile uint32_t rx_tail;
static volatile uint32_t rx_overruns;


void uart_init(uint32_t clock_hz, uint32_t baud) {
  uint32_t value;

  // Disable UART before reconfiguration.
  mmio_write(UART0_CR, 0);

  // GPIO14 / GPIO15 -> ALT0.
  value = mmio_read(GPFSEL1);
  value &= ~((7U << 12) | (7U << 15));
  value |=  ((4U << 12) | (4U << 15));
  mmio_write(GPFSEL1, value);

  // Disable GPIO pulls.
  value = mmio_read(GPPUPPDN0);
  value &= ~((3U << 28) | (3U << 30));
  mmio_write(GPPUPPDN0, value);

  // Clear outstanding interrupts.
  mmio_write(UART0_ICR, 0x7ff);

  // PL011 baud divisor:
  // divisor = clock_hz / (16 * baud)
  // Calculate divisor * 64 using integer arithmetic,
  // rounding to the nearest 1/64.
  uint64_t scaled = ((uint64_t)clock_hz * 4U + baud / 2U) / baud;
  uint32_t ibrd = (uint32_t)(scaled / 64U);
  uint32_t fbrd = (uint32_t)(scaled % 64U);

  mmio_write(UART0_IBRD, ibrd);
  mmio_write(UART0_FBRD, fbrd);

  // Eight data bits, FIFO enabled.
  mmio_write(UART0_LCRH, UART_LCRH_WLEN8 | UART_LCRH_FEN);

  // Enable UART and both directions.
  mmio_write(UART0_CR,
    UART_CR_UARTEN |
    UART_CR_TXE |
    UART_CR_RXE);
}

void uart_putc(char c) {
  while (mmio_read(UART0_FR) & UART_FR_TXFF)
    ;

  mmio_write(UART0_DR, (uint32_t)c);
}

bool uart_has_data(void) {
  return rx_head != rx_tail;
}

char uart_getc(void) {
  while (!uart_has_data())
    ;

  uint32_t tail = rx_tail;
  char c = (char)rx_buffer[tail];

  __asm__ volatile("dmb ish" ::: "memory");

  rx_tail = (tail + 1U) & RX_MASK;

  return c;
}

uint32_t uart_rx_overruns(void) {
  return rx_overruns;
}

void uart_enable_rx_irq(void) {
  // Select the lowest receive FIFO threshold:
  // RXIFLSEL = 000, meaning 1/8 full.
  uint32_t value = mmio_read(UART0_IFLS);
  value &= ~(7U << 3);
  mmio_write(UART0_IFLS, value);

  // Clear old receive interrupt conditions.
  mmio_write(UART0_ICR, UART_INT_RX | UART_INT_RT);

  // Enable receive and receive-timeout interrupts.
  // Other UART interrupt sources remain disabled.
  mmio_write(UART0_IMSC, UART_INT_RX | UART_INT_RT);
}

void uart_handle_irq(void) {
  // Drain the hardware FIFO completely.
  // A receive interrupt represents potentially
  // several bytes, not necessarily just one.
  while (!(mmio_read(UART0_FR) & UART_FR_RXFE)) {
    uint32_t data = mmio_read(UART0_DR);
    uint32_t next = (rx_head + 1U) & RX_MASK;

    if (next == rx_tail) {
      // Software buffer full.
      // Drop the incoming byte and count it.
      rx_overruns++;
      continue;
    }

    rx_buffer[rx_head] = (uint8_t)(data & 0xffU);

    // Publish the byte before updating the head.
    __asm__ volatile("dmb ish" ::: "memory");

    rx_head = next;
  }

  // Clear the RX and receive-timeout conditions.
  // The GIC interrupt will be ended by irq_dispatch().
  mmio_write(UART0_ICR, UART_INT_RX | UART_INT_RT);
}

