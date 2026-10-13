#include <stdint.h>
#include <stdbool.h>

#include "mailbox.h"
#include "timer.h"
#include "uart.h"
#include "console.h"

#define UART_BAUD 115200U

void main(void) {
  uint32_t clock_hz;
  bool clock_ok;

  // Record the time that the OS actually starts
  timer_init();

  // Ask the firmware for the UART clock
  clock_ok = mailbox_get_clock_rate(
      MAILBOX_CLOCK_UART, &clock_hz);

  // A sensible fallback, just in case
  if (!clock_ok)
    clock_hz = 48000000U;

  uart_init(clock_hz, UART_BAUD);

  console_puts("\nOrgOS - bare metal Raspberry Pi 4\n");
  console_puts("--------------------------------\n");

  // Configure the UART clock
  if (clock_ok) {
    console_puts("Firmware mailbox: OK\n");
  } else {
    console_puts("Firmware mailbox: FAILED (using fallback)\n");
  }

  console_puts("UART clock: ");
  console_putu32(clock_hz);
  console_puts(" Hz\n");

  console_puts("Baud rate: ");
  console_putu32(UART_BAUD);
  console_puts("\n");

  console_puts("\nOrgOS interactive console.\n");
  console_run();
}

