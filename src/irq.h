#pragma once

#include <stdint.h>

void irq_dispatch(void);
uint64_t irq_count(void);
