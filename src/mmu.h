#pragma once

#include <stdint.h>
#include <stdbool.h>

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

bool mmu_init(uint32_t arm_base, uint32_t arm_size);
