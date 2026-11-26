#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include "mmu_high_test.h"

static uint64_t pages[2][11][512] __attribute__((aligned(4096)));
static mmu_table_ref_t tables[2][11];
static mmu_address_space_t spaces[2];
static mmu_high_test_control_t control;
static mmu_high_hw_state_t hw;
static mmu_high_test_result_t result;
static uint64_t daif;
static bool locked, fatal_expected;
static unsigned int calls[2], bad_probe, bad_baseline;
static unsigned int mutation, restore_error;
static bool bad_unlock, bad_high_read, bad_high_write;
static vaddr_t unavailable = UINT64_MAX;
static jmp_buf fatal_return;
static const char *fatal_reason;
static struct { mmu_high_operation_t op; uint64_t value; } operations[32];
static unsigned int operation_count, locks, unlocks;

static const physmem_map_t memory = {
  .bank_count = 1, .banks = {{0, 0x8000000}}
};
static const mmu_boot_layout_t layout = {
  .kernel = {0x80000, 0x1b6000}, .text = {0x80000, 0x90000},
  .el0_text = {0x90000, 0x91000}, .rodata = {0x91000, 0x98000},
  .data = {0x98000, 0x99000}, .bss = {0x99000, 0x1b0000},
  .stack = {0x1b0000, 0x1b4000},
  .el0_stack = {{0x1b4000, 0x1b5000}, {0x1b5000, 0x1b6000}}
};
static const mmu_high_code_t code = {0x82000, 0x8200c, 0x82018, 0x83000};

uint64_t mmu_high_hw_lock(void) {
  assert(!locked);
  locks++;
  locked = true;
  uint64_t original = daif;
  daif = 0x3c0;
  return original;
}
void mmu_high_hw_unlock(uint64_t saved) {
  assert(locked && daif == 0x3c0);
  unlocks++;
  locked = false;
  daif = bad_unlock ? saved ^ 0x80 : saved;
}
uint64_t mmu_high_hw_daif(void) { return daif; }
void mmu_high_hw_read(mmu_high_hw_state_t *state) {
  assert(locked && daif == 0x3c0);
  *state = hw;
}
void mmu_high_hw_operation(mmu_high_operation_t op, uint64_t value) {
  assert(locked && daif == 0x3c0 && operation_count < 32);
  operations[operation_count].op = op;
  operations[operation_count++].value = value;
  if (op == MMU_HIGH_WRITE_TTBR1) {
    if (!(restore_error == 2 && value == 0x12345000))
      hw.ttbr1 = value;
  }
  if (op == MMU_HIGH_WRITE_TCR) {
    if (!(restore_error == 1 && value == MMU_TCR_4K_36BIT))
      hw.tcr = value;
    if (mutation == 1 && value == MMU_TCR_DUAL_4K)
      hw.ttbr0 ^= 4096;
  }
}

bool mmu_high_hw_translate(vaddr_t va, bool write, mmu_translation_t *out) {
  assert(locked && daif == 0x3c0);
  if (va == unavailable)
    return false;
  // Test-owned CPU pointers need not be their simulated PAs. This models
  // the registered table aliases, without reading any proposed high VA.
  for (unsigned int t = 0; t < 11; t++) {
    uintptr_t cpu = (uintptr_t)tables[1][t].entries;
    if (va >= cpu && va - cpu < 4096) {
      *out = (mmu_translation_t){.physical_address =
        tables[1][t].physical + va - cpu};
      return true;
    }
  }
  bool high = va >= MMU_TTBR1_BASE;
  paddr_t pa = high ? va - MMU_TTBR1_BASE : va;
  if (high) {
    assert(hw.ttbr1 == control.root && hw.tcr == MMU_TCR_DUAL_4K);
  }
  bool ro = pa >= layout.text.start && pa < layout.text.end;
  *out = (mmu_translation_t){.physical_address = pa,
    .fault = ro && write, .fault_status = ro && write ? 0xf : 0};
  if (high && bad_high_read && !write)
    out->physical_address += 4096;
  if (high && bad_high_write && write)
    out->fault = false;
  return true;
}
void mmu_high_call(vaddr_t entry, mmu_high_probe_result_t *out) {
  assert(locked && daif == 0x3c0);
  bool high = entry >= MMU_TTBR1_BASE;
  assert(entry == code.entry + (high ? MMU_TTBR1_BASE : 0));
  if (high) {
    assert(hw.tcr == MMU_TCR_DUAL_4K && hw.ttbr1 == control.root);
    assert(control.higher->state == MMU_SPACE_ACTIVE);
  } else {
    assert(operation_count == 0 && hw.tcr == MMU_TCR_4K_36BIT);
  }
  calls[high]++;
  *out = (mmu_high_probe_result_t){.result = 42,
    .observed_pc = code.site + (high ? MMU_TTBR1_BASE : 0),
    .observed_sp = hw.sp - 128, .before_sp = hw.sp - 128,
    .after_sp = hw.sp - 128, .low_return = code.low_return};
  unsigned int bad = high ? bad_probe : bad_baseline;
  switch (bad) {
    case 1: out->result = 41; break;
    case 2: out->observed_pc = code.site; break;
    case 3: out->observed_pc += 4; break;
    case 4: out->failures = 1U << 7; break;
    case 5: out->after_sp -= 16; break;
    case 6: out->low_return += 4; break;
    case 7: out->observed_sp = EL0_STACK_VA; break;
    case 8: bad_high_read = true; break;
    case 9: unavailable = MMU_TTBR1_BASE + code.site; break;
  }
  if (high && mutation == 2)
    hw.vbar += 2048;
  if (!high && mutation == 3)
    hw.sp += 16;
}
__attribute__((noreturn)) void mmu_high_hw_fatal(const char *reason) {
  assert(fatal_expected);
  fatal_reason = reason;
  longjmp(fatal_return, 1);
}

static void reset(void) {
  // A fresh fixture models a cold boot after any deliberately fatal test.
  memset(spaces, 0, sizeof(spaces));
  memset(&control, 0, sizeof(control));
  for (unsigned int s = 0; s < 2; s++) {
    for (unsigned int t = 0; t < 11; t++)
      tables[s][t] = (mmu_table_ref_t){pages[s][t],
        0x100000 + (s * 11 + t) * 4096, t == 0 ? 1 : t == 10 ? 3 : 2};
    if (s)
      assert(mmu_high_build(&spaces[s], tables[s], &memory, &layout));
    else {
      uint64_t mapped;
      assert(mmu_boot_build(&spaces[s], tables[s], &memory, &layout, &mapped));
    }
  }
  operation_count = locks = unlocks = calls[0] = calls[1] = 0;
  bad_probe = bad_baseline = mutation = restore_error = 0;
  bad_unlock = bad_high_read = bad_high_write = false;
  unavailable = UINT64_MAX;
  locked = fatal_expected = false;
  fatal_reason = NULL;
  daif = 0x340;
  assert(mmu_high_test_admit(&control, &spaces[0], &spaces[1], &layout, code));
  assert(operation_count == 0 && locks == 0 && calls[0] == 0);
  spaces[0].state = MMU_SPACE_ACTIVE;
  hw = (mmu_high_hw_state_t){.ttbr0 = spaces[0].root_physical,
    .ttbr1 = 0x12345000, .tcr = MMU_TCR_4K_36BIT,
    .mair = MMU_MAIR_DEVICE_NC_WB, .sctlr = 0x81005, .vbar = 0x80800,
    .sp = 0x1b3000, .sp_el0 = 0x800001000, .pc = 0x81500,
    .current_el = 4, .spsel = 1};
}

static void rejected(void) {
  mmu_high_hw_state_t before = hw;
  uint64_t original_daif = daif;
  assert(!mmu_high_test_run(&control, &result));
  assert(!result.validated && !result.activated && !result.passed);
  assert(result.masks_restored && !control.busy);
  assert(operation_count == 0 && calls[0] == 0 && calls[1] == 0);
  assert(memcmp(&hw, &before, sizeof(hw)) == 0);
  assert(daif == original_daif && !locked);
}

static void check_order(void) {
  const mmu_high_operation_t expected[] = {
    MMU_HIGH_DSB, MMU_HIGH_WRITE_TTBR1, MMU_HIGH_ISB, MMU_HIGH_WRITE_TCR,
    MMU_HIGH_ISB, MMU_HIGH_TLBI, MMU_HIGH_DSB, MMU_HIGH_ISB,
    MMU_HIGH_DSB, MMU_HIGH_WRITE_TCR, MMU_HIGH_ISB, MMU_HIGH_TLBI,
    MMU_HIGH_DSB, MMU_HIGH_ISB, MMU_HIGH_WRITE_TTBR1, MMU_HIGH_ISB
  };
  assert(operation_count == sizeof(expected) / sizeof(expected[0]));
  for (unsigned int i = 0; i < operation_count; i++)
    assert(operations[i].op == expected[i]);
  assert(operations[1].value == control.root);
  assert(operations[3].value == MMU_TCR_DUAL_4K);
  assert(((operations[3].value ^ result.before.tcr) &
          ~MMU_HIGH_T1_FIELDS) == 0);
  assert(operations[9].value == result.before.tcr);
  assert(operations[14].value == result.before.ttbr1);
}

static void expect_fatal(void) {
  fatal_expected = true;
  if (setjmp(fatal_return) == 0) {
    (void)mmu_high_test_run(&control, &result);
    assert(!"invalid architecture state returned to foreground");
  }
  assert(fatal_reason && locked && daif == 0x3c0);
}

int main(void) {
  reset();
  for (unsigned int n = 0; n < 16; n++) {
    operation_count = calls[0] = calls[1] = 0;
    daif = (uint64_t)n << 6;
    mmu_high_hw_state_t before = hw;
    assert(mmu_high_test_run(&control, &result));
    check_order();
    assert(result.validated && result.activated && result.restored);
    assert(result.masks_restored && result.original_daif == result.final_daif);
    assert(result.high.observed_pc == result.high_site);
    assert(result.low.observed_pc == code.site);
    assert(result.physical_verified && result.observed_physical == code.site);
    assert(result.high.result == 42);
    assert(calls[0] == 1 && calls[1] == 1);
    assert(memcmp(&before, &hw, sizeof(hw)) == 0 && !locked);
    assert(spaces[1].state == MMU_SPACE_SEALED);
  }
  reset();
  spaces[1].state = MMU_SPACE_BUILDING; rejected();
  spaces[1].state = MMU_SPACE_SEALED;
  spaces[1].root_physical += 4096; rejected();
  spaces[1].root_physical -= 4096;
  spaces[1].geometry.va_bits = 36; rejected();
  spaces[1].geometry.va_bits = 39;
  spaces[1].mair ^= 1; rejected(); spaces[1].mair ^= 1;
  spaces[1].tcr ^= 1; rejected(); spaces[1].tcr ^= 1;
  spaces[1].tables = NULL; rejected(); spaces[1].tables = tables[1];
  tables[1][3].physical += 4096; rejected(); tables[1][3].physical -= 4096;
  const struct { unsigned int t, i; uint64_t bits; } edits[] = {
    {10, 0x82, 1ULL << 6}, {10, 0x82, 1ULL << 7},
    {10, 0x82, 1ULL << 53}, {10, 0x82, 1ULL << 8},
    {10, 0x82, 1ULL << 54}, {10, 0x82, 4096},
    {4, 480, 1ULL << 2}, {0, 25, 3}, {2, 0, 1ULL << 6}
  };
  for (unsigned int i = 0; i < sizeof(edits) / sizeof(edits[0]); i++) {
    pages[1][edits[i].t][edits[i].i] ^= edits[i].bits;
    rejected();
    pages[1][edits[i].t][edits[i].i] ^= edits[i].bits;
  }
  pages[0][10][0x82] ^= 1ULL << 6; rejected();
  pages[0][10][0x82] ^= 1ULL << 6;
  pages[0][2][0] ^= 1ULL << 6; rejected(); pages[0][2][0] ^= 1ULL << 6;
  hw.ttbr0 ^= 4096; rejected(); hw.ttbr0 ^= 4096;
  hw.tcr ^= 1ULL << 23; rejected(); hw.tcr ^= 1ULL << 23;
  hw.mair ^= 1; rejected(); hw.mair ^= 1;
  hw.sctlr ^= 4; rejected(); hw.sctlr ^= 4;
  hw.vbar = 3; rejected(); hw.vbar = 0x80800;
  hw.sp = 0x70000; rejected(); hw.sp = 0x1b3000;
  hw.current_el = 8; rejected(); hw.current_el = 4;
  hw.spsel = 0; rejected(); hw.spsel = 1;
  unavailable = (uintptr_t)tables[1][0].entries; rejected();
  unavailable = UINT64_MAX;

  reset();
  bad_baseline = 1;
  assert(!mmu_high_test_run(&control, &result));
  assert(result.validated && !result.activated && operation_count == 0);
  assert(calls[0] == 1 && calls[1] == 0 && !locked && daif == 0x340);
  for (unsigned int bad = 1; bad <= 9; bad++) {
    reset(); bad_probe = bad;
    assert(!mmu_high_test_run(&control, &result));
    assert(result.activated && result.restored && result.masks_restored);
    assert(!result.passed && hw.tcr == MMU_TCR_4K_36BIT);
    assert(!locked && daif == 0x340);
    check_order();
  }
  for (unsigned int bad = 0; bad < 2; bad++) {
    reset();
    bad_high_read = bad == 0;
    bad_high_write = bad == 1;
    assert(!mmu_high_test_run(&control, &result));
    assert(result.activated && result.restored && calls[1] == 0);
    assert(!locked && hw.ttbr1 == 0x12345000);
    check_order();
  }
  reset(); mutation = 1; expect_fatal();
  reset(); mutation = 2; expect_fatal();
  reset(); mutation = 3; expect_fatal();
  reset(); restore_error = 1; expect_fatal();
  assert(hw.ttbr1 == control.root); // Never restore opaque root with walks on.
  reset(); restore_error = 2; expect_fatal();
  reset(); bad_unlock = true; expect_fatal();

  reset();
  control.admitted = false;
  spaces[0].state = MMU_SPACE_SEALED;
  spaces[1].state = MMU_SPACE_BUILDING;
  assert(!mmu_high_test_admit(&control, &spaces[0], &spaces[1],
                             &layout, code));
  spaces[1].state = MMU_SPACE_SEALED;
  pages[1][10][0x82] ^= 1ULL << 6;
  assert(!mmu_high_test_admit(&control, &spaces[0], &spaces[1],
                             &layout, code));
  pages[1][10][0x82] ^= 1ULL << 6;
  mmu_high_code_t bad = code; bad.site = bad.end;
  assert(!mmu_high_test_admit(&control, &spaces[0], &spaces[1], &layout, bad));
  assert(!control.admitted && operation_count == 0 && locks == 0);
  puts("Temporary TTBR1 controller and simulated execution: PASS");
  return 0;
}
