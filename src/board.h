#pragma once

#include <stdint.h>

// Decode installed RAM from a modern Raspberry Pi
// board revision code. Zero means unknown.
static inline uint32_t board_ram_mib(uint32_t revision) {
  if (!(revision & (1U << 23)))
    return 0U;

  uint32_t code = (revision >> 20) & 7U;

  if (code == 7U)
    return 0U;

  return 256U << code;
}
