#define mmu_activation_hw_matches base_matches
#define mmu_high_hw_daif base_daif
#include "mmu_high_fixture.h"
#undef mmu_activation_hw_matches
#undef mmu_high_hw_daif
#include "mmu_kernel_stack.h"
#include <unistd.h>
#include <sys/wait.h>

static jmp_buf jump_return;
static bool bad_daif_read;
uint64_t mmu_high_hw_daif(void) {
  return base_daif() ^ (bad_daif_read ? 0x80U : 0U);
}
static unsigned int jumps, probe_calls, bad_probe_state;
static vaddr_t jumped_top, jumped_entry;
const mmu_address_space_t *mmu_context_address_space(unsigned int id) {
  return id < 2 ? &spaces[2 + id] : NULL;
}
bool mmu_activation_hw_matches(paddr_t root) {
  mmu_translation_t t;
  return base_matches(root) &&
      mmu_high_hw_translate(hw.sp - 16, true, &t) && !t.fault &&
      t.physical_address == mmu_el1_stack_physical(&control, hw.sp - 16);
}
__attribute__((noreturn))
void mmu_kernel_stack_jump(vaddr_t top, void (*entry)(void)) {
  assert(locked && daif == 0x3c0 && control.busy);
  assert(control.stack_phase == MMU_STACK_TRANSITION);
  assert(!mmu_high_persistent_matches(&control, &hw));
  jumped_top = top;
  jumped_entry = (uintptr_t)entry;
  jumps++;
  hw.sp = top - 64; // Model the fresh C continuation's prologue.
  hw.pc = (uintptr_t)entry;
  longjmp(jump_return, 1); // Simulated branch; never calls the target on host.
}
void mmu_kernel_stack_call(mmu_kernel_stack_execution_t *r) {
  assert(locked && daif == 0x3c0 && control.busy);
  probe_calls++;
  *r = (mmu_kernel_stack_execution_t){hw.sp - 64, hw.sp - 96, 42, 0};
  switch (bad_probe_state) {
    case 1: r->outer_sp -= 16; break;
    case 2: r->result = 41; break;
    case 3: r->failures = 1; break;
    case 4: hw.ttbr1 ^= 4096; break;
    case 5: hw.sp_el0 ^= 16; break;
  }
}
static void boot_reset(void) {
  reset();
  assert(mmu_high_enable(&control));
  control.vectors_high = true;
  control.vectors.vectors = 0x80800;
  hw.vbar = control.live_vbar = MMU_TTBR1_BASE + 0x80800;
  operation_count = jumps = probe_calls = bad_probe_state = 0;
  daif = 0x3c0;
}
static void reject_prepare(vaddr_t target) {
  uint64_t saved = mmu_high_hw_lock();
  mmu_kernel_stack_plan_t plan;
  memset(&plan, 0xab, sizeof(plan));
  mmu_kernel_stack_plan_t old = plan;
  mmu_high_hw_state_t before = hw;
  assert(!mmu_kernel_stack_prepare(&control, &hw, target, &plan));
  assert(memcmp(&old, &plan, sizeof(plan)) == 0);
  assert(memcmp(&before, &hw, sizeof(hw)) == 0 && jumps == 0);
  assert(operation_count == 0 && control.stack_phase == MMU_STACK_BOOT_LOW);
  mmu_high_hw_unlock(saved);
}
static void simulate_handoff(void) {
  if (!setjmp(jump_return))
    mmu_kernel_stack_enter_owner(&control, (void (*)(void))0x86000);
  assert(jumps == 1 && jumped_entry == 0x86000);
  assert(jumped_top == MMU_TTBR1_BASE + layout.stack.end);
}
int main(void) {
  boot_reset();
  reject_prepare(0x86001);
  reject_prepare(layout.bss.start);
  control.layout.stack.start += 4096; reject_prepare(0x86000);
  control.layout.stack.start -= 4096;
  hw.ttbr1 ^= 4096; reject_prepare(0x86000); hw.ttbr1 ^= 4096;
  hw.tcr ^= 1; reject_prepare(0x86000); hw.tcr ^= 1;
  hw.vbar = 0x80800; reject_prepare(0x86000); hw.vbar = control.live_vbar;
  hw.sp = MMU_TTBR1_BASE + 0x1b3000; reject_prepare(0x86000);
  hw.sp = 0x1b3000;
  const uint64_t bits[] = {4096, 1ULL << 6, 1ULL << 7, 1ULL << 53,
    1ULL << 54, 1ULL << 10, 1ULL << 8, 1ULL << 2};
  for (unsigned int i = 0; i < sizeof(bits) / sizeof(bits[0]); i++) {
    pages[1][10][0x1b1] ^= bits[i]; reject_prepare(0x86000);
    pages[1][10][0x1b1] ^= bits[i];
  }
  control.higher->geometry.va_bits = 36; reject_prepare(0x86000);
  control.higher->geometry.va_bits = 39;
  pages[2][10][0x1b1] ^= 1ULL << 6; reject_prepare(0x86000);
  pages[2][10][0x1b1] ^= 1ULL << 6;
  simulate_handoff();
  assert(!mmu_el1_stack_contains(&control, hw.sp));
  mmu_kernel_stack_complete_owner(&control);
  assert(control.stack_phase == MMU_STACK_HIGH && !control.busy);
  assert(mmu_high_persistent_matches(&control, &hw));
  assert(!mmu_el1_stack_contains(&control, 0x1b3000));
  assert(!mmu_el1_stack_contains(&control, MMU_TTBR1_BASE + 0x150000));
  assert(!mmu_el1_stack_contains(&control, hw.sp + 1));
  mmu_high_hw_unlock(0x340); // Model later GIC/UART setup and IRQ unmasking.
  mmu_kernel_stack_report_t report;
  for (unsigned int n = 0; n < 16; n++) {
    daif = (uint64_t)n << 6;
    assert(mmu_kernel_stack_report_owner(&control, false, &report));
    assert(report.phase == MMU_STACK_HIGH && !report.executed);
    assert(report.current_physical == hw.sp - MMU_TTBR1_BASE);
    assert(mmu_kernel_stack_report_owner(&control, true, &report));
    assert(report.executed && report.translated && report.preserved);
    assert(report.execution.result == 42 && !report.execution.failures);
    assert(report.original_daif == report.final_daif);
  }
  uint64_t initial_masks = mmu_high_hw_lock();
  assert(mmu_activation_init(&lower, &spaces[0], &spaces[2], &spaces[3],
                              &layout));
  mmu_high_hw_unlock(initial_masks);
  vaddr_t expected_sp = hw.sp;
  for (unsigned int n = 0; n < 16; n++) {
    assert(mmu_activation_select(&lower, &spaces[2 + n % 2]));
    assert(hw.sp == expected_sp && hw.vbar == control.live_vbar);
    assert(mmu_activation_select(&lower, &spaces[0]));
    assert(mmu_high_test_run(&control, &(mmu_high_test_result_t){0}));
  }
  hw.sp = 0x1b3000;
  assert(!mmu_kernel_stack_report_owner(&control, true, &report));
  assert(!mmu_activation_select(&lower, &spaces[2]));
  hw.sp = MMU_TTBR1_BASE + layout.stack.start + 64;
  unsigned int calls = probe_calls;
  assert(!mmu_kernel_stack_report_owner(&control, true, &report));
  assert(probe_calls == calls); // Reject insufficient nested-call headroom.
  hw.sp = expected_sp;
  for (unsigned int bad = 1; bad <= 3; bad++) {
    bad_probe_state = bad;
    assert(!mmu_kernel_stack_report_owner(&control, true, &report));
    assert(report.preserved && !locked && !control.busy);
  }
  for (unsigned int bad = 1; bad <= 5; bad++) {
    // Each fatal transfer is a disposable boot; never reset a failed
    // handoff into foreground operation in the same simulated kernel.
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
      boot_reset();
      simulate_handoff();
      if (bad == 1) hw.sp = 0x1b3000;
      if (bad == 2) hw.ttbr1 ^= 4096;
      if (bad == 3) hw.vbar = 0x80800;
      if (bad == 4) hw.sp++;
      if (bad == 5) bad_daif_read = true;
      fatal_expected = true;
      if (!setjmp(fatal_return)) {
        mmu_kernel_stack_complete_owner(&control);
        _exit(1);
      }
      assert(control.stack_phase == MMU_STACK_TRANSITION);
      _exit(0);
    }
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  }
  puts("Permanent stack handoff/contracts "
       "and nested execution (simulation): PASS");
  return 0;
}
