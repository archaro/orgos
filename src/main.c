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

  // Ask the firmware for the UART clock
  clock_ok = mailbox_get_clock_rate(
      MAILBOX_CLOCK_UART, &clock_hz);

  // A sensible fallback, just in case
  if (!clock_ok)
    clock_hz = 48000000U;

  uart_init(clock_hz, UART_BAUD);

  uart_puts("\nOrgOS - bare metal Raspberry Pi 4\n");
  uart_puts("--------------------------------\n");

  // Configure the UART clock
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

  // Demonstrate the monotonic system timer
  uart_puts("\nSystem timer test\n");

  uint64_t start = timer_now_us();

  uart_puts("Initial counter, lower 32 bits: ");
  uart_putu32((uint32_t)start);
  uart_puts(" us\n\n");

  for (unsigned int i = 1; i <= 5; i++) {
    timer_delay_ms(1000);

    uint64_t elapsed = timer_now_us() - start;

    // The timer is 64-bit, but we can only output 32-bit unsigned ints for now
    // We can improve this later when we implement a formatting library
    uart_puts("Elapsed: ");
    uart_putu32((uint32_t)(elapsed / 1000U));
    uart_puts(" ms\n");
  }

  uart_puts("\nTimer test complete.\n");

  uart_puts("\nOrgOS interactive console\nThe OS handles echo.  Make sure local echo is off.\n");
  console_run();
}

