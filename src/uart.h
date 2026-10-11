#pragma once

#include <stdint.h>

void uart_init(uint32_t clock_hz, uint32_t baud);
void uart_putc(char c);
void uart_puts(const char *s);
void uart_putu32(uint32_t value);

