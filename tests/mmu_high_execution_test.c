#include "mmu_high_fixture.h"

static mmu_high_test_result_t result;

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

static void persistent(void) {
  reset();
  assert(mmu_high_enable(&control));
  assert(control.persistent && spaces[1].state == MMU_SPACE_ACTIVE);
  assert(hw.tcr == MMU_TCR_DUAL_4K && hw.ttbr1 == control.root);
  assert(hw.ttbr0 == control.kernel_root && daif == 0x340 && !locked);
  assert(operation_count == 8); // Exact enable half of the H2.6e sequence.
  const mmu_high_operation_t enable[] = {
    MMU_HIGH_DSB, MMU_HIGH_WRITE_TTBR1, MMU_HIGH_ISB, MMU_HIGH_WRITE_TCR,
    MMU_HIGH_ISB, MMU_HIGH_TLBI, MMU_HIGH_DSB, MMU_HIGH_ISB
  };
  for (unsigned int i = 0; i < 8; i++)
    assert(operations[i].op == enable[i]);
  assert(operations[1].value == control.root);
  assert(operations[3].value == MMU_TCR_DUAL_4K);
  assert(mmu_high_persistent_matches(&control, &hw));

  // The live upper object cannot be edited or reinitialised by any
  // inactive construction API. Compare all pages, including empty slots.
  static uint64_t saved[13][512];
  memcpy(saved, pages[1], sizeof(saved));
  mmu_mapping_attrs_t attrs = {2, 0, 3, true, true, true};
  assert(!mmu_space_map(&spaces[1], MMU_TTBR1_BASE + 0x200000000ULL,
                        0x200000000ULL, 4096, MMU_DESC_PAGE, attrs));
  assert(!mmu_space_link_table(&spaces[1], MMU_TTBR1_BASE, 1,
                               tables[1][1].physical));
  assert(!mmu_space_seal(&spaces[1]));
  assert(!mmu_high_build(&spaces[1], tables[1], &memory, &layout));
  assert(!mmu_space_init(&spaces[1], MMU_GEOMETRY_TTBR1_4K_39BIT,
                          tables[1], 11, control.root));
  assert(memcmp(saved, pages[1], sizeof(saved)) == 0);

  assert(mmu_activation_init(&lower, &spaces[0], &spaces[2], &spaces[3],
                              &layout));
  assert(!mmu_activation_select(&lower, &spaces[1]));
  operation_count = 0;
  for (unsigned int n = 0; n < 16; n++) {
    daif = (uint64_t)n << 6;
    assert(mmu_high_enable(&control)); // Idempotent, no register writes.
    assert(mmu_high_test_run(&control, &result));
    assert(result.persistent && result.already_active && result.preserved);
    assert(!result.restored && operation_count == 0);
    assert(result.physical_verified && result.high.result == 42);
    assert(result.before.ttbr1 == result.after.ttbr1);
    assert(result.before.tcr == result.after.tcr);
    assert(daif == (uint64_t)n << 6 && !locked);
    for (unsigned int ctx = 2; ctx < 4; ctx++) {
      assert(mmu_activation_select(&lower, &spaces[ctx]));
      assert(hw.ttbr1 == control.root && hw.tcr == MMU_TCR_DUAL_4K);
      mmu_descriptor_info_t stack;
      assert(mmu_space_inspect(&spaces[ctx], EL0_STACK_VA, &stack));
      assert(stack.physical_address == layout.el0_stack[ctx - 2].start);
      assert(stack.access_permissions == 1 && stack.pxn && stack.uxn);
      assert(mmu_high_persistent_matches(&control, &hw));
      assert(mmu_activation_select(&lower, &spaces[0]));
      assert(hw.ttbr0 == control.kernel_root);
      assert(hw.ttbr1 == control.root && hw.tcr == MMU_TCR_DUAL_4K);
    }
  }
  assert(root_writes == 64 && operation_count == 0);
  // An incompatible live upper environment rejects before any root write.
  hw.ttbr1 ^= 4096;
  assert(!mmu_activation_select(&lower, &spaces[2]));
  assert(!mmu_high_test_run(&control, &result));
  assert(!mmu_high_enable(&control));
  assert(root_writes == 64 && operation_count == 0 && !locked);
  hw.ttbr1 ^= 4096;
  hw.tcr ^= 1ULL << 23;
  assert(!mmu_activation_select(&lower, &spaces[2]));
  assert(!mmu_high_test_run(&control, &result));
  hw.tcr ^= 1ULL << 23;
  pages[1][10][0x82] ^= 1ULL << 6;
  assert(!mmu_activation_select(&lower, &spaces[2]));
  assert(!mmu_high_test_run(&control, &result));
  pages[1][10][0x82] ^= 1ULL << 6;
  assert(root_writes == 64 && operation_count == 0 && !locked);
  bad_probe = 1;
  assert(!mmu_high_test_run(&control, &result));
  assert(result.preserved && result.masks_restored && operation_count == 0);
  assert(hw.ttbr1 == control.root && hw.tcr == MMU_TCR_DUAL_4K);

  // An invalid boot precondition has no architectural writes.
  reset(); spaces[1].state = MMU_SPACE_BUILDING;
  assert(!mmu_high_enable(&control) && operation_count == 0 && !locked);
  reset(); bad_high_read = true;
  fatal_expected = true;
  if (!setjmp(fatal_return)) {
    (void)mmu_high_enable(&control);
    assert(!"post-activation proof failure returned");
  }
  assert(locked && daif == 0x3c0 && operation_count == 8);
  for (volatile unsigned int bad = 4; bad <= 5; bad++) {
    reset();
    assert(mmu_high_enable(&control));
    assert(mmu_activation_init(&lower, &spaces[0], &spaces[2], &spaces[3],
                                &layout));
    mutation = bad;
    fatal_expected = true;
    if (!setjmp(fatal_return)) {
      (void)mmu_activation_select(&lower, &spaces[2]);
      assert(!"post-switch upper regime mismatch returned");
    }
    assert(locked && daif == 0x3c0 && root_writes == 1);
    assert(lower.active == &spaces[0]); // Failed switch is never published.
  }
  puts("Persistent TTBR1 activation, probe and TTBR0 interleaving: PASS");
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
  persistent();
  return 0;
}
