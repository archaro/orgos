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

static jmp_buf panic_return;
static uint64_t panic_args[5];
static unsigned int enters, clears;
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
  assert(result.privileged_fault && result.result == 42U);
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
  kernel_mismatch = 1ULL << 19;
  assert(!el0_test_run(&result));
  assert(result.kernel_failures == kernel_mismatch);
  puts("EL0 harness tests: PASS");
  return 0;
}
