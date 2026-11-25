#pragma once

#include "mmu_boot.h"

#define MMU_ENVIRONMENT_COUNT 3U

// Single-core, statically admitted boot + two context environments.
// All pages/registries/objects remain kernel-owned and immutable once
// sealed. Only state changes on activation; this is not an allocator.
typedef struct {
  mmu_address_space_t *spaces[MMU_ENVIRONMENT_COUNT];
  paddr_t roots[MMU_ENVIRONMENT_COUNT];
  const mmu_table_ref_t *registries[MMU_ENVIRONMENT_COUNT];
  mmu_boot_layout_t layout;
  mmu_address_space_t *active;
} mmu_activation_t;

// Kernel is already enabled/ACTIVE; contexts must be SEALED. Rejects
// overlapping ownership. No hardware writes or table edits on admission.
bool mmu_activation_init(mmu_activation_t *activation,
    mmu_address_space_t *kernel, mmu_address_space_t *context0,
    mmu_address_space_t *context1, const mmu_boot_layout_t *layout);
// Only admitted, SEALED destinations. Complete preflight precedes root
// change; rejection leaves hardware, states and active identity unchanged.
// Privileged foreground/owned synchronous-return use only; never IRQ use.
bool mmu_activation_select(mmu_activation_t *activation,
    mmu_address_space_t *space);

// Private hardware boundary. Caller holds saved DAIF across preflight,
// TTBR write/TLBI and metadata updates; unlock restores the original DAIF.
uint64_t mmu_activation_lock(void);
void mmu_activation_unlock(uint64_t daif);
bool mmu_activation_hw_matches(paddr_t root);
void mmu_replace_ttbr0(paddr_t root);
