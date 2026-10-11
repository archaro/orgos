#include <stdint.h>
#include <stdbool.h>

#include "mailbox.h"
#include "uart.h"

#define UART_BAUD 115200U

void main(void) {
  uint32_t clock_hz;
  bool clock_ok;

  // Ask the firmware for the UART clock
  clock_ok = mailbox_get_clock_rate(
      MAILBOX_CLOCK_UART, &clock_hz);

  // A sensible fallback, just in case
  if (!clock_ok)
    clock_hz = 48000000U;

  uart_init(clock_hz, UART_BAUD);

  uart_puts("\nOrgOS - bare metal Raspberry Pi 4\n");
  uart_puts("--------------------------------\n");

  if (clock_ok) {
    uart_puts("Firmware mailbox: OK\n");
  } else {
    uart_puts("Firmware mailbox: FAILED (using fallback)\n");
  }

  uart_puts("UART clock: ");
  uart_putu32(clock_hz);
  uart_puts(" Hz\n");

  uart_puts("Baud rate: ");
  uart_putu32(UART_BAUD);
  uart_puts("\n");

  uart_puts("\nHello, world!\n");

  for (;;)
    __asm__ volatile("wfe");
}

