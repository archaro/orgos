
#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "fdt.h"

// Additional reservations made by OrgOS itself.
// The original firmware reservation records are
// retained alongside these entries.
typedef enum {
  PHYSMEM_RESERVED_FDT_HEADER,
  PHYSMEM_RESERVED_FDT_NODE,
  PHYSMEM_RESERVED_KERNEL,
  PHYSMEM_RESERVED_FDT_ORIGINAL,
  PHYSMEM_RESERVED_LOW_PAGE
} physmem_reservation_kind_t;

typedef struct {
  uint64_t address;
  uint64_t size;
  physmem_reservation_kind_t kind;
} physmem_reservation_t;

#define PHYSMEM_MAX_RESERVATIONS \
    (FDT_MAX_RESERVATIONS + 3U)

typedef struct {
  fdt_memory_range_t banks[FDT_MAX_MEMORY_BANKS];
  unsigned int bank_count;
  physmem_reservation_t reservations[PHYSMEM_MAX_RESERVATIONS];
  unsigned int reservation_count;
  uint64_t described_ram_bytes;
} physmem_map_t;

bool physmem_init(void);
const physmem_map_t *physmem_get_map(void);

