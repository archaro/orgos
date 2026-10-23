#pragma once

#include <stdarg.h>

typedef void (*format_putc_fn)(char c, void *context);

void format_vprintf(format_putc_fn output, void *context,
    const char *fmt, va_list args);

