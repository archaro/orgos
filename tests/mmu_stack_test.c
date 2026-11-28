// Reuse the production-controller hardware model, adding explicit host
// CPU aliases for our separately registered stack and witness PAs.
#define mmu_high_hw_translate base_translation
#include "mmu_high_fixture.h"
#undef mmu_high_hw_translate
#include "mmu_stack.h"

static uint64_t stack_page[512] __attribute__((aligned(4096)));
static uint64_t low_witness[2048] __attribute__((aligned(4096)));
static mmu_stack_storage_t storage;
static const mmu_stack_code_t stack_code = {
  0x85000, 0x85800, 0x85100, 0x85500};
static mmu_stack_result_t report;
static unsigned int stack_calls, bad_evidence, bad_state;
static bool bad_stack_at;

bool mmu_high_hw_translate(vaddr_t va, bool write, mmu_translation_t *out) {
  const struct { uint64_t *cpu; uint64_t size; paddr_t pa; } aliases[] = {
    {stack_page, 4096, 0x150000}, {low_witness, 16384, 0x154000}
  };
  assert(locked);
  for (unsigned int i = 0; i < 2; i++) {
    uintptr_t cpu = (uintptr_t)aliases[i].cpu;
    if (va >= cpu && va - cpu < aliases[i].size) {
      *out = (mmu_translation_t){.physical_address = aliases[i].pa + va - cpu};
      return true;
    }
  }
  if (bad_stack_at && va >= MMU_TTBR1_BASE + 0x150000 &&
      va < MMU_TTBR1_BASE + 0x151000) {
    *out = (mmu_translation_t){.fault = true, .fault_status = 0xf};
    return true;
  }
  return base_translation(va, write, out);
}

void mmu_stack_call(vaddr_t top, mmu_stack_probe_t *p,
    uint64_t *witness, vaddr_t normal_top) {
  assert(control.busy && locked && daif == 0x3c0);
  assert(top == MMU_TTBR1_BASE + 0x150000 + 4080);
  assert(witness == low_witness && normal_top == layout.stack.end);
  stack_calls++;
  mmu_stack_result_t nested;
  assert(!mmu_stack_run(&control, &storage, &stack_code, &nested));
  assert(stack_calls == 1); // Rejected before another call or mask change.
  *p = (mmu_stack_probe_t){.low_sp = hw.sp, .low_frame = hw.sp - 128,
    .high_top = top, .observed_sp = top - 64, .lowest_sp = top - 96,
    .result = 42, .low_match = 1, .initial_nzcv = 0xa0000000,
    .final_nzcv = 0xa0000000, .daif = 0x3c0, .saved_fp = 0x1b3300,
    .low_return = stack_code.wrapper_return,
    .copy_bytes = normal_top - (hw.sp - 128),
    .copy_pointer = (uintptr_t)witness};
  witness[10] = p->saved_fp;
  witness[12] = p->initial_nzcv;
  witness[13] = p->daif;
  witness[15] = 0x5354;
  // Native code does not switch SP: model only the documented assembly
  // write footprint, to exercise the controller's independent validation.
  const unsigned int outer = (4096 - 16 - 64) / 8, inner = outer - 4;
  stack_page[outer] = p->saved_fp;
  stack_page[outer + 1] = stack_code.wrapper_return;
  stack_page[outer + 2] = stack_page[inner + 2] = 6;
  stack_page[outer + 3] = stack_page[inner + 3] = 7;
  stack_page[outer + 4] = 0xabc;
  stack_page[outer + 5] = 0xdef;
  stack_page[inner] = top - 64;
  stack_page[inner + 1] = stack_code.nested_return;
  switch (bad_evidence) {
    case 1: p->observed_sp = hw.sp; break;
    case 2: p->lowest_sp += 16; break;
    case 3: p->result = 41; break;
    case 4: p->local_failures = 1; break;
    case 5: p->callee_failures = 1; break;
    case 6: stack_page[0] ^= 1; break;
    case 7: stack_page[511] ^= 1; break;
    case 8: stack_page[100] ^= 1; break;
    case 9: stack_page[inner + 2] ^= 1; break;
  }
  switch (bad_state) {
    case 1: p->low_match = 0; break;
    case 2: p->low_sp += 16; break;
    case 3: hw.ttbr1 ^= 4096; break;
    case 4: hw.sp_el0 ^= 16; break;
    case 5: p->low_return += 4; break;
    case 6: p->final_nzcv ^= 1ULL << 29; break;
    case 7: p->daif ^= 0x80; break;
  }
}
const mmu_address_space_t *mmu_context_address_space(unsigned int id) {
  return id < 2 ? &spaces[2 + id] : NULL;
}

static void stack_reset(void) {
  reset();
  assert(mmu_high_enable(&control));
  // Installation has its own production tests; establish its exact pinned
  // postcondition here without pretending to execute vector assembly.
  control.vectors_high = true;
  control.vectors.vectors = 0x80800;
  hw.vbar = control.live_vbar = MMU_TTBR1_BASE + 0x80800;
  storage = (mmu_stack_storage_t){stack_page, low_witness, 0x150000, 0x154000};
  stack_calls = bad_evidence = bad_state = operation_count = 0;
  bad_stack_at = false;
}
static void reject_stack(const mmu_stack_code_t *code) {
  mmu_high_hw_state_t before = hw;
  static uint64_t saved[512];
  memcpy(saved, stack_page, sizeof(saved));
  uint64_t old_daif = daif;
  assert(!mmu_stack_run(&control, &storage, code, &report));
  assert(!report.validated && !report.executed && report.masks_restored);
  assert(stack_calls == 0 && operation_count == 0 && !locked && !control.busy);
  assert(memcmp(saved, stack_page, sizeof(saved)) == 0);
  assert(memcmp(&before, &hw, sizeof(hw)) == 0 && daif == old_daif);
}
int main(void) {
  for (unsigned int n = 0; n < 16; n++) {
    stack_reset();
    daif = (uint64_t)n << 6;
    mmu_high_hw_state_t before = hw;
    assert(mmu_stack_run(&control, &storage, &stack_code, &report));
    assert(report.executed && report.translated && report.canaries);
    assert(report.contents && report.low_intact && report.restored);
    assert(report.registers_preserved && report.masks_restored);
    assert(report.probe.result == 42 && report.observed_physical == 0x150fb0);
    assert(report.lowest_physical == 0x150f90);
    assert(stack_calls == 1 && operation_count == 0);
    assert(!locked && !control.busy);
    assert(memcmp(&before, &hw, sizeof(hw)) == 0 && daif == (uint64_t)n << 6);
  }
  stack_reset();
  storage.physical++; reject_stack(&stack_code); storage.physical--;
  storage.page++; reject_stack(&stack_code); storage.page--;
  storage.physical = layout.stack.start; reject_stack(&stack_code);
  storage.physical = 0x150000;
  storage.witness_physical = storage.physical; reject_stack(&stack_code);
  storage.witness_physical = 0x154000;
  storage.physical = tables[1][0].physical; reject_stack(&stack_code);
  storage.physical = 0x150000;
  hw.ttbr0 ^= 4096; reject_stack(&stack_code); hw.ttbr0 ^= 4096;
  hw.ttbr1 ^= 4096; reject_stack(&stack_code); hw.ttbr1 ^= 4096;
  hw.tcr ^= 1; reject_stack(&stack_code); hw.tcr ^= 1;
  hw.vbar = 0x80800; reject_stack(&stack_code);
  hw.vbar = control.live_vbar;
  hw.sp = MMU_TTBR1_BASE + 0x1b3000; reject_stack(&stack_code);
  hw.sp = 0x1b3000;
  hw.sp++; reject_stack(&stack_code); hw.sp--;
  bad_stack_at = true; reject_stack(&stack_code); bad_stack_at = false;
  const uint64_t edits[] = {4096, 1ULL << 6, 1ULL << 7, 1ULL << 53,
    1ULL << 54, 1ULL << 10, 1ULL << 8, 1ULL << 2};
  for (unsigned int i = 0; i < sizeof(edits) / sizeof(edits[0]); i++) {
    pages[1][10][0x150] ^= edits[i]; reject_stack(&stack_code);
    pages[1][10][0x150] ^= edits[i];
  }
  pages[2][10][0x150] ^= 1ULL << 6; reject_stack(&stack_code);
  pages[2][10][0x150] ^= 1ULL << 6;
  mmu_stack_code_t invalid = stack_code; invalid.entry++;
  reject_stack(&invalid);
  for (unsigned int bad = 1; bad <= 9; bad++) {
    stack_reset(); bad_evidence = bad;
    assert(!mmu_stack_run(&control, &storage, &stack_code, &report));
    assert(report.executed && report.restored && report.low_intact);
    assert(report.masks_restored && !locked && !control.busy);
    assert(operation_count == 0);
  }
  for (volatile unsigned int bad = 1; bad <= 8; bad++) {
    // A fresh fixture models reboot after each intentionally fatal return.
    stack_reset(); bad_state = bad;
    bad_unlock = bad == 8;
    fatal_expected = true;
    if (!setjmp(fatal_return)) {
      (void)mmu_stack_run(&control, &storage, &stack_code, &report);
      assert(!"corrupted stack return resumed foreground");
    }
    assert(locked && daif == 0x3c0 && fatal_reason);
  }
  puts("High-stack controller, evidence and rejection (simulated SP): PASS");
  return 0;
}
