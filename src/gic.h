#pragma once

#include <stdint.h>

#define GIC_IRQ_UART0 153U

void gic_init(void);
void gic_enable_irq(uint32_t id);
uint32_t gic_acknowledge(void);
void gic_end_irq(uint32_t token);
