#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

// Synthetic linker addresses, never dereferenced. The host substitutes
// only the stack clear and hardware/assembly boundaries.
__asm__(".global __el0_text_start\n.set __el0_text_start, 0x100000\n"
        ".global __el0_text_end\n.set __el0_text_end, 0x101000\n"
        ".global __el0_stack_bottom\n.set __el0_stack_bottom, 0x101000\n"
        ".global __el0_stack_top\n.set __el0_stack_top, 0x102000\n");
#define memset test_memset
#include "../src/el0_test.c"
#undef memset

const char exception_test_resume[1] = {0};
const char el0_test_check_resume[1] = {0};
const char el0_test_privileged[1] = {0};
const char el0_test_fault_resume[1] = {0};
const char el0_test_exit_resume[1] = {0};
const char el0_test_memory_begin_resume[1] = {0};
const char el0_test_read_done_resume[1] = {0};
const char el0_test_write_done_resume[1] = {0};
const char el0_test_exec_resume[1] = {0};
const char el0_test_kernel_read[1] = {0};
const char el0_test_kernel_read_resume[1] = {0};
const char el0_test_code_write[1] = {0};
const char el0_test_code_write_resume[1] = {0};
volatile uint64_t el0_test_code_canary = EL0_TEST_CODE_CANARY;

static jmp_buf panic_return;
static uint64_t panic_args[5];
static unsigned int enters, clears, prepares;
static int omit_memory = -1;
static bool change_kernel, change_code, syndrome_valid;
static bool enabled = true;
static int corrupt_register = -1;
static bool omit_fault, reject_traps;
static uint64_t kernel_mismatch;
static uint64_t mismatch_bit;
static uint64_t bad_mapping_address = UINT64_MAX;
static unsigned int bad_mapping_kind;

__attribute__((noreturn))
void exception_panic(uint64_t vector, uint64_t esr, uint64_t elr,
                     uint64_t far, uint64_t spsr) {
  panic_args[0] = vector;
  panic_args[1] = esr;
  panic_args[2] = elr;
  panic_args[3] = far;
  panic_args[4] = spsr;
  longjmp(panic_return, 1);
}

static void expect_panic(exception_frame_t frame) {
  exception_frame_t before = frame;
  if (setjmp(panic_return) == 0) {
    exception_dispatch(&frame);
    assert(!"unowned exception returned");
  }
  assert(memcmp(&frame, &before, sizeof(frame)) == 0);
  assert(panic_args[0] == frame.vector);
  assert(panic_args[1] == frame.esr);
  assert(panic_args[2] == frame.elr);
  assert(panic_args[3] == frame.far);
  assert(panic_args[4] == frame.spsr);
}

void *test_memset(void *destination, int value, size_t length) {
  assert((uintptr_t)destination == 0x101000U && length == 4096U);
  assert(value == 0);
  clears++;
  return destination;
}

void el0_test_prepare_stack(void *stack) {
  assert((uintptr_t)stack == 0x101000U);
  assert(prepares < clears);
  prepares++;
}

bool mmu_enabled(void) { return enabled; }

bool mmu_inspect(uint64_t address, mmu_descriptor_info_t *info) {
  bool code = address == 0x100000U;
  bool stack = address == 0x101000U;
  *info = (mmu_descriptor_info_t){
    .physical_address = address,
    .kind = MMU_DESC_PAGE,
    .access_flag = true,
    .access_permissions = code ? 3U : stack ? 1U : 0U,
    .pxn = true,
    .uxn = !code,
    .attr_index = 2U,
    .leaf_descriptor = 3ULL << 8,
  };
  if (address == bad_mapping_address) {
    switch (bad_mapping_kind) {
      case 0: info->access_permissions ^= 1U; break;
      case 1: info->uxn = !info->uxn; break;
      case 2: info->pxn = false; break;
      case 3: info->physical_address += 4096U; break;
      case 4: info->access_flag = false; break;
      case 5: info->kind = MMU_DESC_BLOCK; break;
      case 6: info->attr_index = 0U; break;
      case 7: info->leaf_descriptor = 0; break;
      default: return false;
    }
  }
  return true;
}

static exception_frame_t user_frame(void) {
  exception_frame_t frame = {
    .sp = 0x101ff0U,
    .sp_el0 = 0x101ff0U,
    .spsr = 0xa0000340U,
    .vector = 8U,
    .far = 0xfeed1234U,
  };
  for (unsigned int i = 0; i < 31U; i++)
    frame.x[i] = i == 0U ? EL0_TEST_RESULT : 0x9abc567812340100ULL + i;
  return frame;
}

static exception_frame_t checkpoint(void) {
  exception_frame_t frame = user_frame();
  frame.esr = (0x15ULL << 26) | (1ULL << 25) | EL0_TEST_CHECK_SVC;
  frame.elr = (uintptr_t)el0_test_check_resume;
  return frame;
}

static void checkpoint_svc(unsigned int immediate, const char *resume) {
  exception_frame_t frame = user_frame();
  frame.esr = (0x15ULL << 26) | (1ULL << 25) | immediate;
  frame.elr = (uintptr_t)resume;
  exception_frame_t before = frame;
  exception_dispatch(&frame);
  assert(memcmp(&before, &frame, sizeof(frame)) == 0);
}

static exception_frame_t memory_frame(unsigned int test) {
  exception_frame_t frame = user_frame();
  bool execute = test == EL0_TEST_STACK_EXEC;
  frame.esr = ((execute ? 0x20ULL : 0x24ULL) << 26) | (1ULL << 25) |
      (test == EL0_TEST_CODE_WRITE ? 1ULL << 6 : 0) | 0xfU;
  frame.far = test == EL0_TEST_KERNEL_READ ?
      (uintptr_t)&el0_test_kernel_canary : test == EL0_TEST_CODE_WRITE ?
      (uintptr_t)&el0_test_code_canary : (uintptr_t)__el0_stack_bottom;
  frame.elr = test == EL0_TEST_KERNEL_READ ?
      (uintptr_t)el0_test_kernel_read : test == EL0_TEST_CODE_WRITE ?
      (uintptr_t)el0_test_code_write : (uintptr_t)__el0_stack_bottom;
  frame.x[10] = frame.far;
  if (execute)
    frame.x[12] = (uintptr_t)el0_test_exec_resume;
  return frame;
}

static void reject_memory_variants(exception_frame_t frame) {
  // Change one classifier field at a time. Neither frame nor harness
  // state may change when the production dispatcher rejects the trap.
  const uint64_t changes[] = {
    1ULL << 26, // Same-EL instead of lower-EL abort.
    1ULL << 25, // Wrong instruction length.
    1ULL << 10, // FAR not valid.
    1ULL << 9,  // External abort.
    1ULL << 8,  // Cache maintenance, not the diagnostic load/store.
    1ULL << 7,  // Fault during stage-1 table walk.
    1ULL << 6,  // Wrong access direction (or reserved bit for IAbort).
    1ULL,      // Permission fault at the wrong translation level.
    1ULL << 3, // Translation fault rather than permission fault.
  };
  for (unsigned int i = 0; i < sizeof(changes) / sizeof(changes[0]); i++) {
    exception_frame_t bad = frame;
    bad.esr ^= changes[i];
    expect_panic(bad);
  }
  exception_frame_t bad = frame;
  bad.far += 8;
  expect_panic(bad);
  bad = frame;
  bad.elr += 4;
  expect_panic(bad);
  bad = frame;
  bad.x[10] += 8;
  expect_panic(bad);
  bad = frame;
  bad.vector = 4;
  expect_panic(bad);
  bad = frame;
  bad.spsr |= 5;
  expect_panic(bad);
  if ((frame.esr >> 26) == 0x20U) {
    bad = frame;
    bad.x[12] += 4;
    expect_panic(bad);
  }
}

static void run_memory_fault(unsigned int test) {
  exception_frame_t frame = memory_frame(test);
  if (reject_traps) {
    reject_memory_variants(frame);
    // Another diagnostic's otherwise valid abort is not owned here.
    expect_panic(memory_frame((test + 1U) % EL0_TEST_MEMORY_FAULT_COUNT));
  }
  if ((int)test == omit_memory)
    return;
  if (syndrome_valid && test != EL0_TEST_STACK_EXEC) {
    // Optional valid instruction syndrome fields do not change DFSC/WnR.
    frame.esr |= (1ULL << 24) | (3ULL << 22) | (11ULL << 16) |
        (1ULL << 15);
  }
  exception_frame_t before = frame;
  before.elr = test == EL0_TEST_KERNEL_READ ?
      (uintptr_t)el0_test_kernel_read_resume : test == EL0_TEST_CODE_WRITE ?
      (uintptr_t)el0_test_code_write_resume : (uintptr_t)el0_test_exec_resume;
  exception_dispatch(&frame);
  assert(memcmp(&before, &frame, sizeof(frame)) == 0);
  assert(report.memory_faults[test].observed);
  assert(report.memory_faults[test].esr == frame.esr);
  assert(report.memory_faults[test].far == frame.far);
  if (reject_traps)
    expect_panic(memory_frame(test)); // Already consumed this fault.
}

uint64_t el0_test_enter(void) {
  enters++;
  // A trusted EL1 continuation, distinct from all EL0 register values.
  exception_frame_t kernel = {
    .sp = 0x180000U,
    .sp_el0 = 0xdeadbeefU,
    .elr = 0x80004U,
    .spsr = 0x60000345U,
    .esr = 0x9876U,
    .far = 0x1234U,
    .vector = 4U,
  };
  for (unsigned int i = 0; i < 31U; i++)
    kernel.x[i] = 0xfedcba9800000000ULL + i;
  el0_test_begin(&kernel);
  el0_test_result_t nested;
  assert(!el0_test_run(&nested));

  exception_frame_t frame = checkpoint();
  if (reject_traps) {
    exception_frame_t bad = frame;
    bad.vector = 4;
    expect_panic(bad);
    bad = frame;
    bad.spsr |= 5U;
    expect_panic(bad);
    bad = frame;
    bad.esr ^= 1U;
    expect_panic(bad);
    bad = frame;
    bad.elr += 4U;
    expect_panic(bad);
    bad = frame;
    bad.esr = 1ULL << 25;
    bad.elr = (uintptr_t)el0_test_privileged;
    expect_panic(bad); // Fault before checkpoint is not owned.
  }
  if (corrupt_register >= 0)
    frame.x[corrupt_register] ^= 1ULL << 48;
  if (mismatch_bit == 31U)
    frame.sp_el0 -= 16U;
  if (mismatch_bit == 32U)
    frame.spsr ^= 1ULL << 29;
  if (mismatch_bit == 33U)
    frame.spsr ^= 1ULL << 7;
  exception_frame_t before = frame;
  exception_dispatch(&frame);
  assert(stage == TEST_CHECKED);
  assert(memcmp(&before, &frame, sizeof(frame)) == 0);
  if (reject_traps)
    expect_panic(frame); // Repeated checkpoint.

  if (!omit_fault) {
    frame = user_frame();
    frame.esr = 1ULL << 25;
    frame.elr = (uintptr_t)el0_test_privileged;
    if (reject_traps) {
      exception_frame_t bad = frame;
      bad.elr += 4U;
      expect_panic(bad);
      bad = frame;
      bad.esr |= 0x18ULL << 26;
      expect_panic(bad);
      bad = frame;
      bad.esr = 0;
      expect_panic(bad);
    }
    before = frame;
    before.elr = (uintptr_t)el0_test_fault_resume;
    exception_dispatch(&frame);
    assert(stage == TEST_FAULTED);
    assert(memcmp(&before, &frame, sizeof(frame)) == 0);
  }

  if (reject_traps) {
    expect_panic(memory_frame(EL0_TEST_KERNEL_READ)); // Not armed yet.
    frame = user_frame();
    frame.esr = (0x15ULL << 26) | (1ULL << 25) | EL0_TEST_EXIT_SVC;
    frame.elr = (uintptr_t)el0_test_exit_resume;
    expect_panic(frame); // No skipping the memory tests.
  }
  checkpoint_svc(EL0_TEST_MEMORY_BEGIN_SVC, el0_test_memory_begin_resume);
  run_memory_fault(EL0_TEST_KERNEL_READ);
  checkpoint_svc(EL0_TEST_READ_DONE_SVC, el0_test_read_done_resume);
  run_memory_fault(EL0_TEST_CODE_WRITE);
  checkpoint_svc(EL0_TEST_WRITE_DONE_SVC, el0_test_write_done_resume);
  run_memory_fault(EL0_TEST_STACK_EXEC);
  if (change_kernel)
    el0_test_kernel_canary ^= 1U;
  if (change_code)
    el0_test_code_canary = 0;

  frame = user_frame();
  frame.esr = (0x15ULL << 26) | (1ULL << 25) | EL0_TEST_EXIT_SVC;
  frame.elr = (uintptr_t)el0_test_exit_resume;
  exception_dispatch(&frame);
  assert(stage == TEST_IDLE && !kernel_context);
  // All kernel GPRs, flags, PC, and original SP_EL0 come from trusted state.
  assert(memcmp(&kernel, &frame, sizeof(frame)) == 0);
  return kernel_mismatch;
}

int main(void) {
  el0_test_result_t result;
  expect_panic(checkpoint()); // No active diagnostic.
  assert(!el0_test_run(NULL));
  enabled = false;
  assert(!el0_test_run(&result) && !result.mappings && enters == 0U);
  enabled = true;
  for (unsigned int i = 0; i < 9U; i++) {
    bad_mapping_kind = i;
    bad_mapping_address = 0x100000U;
    assert(!el0_test_run(&result) && !result.mappings && enters == 0U);
    bad_mapping_address = 0x101000U;
    assert(!el0_test_run(&result) && !result.mappings && enters == 0U);
  }
  bad_mapping_address = 0x80000U;
  bad_mapping_kind = 0; // Accidental EL0 permission on a kernel page.
  assert(!el0_test_run(&result) && !result.mappings && enters == 0U);
  bad_mapping_kind = 1; // Accidental EL0 execution of kernel data.
  assert(!el0_test_run(&result) && !result.mappings && enters == 0U);
  bad_mapping_address = UINT64_MAX;

  reject_traps = true;
  assert(el0_test_run(&result));
  assert(result.mappings && result.completed && result.svc_roundtrip);
  assert(result.privileged_fault && result.result == 42U && result.stack_read);
  assert(result.kernel_memory_unchanged && result.code_unchanged);
  assert(result.memory_faults[EL0_TEST_KERNEL_READ].observed);
  assert(result.memory_faults[EL0_TEST_CODE_WRITE].observed);
  assert(result.memory_faults[EL0_TEST_STACK_EXEC].observed);
  assert(result.failures == 0U && clears == 1U);
  expect_panic(checkpoint()); // The finished diagnostic is disarmed.
  reject_traps = false;
  assert(el0_test_run(&result)); // Repeat without retained state.

  for (int i = 0; i < 31; i++) {
    corrupt_register = i;
    assert(!el0_test_run(&result));
    assert(result.failures == (1ULL << i));
  }
  corrupt_register = -1;
  for (mismatch_bit = 31U; mismatch_bit <= 33U; mismatch_bit++) {
    assert(!el0_test_run(&result));
    assert(result.failures == (1ULL << mismatch_bit));
  }
  mismatch_bit = 0;
  omit_fault = true;
  assert(!el0_test_run(&result));
  assert(!result.privileged_fault && result.failures == (1ULL << 34));
  omit_fault = false;
  assert(el0_test_run(&result));
  syndrome_valid = true;
  assert(el0_test_run(&result));
  syndrome_valid = false;
  for (omit_memory = 0; omit_memory < EL0_TEST_MEMORY_FAULT_COUNT;
       omit_memory++) {
    assert(!el0_test_run(&result));
    assert(!result.memory_faults[omit_memory].observed);
    assert(result.completed && result.result == 42U);
  }
  omit_memory = -1;
  change_kernel = true;
  assert(!el0_test_run(&result) && !result.kernel_memory_unchanged);
  change_kernel = false;
  el0_test_kernel_canary = EL0_TEST_KERNEL_CANARY;
  change_code = true;
  assert(!el0_test_run(&result) && !result.code_unchanged);
  change_code = false;
  el0_test_code_canary = EL0_TEST_CODE_CANARY;
  assert(el0_test_run(&result));
  // Exact memory faults remain fatal after disarming.
  for (unsigned int i = 0; i < EL0_TEST_MEMORY_FAULT_COUNT; i++)
    expect_panic(memory_frame(i));
  kernel_mismatch = 1ULL << 19;
  assert(!el0_test_run(&result));
  assert(result.kernel_failures == kernel_mismatch);
  puts("EL0 harness tests: PASS");
  return 0;
}
