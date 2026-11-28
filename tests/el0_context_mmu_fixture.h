#pragma once

// Native hardware model for the production lower and upper controllers.
// Payload instructions remain simulated by el0_context_test.c; no host
// result establishes hardware MMU or higher-half instruction execution.
#include "mmu_activation.h"
#include "mmu_high_test.h"
#include "mmu_vectors.h"

static uint64_t mmu_pages[4][13][512] __attribute__((aligned(4096)));
static mmu_table_ref_t mmu_refs[4][13];
static mmu_address_space_t test_spaces[4];
static mmu_activation_t lower_owner;
static mmu_high_test_control_t upper_owner;
static mmu_high_hw_state_t live;
#define selected_root live.ttbr0
static bool activation_failed, mmu_locked;
static uint64_t live_daif = 0x340;
static unsigned int upper_writes, lower_writes, vector_writes;
static const mmu_boot_layout_t mmu_layout = {
  .kernel = {0x80000, 0x1b6000}, .text = {0x80000, 0x100000},
  .el0_text = {0x100000, 0x101000}, .rodata = {0x101000, 0x102000},
  .data = {0x102000, 0x103000}, .bss = {0x103000, 0x1b0000},
  .stack = {0x1b0000, 0x1b4000},
  .el0_stack = {{0x1b4000, 0x1b5000}, {0x1b5000, 0x1b6000}}
};
static const mmu_high_code_t probe_code = {0x82000, 0x8200c, 0x82018, 0x83000};

uint64_t mmu_high_hw_lock(void) {
  assert(!mmu_locked);
  mmu_locked = true;
  uint64_t saved = live_daif;
  live_daif = 0x3c0;
  return saved;
}
void mmu_high_hw_unlock(uint64_t saved) {
  assert(mmu_locked && live_daif == 0x3c0);
  mmu_locked = false;
  live_daif = saved;
}
uint64_t mmu_high_hw_daif(void) { return live_daif; }
void mmu_high_hw_read(mmu_high_hw_state_t *state) { *state = live; }
void mmu_high_hw_operation(mmu_high_operation_t op, uint64_t value) {
  assert(mmu_locked && live_daif == 0x3c0);
  if (op == MMU_HIGH_WRITE_TCR) {
    live.tcr = value;
    upper_writes++;
  } else if (op == MMU_HIGH_WRITE_TTBR1) {
    live.ttbr1 = value;
    upper_writes++;
  }
}
bool mmu_high_hw_translate(vaddr_t va, bool write, mmu_translation_t *out) {
  assert(mmu_locked);
  for (unsigned int t = 0; t < 11; t++) {
    vaddr_t cpu = (uintptr_t)mmu_refs[3][t].entries;
    if (va >= cpu && va - cpu < 4096) {
      *out = (mmu_translation_t){.physical_address =
        mmu_refs[3][t].physical + va - cpu};
      return true;
    }
  }
  bool high = va >= MMU_TTBR1_BASE;
  mmu_descriptor_info_t info;
  if (!mmu_space_inspect(&test_spaces[high ? 3 : 0], va, &info) ||
      info.kind == MMU_DESC_UNMAPPED)
    return false;
  if (high)
    assert(live.ttbr1 == upper_owner.root && live.tcr == MMU_TCR_DUAL_4K);
  bool fault = write && info.read_only;
  *out = (mmu_translation_t){.fault = fault,
    .fault_status = fault ? 0xf : 0,
    .physical_address = info.physical_address};
  return true;
}
void mmu_high_call(vaddr_t entry, mmu_high_probe_result_t *out) {
  assert(mmu_locked);
  bool high = entry >= MMU_TTBR1_BASE;
  assert(entry == probe_code.entry + (high ? MMU_TTBR1_BASE : 0));
  *out = (mmu_high_probe_result_t){.result = 42,
    .observed_pc = probe_code.site + (high ? MMU_TTBR1_BASE : 0),
    .before_sp = live.sp - 128, .after_sp = live.sp - 128,
    .observed_sp = live.sp - 128, .low_return = probe_code.low_return};
}
__attribute__((noreturn)) void mmu_high_hw_fatal(const char *reason) {
  fprintf(stderr, "Unexpected MMU failure: %s\n", reason);
  __builtin_trap();
}
uint64_t mmu_activation_lock(void) { return mmu_high_hw_lock(); }
void mmu_activation_unlock(uint64_t saved) { mmu_high_hw_unlock(saved); }
bool mmu_activation_hw_matches(paddr_t root) {
  return !activation_failed && selected_root == root &&
      mmu_high_persistent_matches(&upper_owner, &live);
}
void mmu_replace_ttbr0(paddr_t root) {
  assert(mmu_locked && live_daif == 0x3c0);
  selected_root = root;
  lower_writes++;
}
__attribute__((noreturn)) void mmu_activation_fatal(void) {
  mmu_high_hw_fatal("Post-switch environment mismatch");
}
const mmu_address_space_t *mmu_kernel_address_space(void) {
  return &test_spaces[0];
}
const mmu_address_space_t *mmu_context_address_space(unsigned int id) {
  return id < 2 ? &test_spaces[id + 1] : NULL;
}
paddr_t mmu_current_root(void) { return selected_root; }
bool mmu_activate_address_space(const mmu_address_space_t *space) {
  assert(space == &test_spaces[1] || space == &test_spaces[2]);
  assert(selected_root == test_spaces[0].root_physical);
  return mmu_activation_select(&lower_owner, (mmu_address_space_t *)space);
}
bool mmu_restore_kernel_space(void) {
  if (lower_owner.active == &test_spaces[0])
    return mmu_activation_hw_matches(test_spaces[0].root_physical);
  return mmu_activation_select(&lower_owner, &test_spaces[0]);
}

void mmu_vectors_hw_operation(mmu_vector_op_t op, uint64_t value) {
  assert(mmu_locked && live_daif == 0x3c0);
  if (op == MMU_VECTOR_WRITE_VBAR) {
    live.vbar = value;
    vector_writes++;
  }
}
uint64_t mmu_vectors_hw_trigger(void) {
  exception_frame_t frame = {.vector = 4, .esr = 0x56000021,
    .spsr = 0xa00003c5, .elr = 0x84000, .sp = live.sp - 432,
    .sp_el0 = VECTORS_TEST_SP_EL0};
  uint64_t saved = live.sp_el0;
  live.sp_el0 = VECTORS_TEST_SP_EL0;
  exception_sync_entry(&frame, MMU_TTBR1_BASE + 0x810a0);
  live.sp_el0 = saved;
  return 0;
}

static void mmu_fixture_init(void) {
  const physmem_map_t memory = {.bank_count = 1, .banks = {{0, 0x8000000}}};
  for (unsigned int s = 0; s < 4; s++) {
    unsigned int count = s == 1 || s == 2 ? 13 : 11;
    for (unsigned int t = 0; t < count; t++)
      mmu_refs[s][t] = (mmu_table_ref_t){mmu_pages[s][t],
        0x110000 + (s * 13 + t) * 4096,
        t == 0 ? 1 : (t == 10 || t == 12) ? 3 : 2};
    if (!s) {
      uint64_t mapped;
      assert(mmu_boot_build(&test_spaces[s], mmu_refs[s], &memory,
                             &mmu_layout, &mapped));
    } else if (s < 3) {
      assert(mmu_context_build(&test_spaces[s], mmu_refs[s], &memory,
                                &mmu_layout, s - 1));
    } else {
      assert(mmu_high_build(&test_spaces[s], mmu_refs[s], &memory,
                             &mmu_layout));
    }
  }
  assert(mmu_high_test_admit(&upper_owner, &test_spaces[0], &test_spaces[3],
                             &mmu_layout, probe_code));
  test_spaces[0].state = MMU_SPACE_ACTIVE;
  live = (mmu_high_hw_state_t){.ttbr0 = test_spaces[0].root_physical,
    .ttbr1 = 0x12345000, .tcr = MMU_TCR_4K_36BIT,
    .mair = MMU_MAIR_DEVICE_NC_WB, .sctlr = 0x81005, .vbar = 0x80800,
    .sp = 0x1b3000, .pc = 0x81500, .current_el = 4, .spsel = 1};
  assert(mmu_high_enable(&upper_owner));
  const mmu_vector_code_t code = {0x80800, 0x81800, 0x810a0, 0x84000,
    {0x85000, 0x85100, 0x85200, 0x85300}, {0x81140, 0x81340}};
  mmu_vectors_result_t vectors;
  assert(mmu_vectors_install(&upper_owner, &code, &vectors));
  assert(mmu_activation_init(&lower_owner, &test_spaces[0], &test_spaces[1],
                              &test_spaces[2], &mmu_layout));
}

static void mmu_fixture_check(void) {
  assert(vector_writes == 1 && upper_owner.vectors_high);
  assert(upper_writes == 2); // No later diagnostic/switch writes TCR/TTBR1.
  assert(lower_writes > 0 && !mmu_locked && live_daif == 0x340);
  assert(selected_root == test_spaces[0].root_physical);
  assert(mmu_high_persistent_matches(&upper_owner, &live));
  mmu_high_test_result_t report;
  assert(mmu_high_test_run(&upper_owner, &report));
  assert(report.persistent && report.preserved && report.high.result == 42);
  assert(upper_writes == 2);
}
