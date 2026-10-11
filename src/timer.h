#pragma once

#include <stdint.h>

// Microseconds since the hardware counter started
uint64_t timer_now_us(void);

// Busy-wait for the specified duration
void timer_delay_us(uint64_t us);
void timer_delay_ms(uint32_t ms);

