#pragma once

#include "mmu_high.h"
#include "mmu.h"
#include "mmu_high_probe.h"

#define MMU_HIGH_T1_FIELDS ((63ULL << 16) | (1ULL << 23) | \
    (0xffULL << 24) | (1ULL << 38))

typedef struct {
  vaddr_t entry, site, end, low_return;
} mmu_high_code_t;

typedef struct {
  uint64_t ttbr0, ttbr1, tcr, mair, sctlr, vbar;
  vaddr_t sp, sp_el0, pc;
  uint64_t current_el, spsel;
} mmu_high_hw_state_t;

typedef struct {
  bool validated, activated, restored, masks_restored, passed;
  bool physical_verified, persistent, already_active, preserved;
  const char *status;
  paddr_t root, expected_physical, observed_physical;
  vaddr_t high_entry, high_site;
  uint64_t original_daif, final_daif;
  mmu_high_hw_state_t before, enabled, after;
  mmu_high_probe_result_t low, high;
} mmu_high_test_result_t;

// One static higher-half owner, shared by boot activation and diagnostics.
// The 44 KiB exact snapshot belongs in BSS, never on the 16 KiB EL1 stack.
// Supply a zero-initialised, exclusive controller disjoint from the table
// pages and metadata. All referenced objects must outlive the controller.
typedef struct {
  bool admitted, busy, persistent;
  uint64_t live_sctlr, live_vbar;
  const mmu_address_space_t *kernel;
  mmu_address_space_t *higher;
  const mmu_table_ref_t *registry;
  paddr_t kernel_root, root;
  mmu_boot_layout_t layout;
  mmu_high_code_t code;
  mmu_table_ref_t refs[MMU_BOOT_TABLE_COUNT];
  uint64_t snapshot[MMU_BOOT_TABLE_COUNT][MMU_TABLE_ENTRIES];
} mmu_high_test_control_t;

// Called immediately after construction. No architectural operations.
bool mmu_high_test_admit(mmu_high_test_control_t *control,
    const mmu_address_space_t *kernel, mmu_address_space_t *higher,
    const mmu_boot_layout_t *layout, mmu_high_code_t code);
bool mmu_high_test_run(mmu_high_test_control_t *control,
                       mmu_high_test_result_t *result);
// Boot-only persistent enablement, including the execution proof. An
// already-active owner is revalidated/probed without register writes.
// No teardown API: tables and registrations have kernel lifetime.
bool mmu_high_enable(mmu_high_test_control_t *control);
// Pure live-regime check used by TTBR0 preflight and post-write checks.
// Accepts a context TTBR0; the lower controller validates that root.
bool mmu_high_persistent_matches(const mmu_high_test_control_t *control,
    const mmu_high_hw_state_t *state);
// Runtime wrapper bound to the one statically owned hierarchy.
bool mmu_high_test(mmu_high_test_result_t *result);

// Private instrumentable hardware boundary for this static upper owner.
typedef enum {
  MMU_HIGH_DSB, MMU_HIGH_ISB, MMU_HIGH_TLBI,
  MMU_HIGH_WRITE_TTBR1, MMU_HIGH_WRITE_TCR
} mmu_high_operation_t;
uint64_t mmu_high_hw_lock(void);
void mmu_high_hw_unlock(uint64_t daif);
uint64_t mmu_high_hw_daif(void);
void mmu_high_hw_read(mmu_high_hw_state_t *state);
void mmu_high_hw_operation(mmu_high_operation_t operation, uint64_t value);
bool mmu_high_hw_translate(vaddr_t address, bool write,
                           mmu_translation_t *translation);
void mmu_high_call(vaddr_t entry, mmu_high_probe_result_t *result);
__attribute__((noreturn)) void mmu_high_hw_fatal(const char *reason);
