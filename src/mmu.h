#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "physmem.h"

typedef struct {
  uint64_t id_aa64mmfr0;
  uint64_t sctlr;
  uint64_t tcr;
  uint64_t mair;
  uint64_t ttbr0;
  uint64_t ttbr1;
} mmu_info_t;

void mmu_get_info(mmu_info_t *info);

bool mmu_enabled(void);
bool mmu_granule_4k_supported(void);

unsigned int mmu_physical_address_bits(void);
uint64_t mmu_ram_mapped_bytes(void);

typedef struct {
  uint64_t virtual_address;
  uint64_t physical_address;
  uint64_t par;
  uint8_t fault_status;
  bool fault;
} mmu_translation_t;
bool mmu_translate(uint64_t address, mmu_translation_t *result);

bool mmu_init(const physmem_map_t *memory);
