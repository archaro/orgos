
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "format.h"

static void put_char(format_putc_fn output, void *context, char c) {
  output(c, context);
}

static void put_string(format_putc_fn output, void *context,
    const char *s) {
  if (!s)
    s = "(null)";

  while (*s)
    put_char(output, context, *s++);
}

static void put_number(format_putc_fn output, void *context,
    uint64_t value, unsigned int base, unsigned int width,
    bool zero_pad, bool uppercase, bool negative) {

  char digits[65];
  unsigned int count = 0U;
  const char *alphabet = uppercase ?
      "0123456789ABCDEF" : "0123456789abcdef";

  do {
    digits[count++] = alphabet[value % base];
    value /= base;
  } while (value);

  unsigned int total = count + (negative ? 1U : 0U);
  unsigned int padding = width > total ? width - total : 0U;

  if (negative && zero_pad)
    put_char(output, context, '-');

  while (padding--) {
    put_char(output, context, zero_pad ? '0' : ' ');
  }

  if (negative && !zero_pad)
    put_char(output, context, '-');

  while (count)
    put_char(output, context, digits[--count]);
}

static uint64_t signed_magnitude(int64_t value) {
  if (value >= 0)
    return (uint64_t)value;

  // Avoid negating INT64_MIN in signed arithmetic.
  return (uint64_t)(-(value + 1)) + 1U;
}

void format_vprintf(format_putc_fn output, void *context,
    const char *fmt, va_list args) {

  if (!output || !fmt)
    return;

  while (*fmt) {
    if (*fmt != '%') {
      put_char(output, context, *fmt++);
      continue;
    }

    fmt++;

    if (*fmt == '%') {
      put_char(output, context, '%');
      fmt++;
      continue;
    }

    if (*fmt == '\0') {
      put_char(output, context, '%');
      break;
    }

    bool zero_pad = false;
    unsigned int width = 0U;
    if (*fmt == '0') {
      zero_pad = true;
      fmt++;
    }

    while (*fmt >= '0' && *fmt <= '9') {
      unsigned int digit = (unsigned int)(*fmt++ - '0');
      // Cap padding so invalid formats cannot produce
      // an unbounded amount of output.
      if (width < 256U)
        width = width * 10U + digit;

      if (width > 256U)
        width = 256U;
    }

    unsigned int length = 0U;
    if (*fmt == 'l') {
      length = 1U;
      fmt++;

      if (*fmt == 'l') {
        length = 2U;
        fmt++;
      }
    } else if (*fmt == 'z') {
      length = 3U;
      fmt++;
    }

    char specifier = *fmt;
    if (specifier == '\0')
      break;

    fmt++;

    if (specifier == 's' && length == 0U) {
      put_string(output, context, va_arg(args, const char *));
      continue;
    }

    if (specifier == 'c' && length == 0U) {
      put_char(output, context, (char)va_arg(args, int));
      continue;
    }

    if (specifier == 'p' && length == 0U) {
      uintptr_t value = (uintptr_t)va_arg(args, void *);

      put_string(output, context, "0x");
      put_number(output, context, (uint64_t)value, 16U,
          2U * sizeof(void *), true, false, false);
      continue;
    }

    if (specifier == 'd' || specifier == 'i') {
      int64_t value;

      if (length == 2U)
        value = va_arg(args, long long);
      else if (length == 1U)
        value = va_arg(args, long);
      else if (length == 3U)
        value = va_arg(args, ptrdiff_t);
      else
        value = va_arg(args, int);

      put_number(output, context, signed_magnitude(value),
          10U, width, zero_pad, false, value < 0);
      continue;
    }

    if (specifier == 'u' || specifier == 'x' || specifier == 'X') {
      uint64_t value;

      if (length == 2U)
        value = va_arg(args, unsigned long long);
      else if (length == 1U)
        value = va_arg(args, unsigned long);
      else if (length == 3U)
        value = va_arg(args, size_t);
      else
        value = va_arg(args, unsigned int);

      unsigned int base = specifier == 'u' ? 10U : 16U;
      put_number(output, context, value, base, width,
          zero_pad, specifier == 'X', false);
      continue;
    }

    // Unsupported conversion. Display a visible marker
    // instead of silently discarding the format error.
    put_string(output, context, "%?");
  }
}

