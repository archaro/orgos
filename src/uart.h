#pragma once

#include <stdint.h>
#include <stdbool.h>

void uart_init(uint32_t clock_hz, uint32_t baud);
void uart_putc(char c);
bool uart_has_data(void);
char uart_getc(void);
void uart_enable_rx_irq(void);
void uart_handle_irq(void);
uint32_t uart_rx_overruns(void);
