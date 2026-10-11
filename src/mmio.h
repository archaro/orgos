#pragma once

#include <stdint.h>

#define MMIO_BASE 0xfe000000UL

static inline void mmio_write(uintptr_t reg, uint32_t value) {
  *(volatile uint32_t *)reg = value;
}

static inline uint32_t mmio_read(uintptr_t reg) {
  return *(volatile uint32_t *)reg;
}

