
#include <stddef.h>
#include <stdbool.h>

#include "console.h"
#include "uart.h"
#include "irq.h"

#define CONSOLE_LINE_SIZE 256

typedef void (*console_command_fn)(const char *args);

#include "timer.h"

static void cmd_timer(const char *args) {
  (void)args;

  // This function tells us some interesting information about the timer.
  uint64_t tick = timer_now_us();

  console_puts("System timer:\n");
  console_puts("  Counter    : ");
  console_putu64(tick);
  console_puts(" ticks\n");
  console_puts("  Frequency  : 1000000 Hz\n");
  console_puts("  Resolution : 1 us\n");
  console_puts("  Elapsed    : ");
  console_putu64(tick / 1000000U);
  console_putc('.');

  uint32_t fraction = (uint32_t)(tick % 1000000U);
  uint32_t divisor = 100000U;
  while (divisor) {
    console_putc('0' + (fraction / divisor) % 10U);
    divisor /= 10U;
  }
  console_puts(" seconds since time began.\n");
}

static void cmd_uptime(const char *args) {
  (void)args;

  uint64_t seconds = timer_uptime_us() / 1000000U;
  uint64_t days = seconds / 86400U;
  seconds %= 86400U;
  uint32_t hours = (uint32_t)(seconds / 3600U);
  seconds %= 3600U;
  uint32_t minutes = (uint32_t)(seconds / 60U);
  seconds %= 60U;

  console_puts("OrgOS uptime: ");
  if (days) {
    console_putu64(days);
    console_puts(days == 1 ? " day, " : " days, ");
  }
  console_putu32(hours);
  console_puts("h ");
  console_putu32(minutes);
  console_puts("m ");
  console_putu32((uint32_t)seconds);
  console_puts("s\n");
}

static void cmd_irq(const char *args) {
  (void)args;

  console_puts("Interrupts handled: ");
  console_putu64(irq_count());
  console_putc('\n');

  console_puts("UART RX overruns : ");
  console_putu32(uart_rx_overruns());
  console_putc('\n');
}

static void cmd_help(const char *args);

typedef struct {
  const char *name;
  console_command_fn handler;
  const char *description;
} console_command_t;

// Commands will be added here as they are implemented.
// The NULL entry terminates the table.
static const console_command_t commands[] = {
  { "timer",  cmd_timer,  "Display system timer information" },
  { "uptime", cmd_uptime, "Show time since OrgOS started" },
  { "help",   cmd_help,   "List available commands" },
  { "irq", cmd_irq, "Display interrupt statistics" },
  { NULL, NULL, NULL }
};

static void cmd_help(const char *args) {
  (void)args;

  console_puts("Available commands:\n\n");

  for (size_t i = 0; commands[i].name != NULL; i++) {
    console_puts("  ");
    console_puts(commands[i].name);

    // Align descriptions at column 12
    size_t length = 0;
    const char *p = commands[i].name;

    while (*p++) {
      length++;
    }

    do {
      console_putc(' ');
      length++;
    } while (length < 10);

    console_puts(commands[i].description);
    console_putc('\n');
  }
}

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
  console_puts("OrgOS> ");
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

  console_puts("No such command.\n");
}

void console_run(void) {
  char line[CONSOLE_LINE_SIZE];
  size_t length = 0;
  bool last_was_cr = false;

  console_puts("Console ready.\n");
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
      console_puts("\n");

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
        console_puts("\b \b");
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
      console_putc('\a');
      continue;
    }

    // Append and echo the character.
    line[length++] = (char)c;
    console_putc((char)c);
  }
}

void console_putc(char c) {
  if (c == '\n')
    uart_putc('\r');

  uart_putc(c);
}

void console_puts(const char *s) {
  while (*s)
    console_putc(*s++);
}

void console_putu32(uint32_t value) {
  char buffer[10];
  unsigned int length = 0;

  do {
    buffer[length++] = '0' + (value % 10U);
    value /= 10U;
  } while (value);

  while (length)
    uart_putc(buffer[--length]);
}

void console_putu64(uint64_t value) {
  char buffer[20];
  unsigned int length = 0;

  do {
    buffer[length++] = '0' + (value % 10U);
    value /= 10U;
  } while (value);

  while (length)
    uart_putc(buffer[--length]);
}

