#include "mmu_high_fixture.h"
#include "mmu_vectors.h"
#include "irq.h"
#include "gic.h"
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>

// Only the fixed SVC's low return label is needed by production dispatch.
__asm__(".global exception_test_resume\n"
        ".set exception_test_resume, 0x84000\n");
bool el0_context_handle_exception(exception_frame_t *frame) {
  (void)frame;
  return false;
}
bool el0_test_handle_exception(exception_frame_t *frame) {
  (void)frame;
  return false;
}
__attribute__((noreturn)) void exception_panic(uint64_t vector, uint64_t esr,
    uint64_t elr, uint64_t far, uint64_t spsr) {
  (void)vector; (void)esr; (void)elr; (void)far; (void)spsr;
  mmu_high_hw_fatal("Unexpected armed exception evidence");
}
static mmu_vector_code_t vectors;
static mmu_vectors_result_t report;
static mmu_vector_op_t vector_ops[8];
static uint64_t vector_values[8];
static unsigned int vector_count, triggers, bad_entry, bad_transition;

void mmu_vectors_hw_operation(mmu_vector_op_t op, uint64_t value) {
  assert(locked && daif == 0x3c0 && control.busy && vector_count < 8);
  vector_ops[vector_count] = op;
  vector_values[vector_count++] = value;
  if (op == MMU_VECTOR_WRITE_VBAR) {
    if (!(bad_transition == 2 && value == vectors.vectors))
      hw.vbar = value;
    if (bad_transition == 1)
      hw.ttbr1 ^= 4096;
  }
}
uint64_t mmu_vectors_hw_trigger(void) {
  assert(control.vectors_high ? vector_count == 0 :
      vector_count == 2 && vector_ops[1] == MMU_VECTOR_ISB);
  assert(hw.vbar == MMU_TTBR1_BASE + vectors.vectors);
  triggers++;
  if (bad_entry == 11)
    return 0; // Missing entry observation must not be claimed as success.
  exception_frame_t frame = {
    .vector = 4, .esr = 0x56000021, .spsr = 0xa00003c5,
    .elr = vectors.continuation, .sp = hw.sp - 432,
    .sp_el0 = VECTORS_TEST_SP_EL0, .far = 0x1234
  };
  for (unsigned int i = 0; i < 31; i++)
    frame.x[i] = 0x9abc567812340100ULL + i;
  vaddr_t pc = MMU_TTBR1_BASE + vectors.observed_site;
  switch (bad_entry) {
    case 1: frame.vector = 8; break;
    case 2: frame.esr ^= 1; break;
    case 3: frame.spsr ^= 1; break;
    case 4: frame.elr += 4; break;
    case 5: pc -= MMU_TTBR1_BASE; break;
    case 6: pc += 4; break;
    case 7: frame.sp_el0 ^= 16; break;
    case 8: frame.sp -= 1; break;
    case 9: bad_high_read = true; break;
    case 10: hw.vbar = vectors.vectors; break;
    case 14: frame.esr &= ~(1ULL << 25); break;
    case 15: frame.spsr ^= 1ULL << 29; break;
    case 16: frame.spsr ^= 1ULL << 7; break;
    case 17: frame.sp = EL0_STACK_VA; break;
    case 18: frame.esr = 0x9600000f; break;
    case 19: frame.spsr ^= 1ULL << 5; break;
  }
  exception_frame_t before = frame;
  uint64_t original_sp_el0 = hw.sp_el0;
  hw.sp_el0 = VECTORS_TEST_SP_EL0;
  exception_sync_entry(&frame, pc);
  assert(memcmp(&before, &frame, sizeof(frame)) == 0);
  if (bad_entry == 12)
    exception_sync_entry(&frame, pc); // Only one armed entry is authorised.
  hw.sp_el0 = original_sp_el0;
  if (bad_transition == 3)
    hw.ttbr0 ^= 4096;
  return bad_entry == 13 ? 1ULL << 34 : 0;
}

static void vector_reset(void) {
  reset();
  assert(mmu_high_enable(&control));
  vectors = (mmu_vector_code_t){0x80800, 0x81800, 0x810a0, 0x84000,
    {0x85000, 0x85100, 0x85200, 0x85300}, {0x81140, 0x81340}};
  vector_count = triggers = bad_entry = bad_transition = 0;
  operation_count = 0;
}
static void rejected_vector(void) {
  mmu_high_hw_state_t before = hw;
  unsigned int old_count = vector_count;
  assert(!mmu_vectors_run(&control, &vectors, &report));
  assert(!report.validated && !report.switched && report.masks_restored);
  assert(vector_count == old_count && triggers == 0 && operation_count == 0);
  assert(!locked && !control.busy && memcmp(&before, &hw, sizeof(hw)) == 0);
}

// Fatal cases model disposable cold boots in child processes. They never
// reset/disarm a failed transition and return it to an ordinary foreground.
static void fatal_case(unsigned int entry, unsigned int transition) {
  pid_t child = fork();
  assert(child >= 0);
  if (!child) {
    vector_reset();
    bad_entry = entry;
    bad_transition = transition;
    bad_unlock = transition == 4;
    fatal_expected = true;
    if (!setjmp(fatal_return)) {
      (void)mmu_vectors_run(&control, &vectors, &report);
      _exit(1);
    }
    assert(locked && daif == 0x3c0);
    _exit(0);
  }
  int status;
  assert(waitpid(child, &status, 0) == child);
  assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static uint32_t iar = 1023, eoi, uart_calls, quiesces;
static unsigned int bad_irq;
static exception_frame_t *irq_live_frame;
uint32_t gic_acknowledge(void) { return iar; }
void gic_end_irq(uint32_t token) { eoi = token; }
void uart_handle_irq(void) {
  uart_calls++;
  if (bad_irq == 3) irq_live_frame->x[16] ^= 1;
  if (bad_irq == 4) hw.ttbr1 ^= 4096;
}
uint32_t pcie_msi_host_irq_quiesce(void) { quiesces++; return 1; }
void gic_pcie_msi_test_finish(void) {}
uint64_t timer_now_us(void) { return 1234; }
__attribute__((noreturn)) void irq_unexpected(uint32_t id, uint32_t token) {
  (void)id; (void)token;
  mmu_high_hw_fatal("Unowned IRQ source");
}
const mmu_address_space_t *mmu_context_address_space(unsigned int id) {
  return id < 2 ? &spaces[id + 2] : NULL;
}

static void deliver_irq(unsigned int path, uint32_t source) {
  exception_frame_t frame = {.vector = path ? 9 : 5,
    .spsr = path ? 0xa0000340 : 0x60000345,
    .elr = path ? layout.el0_text.start : 0x84040,
    .sp = path ? EL0_STACK_TOP - 32 : hw.sp - 64,
    .sp_el0 = path ? EL0_STACK_TOP - 32 : hw.sp_el0};
  for (unsigned int i = 0; i < 31; i++) frame.x[i] = 0x987654320000ULL + i;
  if (bad_irq == 2) frame.spsr ^= 1;
  irq_live_frame = &frame;
  exception_frame_t before = frame;
  uint64_t saved_daif = mmu_high_hw_lock(); // Model CPU IRQ entry masking.
  uint64_t saved_sp_el0 = hw.sp_el0;
  hw.sp_el0 = frame.sp_el0;
  iar = source;
  eoi = UINT32_MAX;
  exception_irq_entry(&frame, MMU_TTBR1_BASE + vectors.irq_sites[path] +
      (bad_irq == 1 ? 4 : 0));
  assert(memcmp(&before, &frame, sizeof(frame)) == 0);
  assert(source == 1023 ? eoi == UINT32_MAX : eoi == source);
  hw.sp_el0 = saved_sp_el0;
  mmu_high_hw_unlock(saved_daif); // Simulated return, not hardware ERET.
}

static void permanent_vectors(void) {
  vector_reset();
  assert(mmu_vectors_install(&control, &vectors, &report));
  assert(control.vectors_high && report.permanent && vector_count == 2);
  assert(hw.vbar == MMU_TTBR1_BASE + vectors.vectors);
  assert(mmu_high_persistent_matches(&control, &hw));
  vector_count = 0;
  for (unsigned int n = 0; n < 16; n++) {
    daif = (uint64_t)n << 6;
    assert(mmu_vectors_install(&control, &vectors, &report));
    assert(mmu_vectors_run(&control, &vectors, &report));
    assert(report.permanent && report.already_active && report.observed);
    assert(vector_count == 0 && daif == (uint64_t)n << 6);
  }
  daif = 0x340;
  assert(mmu_activation_init(&lower, &spaces[0], &spaces[2], &spaces[3],
                              &layout));
  for (unsigned int n = 0; n < 8; n++) {
    assert(mmu_activation_select(&lower, &spaces[2 + n % 2]));
    assert(hw.vbar == control.live_vbar && hw.ttbr1 == control.root);
    assert(mmu_activation_select(&lower, &spaces[0]));
    assert(mmu_high_test_run(&control, &(mmu_high_test_result_t){0}));
    // High leaf probe adds no VBAR/TCR writes under the permanent regime.
    assert(vector_count == 0 && operation_count == 0);
  }
  mmu_vector_irq_result_t irq;
  assert(mmu_vectors_irq_read_owner(&control, &irq));
  assert(!irq.paths[0].observed && !irq.paths[1].observed);
  assert(mmu_vectors_irq_arm_owner(&control));
  deliver_irq(0, 1023);
  assert(mmu_vectors_irq_read_owner(&control, &irq));
  assert(!irq.paths[0].observed && !irq.paths[0].verified);
  deliver_irq(0, GIC_IRQ_UART0 | 0x1000U);
  assert(uart_calls == 1);
  assert(mmu_vectors_irq_read_owner(&control, &irq));
  assert(irq.paths[0].verified && irq.paths[0].handler_completed);
  assert(irq.paths[0].physical == vectors.irq_sites[0]);
  assert(!irq.paths[1].observed);
  deliver_irq(0, GIC_IRQ_PCIE_MSI); // Unarmed path still dispatches normally.
  assert(quiesces == 1 && irq_pcie_msi_hardware_count() == 1);
  assert(mmu_activation_select(&lower, &spaces[2]));
  deliver_irq(1, GIC_IRQ_UART0);
  assert(mmu_activation_select(&lower, &spaces[0]));
  assert(mmu_vectors_irq_read_owner(&control, &irq));
  assert(irq.paths[1].verified && irq.paths[1].frame.vector == 9);
  assert(irq.paths[1].state.ttbr0 == spaces[2].root_physical);
  assert(irq.paths[1].physical == vectors.irq_sites[1]);
  unavailable = MMU_TTBR1_BASE + vectors.irq_sites[0];
  assert(mmu_vectors_irq_read_owner(&control, &irq));
  assert(irq.paths[0].observed && !irq.paths[0].verified);
  unavailable = UINT64_MAX;
  bad_high_read = true;
  assert(!mmu_vectors_irq_read_owner(&control, &irq));
  bad_high_read = false;
  hw.vbar = vectors.vectors; // Low VBAR is no longer an admissible regime.
  assert(!mmu_vectors_irq_arm_owner(&control));
  assert(!mmu_vectors_install(&control, &vectors, &report));
  assert(vector_count == 0);
  hw.vbar = MMU_TTBR1_BASE + vectors.vectors;
  puts("Permanent vectors, TTBR0 switching and simulated EL1/EL0 IRQs: PASS");
}

static void irq_fatal_case(unsigned int bad) {
  pid_t child = fork();
  assert(child >= 0);
  if (!child) {
    vector_reset();
    assert(mmu_vectors_install(&control, &vectors, &report));
    assert(mmu_vectors_irq_arm_owner(&control));
    bad_irq = bad;
    fatal_expected = true;
    if (!setjmp(fatal_return)) {
      deliver_irq(0, GIC_IRQ_UART0);
      _exit(1);
    }
    assert(locked && daif == 0x3c0);
    _exit(0);
  }
  int status;
  assert(waitpid(child, &status, 0) == child);
  assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

int main(void) {
  vector_reset();
  for (unsigned int n = 0; n < 16; n++) {
    daif = (uint64_t)n << 6;
    vector_count = triggers = 0;
    mmu_high_hw_state_t before = hw;
    assert(mmu_vectors_run(&control, &vectors, &report));
    assert(report.observed && report.restored && report.persistent);
    assert(report.passed && report.masks_restored && report.failures == 0);
    assert(report.observed_pc == MMU_TTBR1_BASE + vectors.observed_site);
    assert(report.observed_physical == vectors.observed_site);
    assert(report.vector == 4 && report.esr == 0x56000021);
    assert(report.continuation == vectors.continuation);
    assert(vector_count == 4 && triggers == 1 && operation_count == 0);
    assert(vector_ops[0] == MMU_VECTOR_WRITE_VBAR &&
           vector_values[0] == MMU_TTBR1_BASE + vectors.vectors);
    assert(vector_ops[1] == MMU_VECTOR_ISB);
    assert(vector_ops[2] == MMU_VECTOR_WRITE_VBAR &&
           vector_values[2] == before.vbar);
    assert(vector_ops[3] == MMU_VECTOR_ISB);
    assert(!locked && !control.busy && daif == (uint64_t)n << 6);
    assert(memcmp(&before, &hw, sizeof(hw)) == 0);
    assert(mmu_high_persistent_matches(&control, &hw));
  }
  vector_reset();
  vectors.vectors++; rejected_vector(); vectors.vectors--;
  vectors.observed_site = vectors.end; rejected_vector();
  vectors.observed_site = 0x810a0;
  vectors.vectors = MMU_TTBR1_BASE + 0x80800; rejected_vector();
  vectors.vectors = 0x80800;
  vectors.handlers[0] = layout.bss.start; rejected_vector();
  vectors.handlers[0] = 0x85000;
  hw.ttbr1 ^= 4096; rejected_vector(); hw.ttbr1 ^= 4096;
  hw.tcr ^= 1ULL << 23; rejected_vector(); hw.tcr ^= 1ULL << 23;
  hw.vbar += 2048; rejected_vector(); hw.vbar -= 2048;
  hw.sp_el0 = 0x12345678; // Must be preserved, not assumed zero.
  bad_high_read = true; rejected_vector(); bad_high_read = false;
  bad_high_write = true; rejected_vector(); bad_high_write = false;
  unavailable = MMU_TTBR1_BASE + vectors.vectors; rejected_vector();
  unavailable = UINT64_MAX;
  const uint64_t edits[] = {1ULL << 6, 1ULL << 7, 1ULL << 53,
    1ULL << 54, 1ULL << 2, 4096};
  for (unsigned int i = 0; i < sizeof(edits) / sizeof(edits[0]); i++) {
    pages[1][10][0x80] ^= edits[i]; rejected_vector();
    pages[1][10][0x80] ^= edits[i];
  }
  // An unarmed ordinary diagnostic still goes to the original dispatcher,
  // even when the caller supplies a meaningless observation parameter.
  exception_frame_t ordinary = {.vector = 4, .esr = 0x56000021,
    .spsr = 0xa0000345, .elr = vectors.continuation};
  exception_frame_t saved = ordinary;
  exception_sync_entry(&ordinary, 0);
  assert(memcmp(&saved, &ordinary, sizeof(saved)) == 0);
  for (unsigned int bad = 1; bad <= 19; bad++)
    fatal_case(bad, 0);
  for (unsigned int bad = 1; bad <= 4; bad++)
    fatal_case(0, bad);
  permanent_vectors();
  for (unsigned int bad = 1; bad <= 4; bad++) irq_fatal_case(bad);
  puts("Higher-vector transition, observation, preservation "
       "and rejection: PASS");
  return 0;
}
