#pragma once

#include "mmu_tables.h"
#include "physmem.h"

#define MMU_BOOT_RAM_REGIONS 8U
#define MMU_BOOT_TABLE_COUNT 11U

typedef struct { vaddr_t start, end; } mmu_boot_range_t;
typedef struct {
  mmu_boot_range_t kernel, text, el0_text, rodata, data, bss, stack;
  mmu_boot_range_t el0_stack[2];
} mmu_boot_layout_t;

// The current identity-map POLICY, using caller-owned inactive storage.
// Registration order: root, eight RAM L2s, PCIe L2, low-RAM L3. The pure
// mechanism has no such ordering or identity-map requirement. No activation.
bool mmu_boot_build(mmu_address_space_t *space,
    const mmu_table_ref_t tables[MMU_BOOT_TABLE_COUNT],
    const physmem_map_t *memory, const mmu_boot_layout_t *layout,
    uint64_t *mapped_ram_bytes);
