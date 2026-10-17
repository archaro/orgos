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


// Fixed capacities for the initial read-only parser.
// A normal Raspberry Pi 4 FDT should fit comfortably.
// Exceeding a limit causes parsing to fail rather
// than silently discarding memory reservations.
#define FDT_MAX_MEMORY_BANKS 16U
#define FDT_MAX_RESERVATIONS 64U

typedef struct {
  uint64_t address;
  uint64_t size;
} fdt_memory_range_t;

typedef enum {
  FDT_RESERVE_HEADER,
  FDT_RESERVE_NODE
} fdt_reservation_kind_t;

typedef struct {
  fdt_memory_range_t range;
  fdt_reservation_kind_t kind;
} fdt_reservation_t;

typedef struct {
  fdt_memory_range_t banks[FDT_MAX_MEMORY_BANKS];
  unsigned int bank_count;

  fdt_reservation_t reservations[FDT_MAX_RESERVATIONS];
  unsigned int reservation_count;

  uint32_t address_cells;
  uint32_t size_cells;
} fdt_memory_map_t;

bool fdt_get_memory_map(fdt_memory_map_t *map);

