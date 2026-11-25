#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "physmem.h"
#include "mmu_tables.h"

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
bool mmu_translate_write(uint64_t address, mmu_translation_t *result);

bool mmu_inspect(uint64_t address, mmu_descriptor_info_t *info);

bool mmu_init(const physmem_map_t *memory);

// Three static identity-mapped environments. Returned objects are borrowed,
// sealed/active and immutable except for MMU-managed activation state.
const mmu_address_space_t *mmu_kernel_address_space(void);
const mmu_address_space_t *mmu_context_address_space(unsigned int id);
// Only an admitted SEALED space can be selected; invalid requests do not
// change hardware or active identity. Foreground/owned sync-return only.
bool mmu_activate_address_space(const mmu_address_space_t *space);
bool mmu_restore_kernel_space(void);
// Actual TTBR0_EL1, including any unexpected ASID/CnP bits for diagnosis.
paddr_t mmu_current_root(void);
