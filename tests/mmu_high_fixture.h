#pragma once

// Shared instrumented MMU hardware model; all execution is simulated.
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include "mmu_high_test.h"
#include "mmu_activation.h"

static uint64_t pages[4][13][512] __attribute__((aligned(4096)));
static mmu_table_ref_t tables[4][13];
static mmu_address_space_t spaces[4];
static mmu_high_test_control_t control;
static mmu_activation_t lower;
static unsigned int root_writes;
static mmu_high_hw_state_t hw;
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
    assert(hw.ttbr0 == control.kernel_root);
    assert(control.persistent ? hw.tcr == MMU_TCR_DUAL_4K :
        operation_count == 0 && hw.tcr == MMU_TCR_4K_36BIT);
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

uint64_t mmu_activation_lock(void) { return mmu_high_hw_lock(); }
void mmu_activation_unlock(uint64_t saved) { mmu_high_hw_unlock(saved); }
bool mmu_activation_hw_matches(paddr_t root) {
  return hw.ttbr0 == root && mmu_high_persistent_matches(&control, &hw);
}
void mmu_replace_ttbr0(paddr_t root) {
  assert(locked && daif == 0x3c0);
  hw.ttbr0 = root;
  root_writes++;
  if (mutation == 4)
    hw.ttbr1 ^= 4096;
  if (mutation == 5)
    hw.tcr ^= 1ULL << 23;
}
__attribute__((noreturn)) void mmu_activation_fatal(void) {
  mmu_high_hw_fatal("TTBR0 post-write regime mismatch");
}

static void reset(void) {
  // A fresh fixture models a cold boot after any deliberately fatal test.
  memset(spaces, 0, sizeof(spaces));
  memset(&control, 0, sizeof(control));
  for (unsigned int s = 0; s < 4; s++) {
    for (unsigned int t = 0; t < (s < 2 ? 11U : 13U); t++)
      tables[s][t] = (mmu_table_ref_t){pages[s][t],
        0x100000 + (s * 13 + t) * 4096,
        t == 0 ? 1 : (t == 10 || t == 12) ? 3 : 2};
    if (s == 1)
      assert(mmu_high_build(&spaces[s], tables[s], &memory, &layout));
    else if (s >= 2)
      assert(mmu_context_build(&spaces[s], tables[s], &memory,
                               &layout, s - 2));
    else {
      uint64_t mapped;
      assert(mmu_boot_build(&spaces[s], tables[s], &memory, &layout, &mapped));
    }
  }
  lower = (mmu_activation_t){0};
  root_writes = 0;
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

