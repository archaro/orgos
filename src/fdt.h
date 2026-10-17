#pragma once

#include <stdint.h>
#include <stdbool.h>

// Physical address supplied by the firmware.
// Defined in fdt.c and populated by boot.S.
extern uint64_t firmware_fdt_address;

typedef struct {
  uint64_t address;
  uint32_t total_size;
  uint32_t version;
  uint32_t last_compatible_version;
  uint32_t structure_offset;
  uint32_t strings_offset;
  uint32_t reservations_offset;
  uint32_t structure_size;
  uint32_t strings_size;
} fdt_info_t;

bool fdt_init(void);
bool fdt_get_info(fdt_info_t *info);
