#include <stdbool.h>
#include <stddef.h>

#include "console.h"
#include "uart.h"

#define CONSOLE_LINE_SIZE 256

void console_run(void) {
  char line[CONSOLE_LINE_SIZE];
  size_t length = 0;
  bool overflow = false;
  bool last_was_cr = false;

  uart_puts("Console ready.\n");

  for (;;) {
    char c = uart_getc();

    // Treat CRLF as a single line ending
    if (c == '\n' && last_was_cr) {
      last_was_cr = false;
      continue;
    }

    last_was_cr = (c == '\r');

    // End of line
    if (c == '\r' || c == '\n') {
      if (overflow) {
        uart_puts("Input line too long.\n");
      } else {
        line[length] = '\0';

        uart_puts("Received: ");
        uart_puts(line);
        uart_puts("\n");
      }

      length = 0;
      overflow = false;
      continue;
    }

    // Backspace or DEL
    if (c == '\b' || c == 0x7f) {
      if (length > 0)
        length--;

      continue;
    }

    // Ignore other ASCII control characters
    if ((unsigned char)c < 0x20)
      continue;

    // Once full, discard until the next newline
    if (overflow)
      continue;

    if (length < sizeof(line) - 1) {
      line[length++] = c;
    } else {
      overflow = true;
    }
  }
}

