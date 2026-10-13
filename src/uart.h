#pragma once

#include <stdint.h>
#include <stdbool.h>

void uart_init(uint32_t clock_hz, uint32_t baud);
void uart_putc(char c);
bool uart_has_data(void);
char uart_getc(void);
