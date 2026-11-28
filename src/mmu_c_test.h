#pragma once
#include "mmu_high_test.h"
#include "mmu_c_island.h"

#define MMU_C_GUARD_BEFORE 0x13579bdf2468ace0ULL
#define MMU_C_GUARD_AFTER  0xfedcba9876543210ULL

typedef struct {
  vaddr_t start, end, entry, site[3];
  // Low linked function bounds, indexed outer/middle/leaf.
  mmu_boot_range_t function[3];
  vaddr_t low_return, ro, rw;
} mmu_c_manifest_t;
typedef struct {
  bool validated, executed, passed, preserved, masks_restored;
  bool chain_verified, data_verified, registers_preserved;
  const char *status;
  vaddr_t high_entry, high_ro, high_rw;
  uint64_t original_daif, final_daif;
  mmu_high_hw_state_t before, after;
  mmu_c_evidence_t evidence;
  mmu_c_call_t call;
  paddr_t expected_code_pa[3], code_pa[3], stack_pa[3], local_pa[3];
  paddr_t ro_pa, rw_pa;
} mmu_c_result_t;

// Private controller: trusted linker manifest and static, exclusive storage.
// Caller is foreground core 0. No callbacks or mapping changes.
bool mmu_c_run(mmu_high_test_control_t *owner, const mmu_c_manifest_t *code,
    const volatile uint64_t ro[3], volatile mmu_c_storage_t *rw,
    mmu_c_result_t *result);
bool mmu_c_test(mmu_c_result_t *result);
