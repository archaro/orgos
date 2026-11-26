#pragma once

#include "mmu_boot.h"

// Fixed-offset proposal: VA = canonical upper-39-bit base + 36-bit PA.
// Numbers only: this does not establish a usable CPU pointer or mapping.
// Output is unchanged on invalid input; successful conversion may be unmapped.
bool mmu_high_address(paddr_t physical, vaddr_t *virtual_address);
// Uses the same 11 registered roles as the boot policy. All mappings are
// privileged, with no writable alias of text/rodata. Never activates it.
bool mmu_high_build(mmu_address_space_t *space,
    const mmu_table_ref_t tables[MMU_BOOT_TABLE_COUNT],
    const physmem_map_t *memory, const mmu_boot_layout_t *layout);
// Borrowed static hierarchy for software inspection only. Always SEALED;
// absent if construction has not completed. Not admitted for activation.
const mmu_address_space_t *mmu_high_address_space(void);
