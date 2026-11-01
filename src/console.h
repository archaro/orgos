#pragma once

#include <stdint.h>
#include <stdbool.h>

void console_printf(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));
void console_start(void);
bool console_poll(void);
void console_putc(char c);
void console_puts(const char *s);
void console_putu32(uint32_t value);
void console_putu64(uint64_t value);
void console_puthex(uint32_t value, unsigned int digits);

