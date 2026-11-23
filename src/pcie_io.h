#pragma once

#include <stdint.h>
#include "mmio.h"

// Private hardware boundary; configuration Command/MSI fields need halfwords.
static inline uint32_t pcie_read32(uintptr_t address) {
  return mmio_read(address);
}

static inline void pcie_write32(uintptr_t address, uint32_t value) {
  mmio_write(address, value);
}

static inline uint16_t pcie_read16(uintptr_t address) {
  return *(volatile uint16_t *)address;
}

static inline void pcie_write16(uintptr_t address, uint16_t value) {
  *(volatile uint16_t *)address = value;
}

static inline void pcie_barrier(void) {
  __asm__ volatile("dsb sy" ::: "memory");
}

static inline void pcie_instruction_sync(void) {
  __asm__ volatile("isb" ::: "memory");
}

