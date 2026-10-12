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

void uart_puts(const char *s) {
  while (*s) {
    if (*s == '\n')
      uart_putc('\r');

    uart_putc(*s++);
  }
}

void uart_putu32(uint32_t value) {
  char buffer[10];
  unsigned int length = 0;

  do {
    buffer[length++] = '0' + (value % 10U);
    value /= 10U;
  } while (value);

  while (length)
    uart_putc(buffer[--length]);
}

bool uart_has_data(void) {
  return !(mmio_read(UART0_FR) & UART_FR_RXFE);
}

char uart_getc(void) {
  while (!uart_has_data())
    ;

  return (char)(mmio_read(UART0_DR) & 0xffU);
}
