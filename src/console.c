#include <stddef.h>
#include <stdarg.h>
#include "format.h"
#include "console.h"
#include "console_args.h"
#include "console_midi.h"
#include "console_commands.h"
#include "board.h"
#include "mailbox.h"
#include "timer.h"
#include "uart.h"
#include "irq.h"
#include "exceptions.h"
#include "mmu.h"

#define CONSOLE_LINE_SIZE 256

static char console_line[CONSOLE_LINE_SIZE];
static size_t console_length;
static bool console_last_was_cr;
static bool console_line_overflowed;

typedef void (*console_command_fn)(const char *args);

static void console_format_putc(char c, void *context) {
  (void)context;
  console_putc(c);
}

void console_printf(const char *fmt, ...) {
  va_list args;

  va_start(args, fmt);
  format_vprintf(console_format_putc, NULL, fmt, args);
  va_end(args);
}

void console_puthex(uint32_t value, unsigned int digits) {
  static const char hex[] = "0123456789ABCDEF";

  for (unsigned int i = 0; i < digits; i++) {
    unsigned int shift = (digits - 1U - i) * 4U;
    console_putc(hex[(value >> shift) & 0x0fU]);
  }
}

void console_puthex64(uint64_t value) {
  static const char hex[] = "0123456789ABCDEF";

  for (unsigned int i = 0; i < 16; i++) {
    unsigned int shift = (15U - i) * 4U;
    console_putc(hex[(value >> shift) & 0x0fU]);
  }
}

static void cmd_uptime(const char *args) {
  if (!console_arg_end(args)) {
    console_puts("Usage: uptime\n");
    return;
  }

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
  if (!console_arg_end(args)) {
    console_puts("Usage: irq\n");
    return;
  }

  console_puts("Interrupts handled: ");
  console_putu64(irq_count());
  console_putc('\n');

  console_puts("UART RX overruns : ");
  console_putu32(uart_rx_overruns());
  console_putc('\n');
}

// Deliberately narrow EL1 trap test; no EL0 or syscall interface.
static void cmd_svc(const char *args) {
  if (!console_arg_end(args)) {
    console_puts("Usage: svc\n");
    return;
  }

  uint64_t failures = exception_test();
  if (failures == 0U) {
    console_puts("EL1 SVC context test: PASS (x0-x30, SP, NZCV, DAIF)\n");
  } else {
    console_puts("EL1 SVC context test: FAIL, mismatch mask 0x");
    console_puthex64(failures);
    console_putc('\n');
  }
}

static void cmd_help(const char *args);

typedef struct {
  const char *name;
  console_command_fn handler;
  const char *description;
} console_command_t;

static void cmd_memory(const char *args) {
  (void)args;
  uint32_t base;
  uint32_t size;
  uint32_t revision;
  uint32_t clock_hz;

  console_puts("Firmware mailbox:\n");
  if (mailbox_get_board_revision(&revision)) {
    console_puts("  Board revision : 0x");
    console_puthex(revision, 8);
    console_putc('\n');
    if (revision & (1U << 23)) {
      uint32_t model = (revision >> 4) & 0xffU;
      uint32_t processor = (revision >> 12) & 0x0fU;
      uint32_t ram_mib = board_ram_mib(revision);
      console_puts("  Board model    : ");
      if (model == 0x11U)
        console_puts("Raspberry Pi 4 Model B\n");
      else {
        console_puts("Type 0x");
        console_puthex(model, 2);
        console_putc('\n');
      }
      console_puts("  Processor      : ");
      if (processor == 3U)
        console_puts("BCM2711\n");
      else {
        console_puts("Code ");
        console_putu32(processor);
        console_putc('\n');
      }
      console_puts("  Installed RAM  : ");
      if (ram_mib) {
        console_putu32(ram_mib);
        console_puts(" MiB\n");
      } else {
        console_puts("Unknown\n");
      }
    } else {
      console_puts("  Legacy revision encoding\n");
    }
  } else {
    console_puts("  Board revision query failed.\n");
  }
  console_puts("  UART clock     : ");
  if (mailbox_get_clock_rate(MAILBOX_CLOCK_UART, &clock_hz))
    console_printf("%u Hz\n", (unsigned int)clock_hz);
  else
    console_puts("FAILED\n");
  console_puts("\nARM memory allocation:\n");
  if (!mailbox_get_arm_memory(&base, &size)) {
    console_puts("  Firmware memory query failed.\n");
    return;
  }
  uint64_t end = (uint64_t)base + size;
  uint64_t mapped = mmu_ram_mapped_bytes();
  console_puts("  Base address : 0x");
  console_puthex(base, 8);
  console_putc('\n');
  console_puts("  Size         : ");
  console_putu32(size / (1024U * 1024U));
  console_puts(" MiB\n");
  console_puts("  End address  : 0x");
  console_puthex64(end);
  console_puts(" (exclusive)\n");
  console_puts("  MMU-mapped RAM: ");
  console_putu64(mapped / (1024ULL * 1024ULL));
  console_puts(" MiB\n");
  if (mapped < size) {
    console_puts("  Note: A partial final RAM block is not mapped.\n");
  }
}

// Commands will be added here as they are implemented.
// The NULL entry terminates the table.
static const console_command_t commands[] = {
  { "uptime", cmd_uptime, "Show time since OrgOS started" },
  { "help",   cmd_help,   "List available commands" },
  { "memory", cmd_memory, "Inspect firmware mailbox and ARM memory" },
  { "irq", cmd_irq, "Display interrupt statistics" },
  { "svc", cmd_svc, "Test recoverable synchronous exception at EL1" },
  { "fdt", cmd_fdt, "Inspect firmware Device Tree, memory and DMA" },
  { "physmem", cmd_physmem, "Display physical RAM and reservations" },
  { "cache", cmd_cache, "Display CPU cache configuration" },
  { "pci", cmd_pci, "Inspect PCIe controller and DMA configuration" },
  { "page", cmd_page, "Inspect physical page statistics and run tests" },
  { "dma", cmd_dma, "Run DMA ownership and cache tests" },
  { "usb", cmd_usb, "Inspect and test USB controller and MIDI transport" },
  { "midi", console_midi_command, "Decode, receive and monitor MIDI" },
  { NULL, NULL, NULL }
};

static void cmd_help(const char *args) {
  if (!console_arg_end(args)) {
    console_puts("Usage: help\n");
    return;
  }

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
  return c == ' '  || c == '\t' || c == '\n' ||
         c == '\r' || c == '\v' || c == '\f';
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

void console_start(void) {
  console_puts("Console ready.\n");
  console_prompt();
}

bool console_poll(void) {
  // Do not wait for input.
  if (!uart_has_data())
    return false;

  unsigned char c = (unsigned char)uart_getc();
  // Treat CRLF as one newline.
  if (c == '\n' && console_last_was_cr) {
    console_last_was_cr = false;
    return true;
  }

  console_last_was_cr = (c == '\r');
  if (c == '\r' || c == '\n') {
    console_puts("\n");

    if (console_line_overflowed) {
      console_puts("Input line too long; command discarded.\n");
    } else {
      console_line[console_length] = '\0';
      console_dispatch(console_line);
    }

    console_length = 0U;
    console_line_overflowed = false;
    console_prompt();
    return true;
  }

  // Once overflow has occurred, discard everything
  // until the end of the line. In particular, do not
  // allow backspace to make a truncated line executable.
  if (console_line_overflowed)
    return true;

  if (c == '\b' || c == 0x7f) {
    if (console_length > 0U) {
      console_length--;
      console_puts("\b \b");
    }
    return true;
  }

  if (c == '\t')
    c = ' ';

  if (c < 0x20 || c > 0x7e)
    return true;

  if (console_length >= sizeof(console_line) - 1U) {
    console_line_overflowed = true;
    console_putc('\a');
    return true;
  }

  console_line[console_length++] = (char)c;
  console_putc((char)c);

  return true;
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

