#define mmu_high_hw_translate base_translate
#include "mmu_high_fixture.h"
#undef mmu_high_hw_translate
#include "mmu_c_test.h"

static const mmu_c_manifest_t island = {
  .start = 0x86000, .end = 0x86120, .entry = 0x860a0,
  .site = {0x860c0, 0x86058, 0x8600c},
  .function = {{0x860a0, 0x86120}, {0x86040, 0x860a0},
    {0x86000, 0x86040}},
  .low_return = 0x84000, .ro = 0x92000, .rw = 0x9a000
};
static uint64_t inputs[3] = {MMU_C_GUARD_BEFORE, 6, 7};
static mmu_c_storage_t storage;
static mmu_c_result_t result;
static unsigned int c_calls, bad_evidence, bad_state;
static unsigned int corrupt_after_call;
static bool nested;
static vaddr_t wrong_translation;

bool mmu_high_hw_translate(vaddr_t va, bool write, mmu_translation_t *t) {
  if (!base_translate(va, write, t)) return false;
  paddr_t pa = va >= MMU_TTBR1_BASE ? va - MMU_TTBR1_BASE : va;
  // Extend the common fixture's AT model to privileged read-only data.
  if (pa >= layout.rodata.start && pa < layout.rodata.end && write) {
    t->fault = true;
    t->fault_status = 0xf;
  }
  if (c_calls && va == wrong_translation) t->physical_address ^= 4096;
  return true;
}
void mmu_c_call(uint64_t entry, mmu_c_evidence_t *e,
    const volatile uint64_t *ro, volatile mmu_c_storage_t *rw,
    mmu_c_call_t *call) {
  assert(locked && daif == 0x3c0 && control.busy);
  assert(entry == MMU_TTBR1_BASE + island.entry);
  assert((uintptr_t)ro == MMU_TTBR1_BASE + island.ro);
  assert((uintptr_t)rw == MMU_TTBR1_BASE + island.rw);
  c_calls++;
  if (nested) {
    unsigned int nlocks = locks;
    mmu_c_result_t rejected;
    assert(!mmu_c_run(&control, &island, inputs, &storage, &rejected));
    assert(locks == nlocks && !rejected.executed && control.busy);
  }
  *call = (mmu_c_call_t){42, hw.sp - 160, hw.sp - 160,
    island.low_return, 0, 0xa0000000, 0xa0000000};
  for (unsigned int i = 0; i < 3; i++) {
    e->pc[i] = MMU_TTBR1_BASE + island.site[i];
    e->sp[i] = hw.sp - 160 - 48 * (i + 1);
    e->local[i] = e->sp[i] + 32;
    e->local_value[i] = i == 0 ? 6 : i == 1 ? 7 : 42;
  }
  e->caller[0] = MMU_TTBR1_BASE + 0x86100;
  e->caller[1] = MMU_TTBR1_BASE + 0x86088;
  e->ro_address = MMU_TTBR1_BASE + island.ro + 8;
  e->rw_address = MMU_TTBR1_BASE + island.rw + 8;
  e->ro_value[0] = 6; e->ro_value[1] = 7;
  e->written = storage.value = 42;
  switch (bad_evidence) {
    case 1: e->pc[0] -= MMU_TTBR1_BASE; break;
    case 2: e->pc[1] += 4; break;
    case 3: e->pc[2] = e->pc[0]; break;
    case 4: e->sp[1] = layout.stack.end; break;
    case 5: e->sp[2] = e->sp[1] + 16; break;
    case 6: e->local[0] = EL0_STACK_VA; break;
    case 7: e->local[1]++; break;
    case 8: e->caller[0] -= MMU_TTBR1_BASE; break;
    case 9: e->caller[1] = e->pc[0]; break;
    case 10: e->ro_address -= MMU_TTBR1_BASE; break;
    case 11: e->rw_address += 8; break;
    case 12: e->ro_value[1]++; break;
    case 13: e->local_value[0]++; break;
    case 14: e->written++; break;
    case 15: storage.guard_before++; break;
    case 16: storage.guard_after++; break;
    case 17: storage.value++; break;
    case 18: call->result++; break;
    case 19: call->before_sp -= 16; break;
    case 20: call->after_sp -= 16; break;
    case 21: call->failures = 1U << 8; break;
    case 22: call->low_return += 4; break;
    case 23: inputs[0]++; break;
    case 24: call->nzcv_after ^= 1ULL << 31; break;
  }
  const vaddr_t observed[] = {e->pc[1], e->sp[1], e->local[1],
    e->ro_address, e->rw_address};
  if (corrupt_after_call)
    wrong_translation = observed[corrupt_after_call - 1];
  switch (bad_state) {
    case 1: hw.ttbr0 ^= 4096; break;
    case 2: hw.ttbr1 ^= 4096; break;
    case 3: hw.tcr ^= 1; break;
    case 4: hw.mair ^= 1; break;
    case 5: hw.sctlr ^= 1; break;
    case 6: hw.vbar += 2048; break;
    case 7: hw.sp -= 16; break;
    case 8: hw.sp_el0 ^= 16; break;
  }
}
static void setup(void) {
  reset();
  assert(mmu_high_enable(&control));
  control.vectors_high = true;
  control.vectors.vectors = 0x80800;
  hw.vbar = control.live_vbar = MMU_TTBR1_BASE + 0x80800;
  control.stack_phase = MMU_STACK_HIGH;
  hw.sp += MMU_TTBR1_BASE;
  c_calls = operation_count = bad_evidence = bad_state = 0;
  wrong_translation = 0;
  corrupt_after_call = 0;
  nested = false;
  inputs[0] = MMU_C_GUARD_BEFORE;
  storage = (mmu_c_storage_t){1, 2, 3};
}
static void reject(const mmu_c_manifest_t *m) {
  unsigned int before_calls = c_calls;
  mmu_high_hw_state_t before = hw;
  mmu_c_storage_t data = storage;
  uint64_t masks = daif;
  assert(!mmu_c_run(&control, m, inputs, &storage, &result));
  assert(!result.validated && !result.executed && c_calls == before_calls);
  assert(memcmp(&before, &hw, sizeof(hw)) == 0);
  assert(memcmp(&data, &storage, sizeof(data)) == 0);
  assert(!locked && daif == masks && operation_count == 0);
}
int main(void) {
  setup();
  for (unsigned int mask = 0; mask < 16; mask++) {
    daif = (uint64_t)mask << 6;
    nested = true;
    assert(mmu_c_run(&control, &island, inputs, &storage, &result));
    assert(result.executed && result.validated && result.passed);
    assert(result.chain_verified && result.data_verified);
    assert(result.preserved && result.registers_preserved);
    assert(result.masks_restored && result.original_daif == result.final_daif);
    assert(result.high_entry == MMU_TTBR1_BASE + island.entry);
    assert(result.code_pa[0] == island.site[0]);
    assert(result.local_pa[2] == result.evidence.local[2] - MMU_TTBR1_BASE);
    assert(!control.busy && !locked && operation_count == 0);
  }
  setup();
  mmu_c_manifest_t bad = island;
  bad.entry = 0x0080000000000000; reject(&bad);
  bad = island; bad.entry += 4; reject(&bad);
  bad = island; bad.end += 4096; reject(&bad);
  bad = island; bad.site[1] = island.end; reject(&bad);
  bad = island; bad.function[1].end -= 4; reject(&bad);
  bad = island; bad.ro = island.rw; reject(&bad);
  bad = island; bad.rw = island.ro; reject(&bad);
  bad = island; bad.low_return = layout.bss.start; reject(&bad);
  bad = island; bad.ro++; reject(&bad);
  const uint64_t bits[] = {1, 4096, 1ULL << 6, 1ULL << 7,
    1ULL << 53, 1ULL << 54, 1ULL << 10, 1ULL << 8, 1ULL << 2};
  const unsigned int leaf[] = {0x86, 0x92, 0x9a};
  for (unsigned int space = 0; space < 2; space++)
    for (unsigned int p = 0; p < 3; p++)
      for (unsigned int b = 0; b < sizeof(bits) / sizeof(bits[0]); b++) {
        pages[space][10][leaf[p]] ^= bits[b];
        reject(&island);
        pages[space][10][leaf[p]] ^= bits[b];
      }
  uint64_t *regs[] = {&hw.ttbr0, &hw.ttbr1, &hw.tcr, &hw.mair,
    &hw.sctlr, &hw.vbar, &hw.current_el, &hw.spsel};
  for (unsigned int i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
    uint64_t old = *regs[i]; *regs[i] ^= 4;
    reject(&island); *regs[i] = old;
  }
  hw.sp -= MMU_TTBR1_BASE; reject(&island); hw.sp += MMU_TTBR1_BASE;
  hw.sp++; reject(&island); hw.sp--;
  hw.sp = MMU_TTBR1_BASE + layout.stack.start + 512; reject(&island);
  hw.sp = MMU_TTBR1_BASE + 0x1b3000;
  control.stack_phase = MMU_STACK_BOOT_LOW; reject(&island);
  control.stack_phase = MMU_STACK_HIGH;
  control.busy = true; reject(&island); control.busy = false;
  control.vectors_high = false; reject(&island); control.vectors_high = true;
  unavailable = MMU_TTBR1_BASE + island.entry; reject(&island);
  unavailable = UINT64_MAX;
  // Extent starts at leaf, so deny its actual first hardware query too.
  unavailable = MMU_TTBR1_BASE + island.start; reject(&island);
  unavailable = UINT64_MAX;
  for (unsigned int bad = 1; bad <= 24; bad++) {
    setup(); bad_evidence = bad;
    assert(!mmu_c_run(&control, &island, inputs, &storage, &result));
    assert(result.executed && result.preserved && result.masks_restored);
    assert(!locked && !control.busy);
  }
  for (unsigned int bad = 1; bad <= 5; bad++) {
    setup(); corrupt_after_call = bad;
    assert(!mmu_c_run(&control, &island, inputs, &storage, &result));
    assert(result.executed && result.preserved);
    assert(!result.chain_verified || !result.data_verified);
  }
  for (unsigned int bad = 1; bad <= 8; bad++) {
    setup(); bad_state = bad; fatal_expected = true;
    if (!setjmp(fatal_return)) {
      (void)mmu_c_run(&control, &island, inputs, &storage, &result);
      assert(!"architectural corruption must be fatal");
    }
    assert(locked && daif == 0x3c0 && fatal_reason);
  }
  setup(); bad_unlock = true; fatal_expected = true;
  if (!setjmp(fatal_return)) {
    (void)mmu_c_run(&control, &island, inputs, &storage, &result);
    assert(!"mask restoration failure must be fatal");
  }
  puts("C island controller, isolation, evidence "
       "and preservation (simulation): PASS");
  return 0;
}
