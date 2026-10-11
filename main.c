#include <stdint.h>

#define MMIO_BASE       0xfe000000UL

#define GPIO_BASE       (MMIO_BASE + 0x00200000)
#define UART0_BASE      (MMIO_BASE + 0x00201000)

#define GPFSEL1         (GPIO_BASE + 0x04)
#define GPPUPPDN0       (GPIO_BASE + 0xe4)

#define UART0_DR        (UART0_BASE + 0x00)
#define UART0_FR        (UART0_BASE + 0x18)
#define UART0_IBRD      (UART0_BASE + 0x24)
#define UART0_FBRD      (UART0_BASE + 0x28)
#define UART0_LCRH      (UART0_BASE + 0x2c)
#define UART0_CR        (UART0_BASE + 0x30)
#define UART0_ICR       (UART0_BASE + 0x44)

#define UART_FR_TXFF    (1U << 5)

#define UART_CR_UARTEN  (1U << 0)
#define UART_CR_TXE     (1U << 8)
#define UART_CR_RXE     (1U << 9)

#define UART_LCRH_FEN   (1U << 4)
#define UART_LCRH_WLEN8 (3U << 5)

static inline void
mmio_write(uintptr_t reg, uint32_t value)
{
  *(volatile uint32_t *)reg = value;
}

static inline uint32_t
mmio_read(uintptr_t reg)
{
  return *(volatile uint32_t *)reg;
}

static void
uart_init(void)
{
  uint32_t value;

  /*
   * Disable UART while configuring it.
   */
  mmio_write(UART0_CR, 0);

  /*
   * GPIO14 and GPIO15 -> ALT0:
   *
   * GPIO14: GPFSEL1 bits 14:12 = 100
   * GPIO15: GPFSEL1 bits 17:15 = 100
   */
  value = mmio_read(GPFSEL1);

  value &= ~((7U << 12) | (7U << 15));
  value |=  ((4U << 12) | (4U << 15));

  mmio_write(GPFSEL1, value);

  /*
   * BCM2711 has dedicated pull-up/down registers.
   *
   * Disable pulls on GPIO14 and GPIO15.
   *
   * GPIO14 = bits 29:28
   * GPIO15 = bits 31:30
   */
  value = mmio_read(GPPUPPDN0);
  value &= ~((3U << 28) | (3U << 30));
  mmio_write(GPPUPPDN0, value);

  /*
   * Clear pending UART interrupts.
   */
  mmio_write(UART0_ICR, 0x7ff);

  /*
   * Baud rate:
   *
   * Assume UART clock = 48 MHz.
   *
   * Baud divisor = 48,000,000 / (16 * 115200)
   *              = 26.041666...
   *
   * IBRD = 26
   * FBRD = round(0.041666 * 64) = 3
   */
  mmio_write(UART0_IBRD, 26);
  mmio_write(UART0_FBRD, 3);

  /*
   * 8 data bits, FIFO enabled, no parity, one stop bit.
   */
  mmio_write(UART0_LCRH,
      UART_LCRH_WLEN8 |
      UART_LCRH_FEN);

  /*
   * Enable UART, transmitter and receiver.
   */
  mmio_write(UART0_CR,
      UART_CR_UARTEN |
      UART_CR_TXE |
      UART_CR_RXE);
}

static void
uart_putc(char c)
{
  while (mmio_read(UART0_FR) & UART_FR_TXFF)
    ;

  mmio_write(UART0_DR, (uint32_t)c);
}

static void
uart_puts(const char *s)
{
  while (*s != '\0') {
    if (*s == '\n')
      uart_putc('\r');

    uart_putc(*s++);
  }
}

void
main(void)
{
  uart_init();

  uart_puts("Hello, world!\n");

  for (;;)
    __asm__ volatile("wfe");
}

