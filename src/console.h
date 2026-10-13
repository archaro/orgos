#pragma once

#include <stdint.h>

void console_run(void);
void console_putc(char c);
void console_puts(const char *s);
void console_putu32(uint32_t value);
void console_putu64(uint64_t value);
