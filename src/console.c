
#include <stddef.h>
#include <stdbool.h>

#include "console.h"
#include "uart.h"

#define CONSOLE_LINE_SIZE 256

typedef void (*console_command_fn)(const char *args);

#include "timer.h"

static void cmd_clock(const char *args) {
  (void)args;

  // This function tells us some interesting information about the timer.
  uint64_t tick = timer_now_us();

  uart_puts("System clock:\n");
  uart_puts("  Counter    : ");
  uart_putu64(tick);
  uart_puts(" ticks\n");
  uart_puts("  Frequency  : 1000000 Hz\n");
  uart_puts("  Resolution : 1 us\n");
  uart_puts("  Elapsed    : ");
  uart_putu64(tick / 1000000U);
  uart_putc('.');

  uint32_t fraction = (uint32_t)(tick % 1000000U);
  uint32_t divisor = 100000U;
  while (divisor) {
    uart_putc('0' + (fraction / divisor) % 10U);
    divisor /= 10U;
  }
  uart_puts(" seconds since time began.\n");
}

typedef struct {
  const char *name;
  console_command_fn handler;
} console_command_t;

// Commands will be added here as they are implemented.
// The NULL entry terminates the table.
static const console_command_t commands[] = {
  { "clock", cmd_clock },
  { NULL, NULL }
};

static bool is_space(char c) {
  return c == ' '  ||
         c == '\t' ||
         c == '\n' ||
         c == '\r' ||
         c == '\v' ||
         c == '\f';
}

static bool string_equal(const char *a, const char *b) {
  while (*a && *b) {
    if (*a != *b)
      return false;

    a++;
    b++;
  }

  return *a == *b;
}

static void console_prompt(void) {
  uart_puts("OrgOS> ");
}

// Dispatch a completed, NUL-terminated input line.
// The buffer may be modified in place.
static void console_dispatch(char *line) {
  char *command = line;
  char *args;

  // Skip leading whitespace.
  while (is_space(*command))
    command++;

  // An empty line is not a command.
  if (*command == '\0')
    return;

  // Locate the end of the first word.
  char *p = command;

  while (*p && !is_space(*p))
    p++;

  if (*p) {
    // Terminate the command name and skip
    // whitespace preceding its arguments.
    *p++ = '\0';

    while (is_space(*p))
      p++;
  }

  args = p;

  // Look up the command.
  for (size_t i = 0; commands[i].name != NULL; i++) {
    if (string_equal(command, commands[i].name)) {
      commands[i].handler(args);
      return;
    }
  }

  uart_puts("No such command.\n");
}

void console_run(void) {
  char line[CONSOLE_LINE_SIZE];
  size_t length = 0;
  bool last_was_cr = false;

  uart_puts("Console ready.\n");
  console_prompt();

  for (;;) {
    unsigned char c = (unsigned char)uart_getc();

    // Treat CRLF as one newline.
    if (c == '\n' && last_was_cr) {
      last_was_cr = false;
      continue;
    }

    last_was_cr = (c == '\r');

    // A completed input line.
    if (c == '\r' || c == '\n') {
      uart_puts("\n");

      line[length] = '\0';
      console_dispatch(line);

      length = 0;
      console_prompt();
      continue;
    }

    // Backspace (0x08) or DEL (0x7f).
    // Remove one character from the buffer
    // and erase it from the terminal.
    if (c == '\b' || c == 0x7f) {
      if (length > 0) {
        length--;
        uart_puts("\b \b");
      }

      continue;
    }

    // Convert TAB to one ordinary space.
    // The display and buffer must agree.
    if (c == '\t')
      c = ' ';

    // Ignore unsupported control characters
    // and non-ASCII input.
    if (c < 0x20 || c > 0x7e)
      continue;

    // Reserve the final byte for '\0'.
    // If the line is full, ring the terminal
    // bell and refuse the additional character.
    // The user can still use backspace.
    if (length >= sizeof(line) - 1) {
      uart_putc('\a');
      continue;
    }

    // Append and echo the character.
    line[length++] = (char)c;
    uart_putc((char)c);
  }
}

