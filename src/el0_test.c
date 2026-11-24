#include "el0_test.h"
#include "mmu.h"
#include "string.h"

extern char __el0_text_start[], __el0_text_end[];
extern char __el0_stack_bottom[], __el0_stack_top[];
extern const char el0_test_check_resume[], el0_test_privileged[];
extern const char el0_test_fault_resume[], el0_test_exit_resume[];
extern uint64_t el0_test_enter(void);

enum test_stage { TEST_IDLE, TEST_ENTERED, TEST_CHECKED, TEST_FAULTED };
static enum test_stage stage;
// Points to the live continuation on the EL1 stack, never into EL0 memory.
static exception_frame_t *kernel_context;
static el0_test_result_t report;

// Verify the installed mappings before entering EL0. The two linker-owned
// pages must be identity mapped, cacheable, shareable, and have exact AP/XN
// permissions. Scan the rest of the low region for accidental EL0 access.
static bool mappings_ready(void) {
  if (!mmu_enabled())
    return false;

  uintptr_t code = (uintptr_t)__el0_text_start;
  uintptr_t stack = (uintptr_t)__el0_stack_bottom;
  if (code >= 0x200000U || stack >= 0x200000U ||
      (code & 4095U) || (stack & 4095U) || code == stack ||
      (uintptr_t)__el0_text_end - code != 4096U ||
      (uintptr_t)__el0_stack_top - stack != 4096U)
    return false;

  for (uint64_t address = 0; address < 0x200000U; address += 4096U) {
    mmu_descriptor_info_t info;
    if (!mmu_inspect(address, &info) || info.kind != MMU_DESC_PAGE ||
        !info.access_flag || info.physical_address != address)
      return false;
    if (address == code || address == stack) {
      bool text = address == code;
      if (info.access_permissions != (text ? 3U : 1U) ||
          !info.pxn || info.uxn != !text || info.attr_index != 2U ||
          ((info.leaf_descriptor >> 8) & 3U) != 3U)
        return false;
    } else if ((info.access_permissions & 1U) || !info.uxn) {
      return false;
    }
  }
  return true;
}

// Called only by the assembly launcher after saving the EL1 continuation.
void el0_test_begin(exception_frame_t *context) {
  kernel_context = context;
  stage = TEST_ENTERED;
}

static uint64_t context_failures(const exception_frame_t *frame) {
  uint64_t failures = 0;
  for (unsigned int i = 0; i < 31U; i++) {
    uint64_t expected = i == 0U ? EL0_TEST_RESULT :
        0x9abc567812340100ULL + i;
    if (frame->x[i] != expected)
      failures |= 1ULL << i;
  }
  uint64_t expected_sp = (uintptr_t)__el0_stack_top - 16U;
  if (frame->sp != expected_sp || frame->sp_el0 != expected_sp)
    failures |= 1ULL << 31;
  if ((frame->spsr & 0xf000001fULL) != 0xa0000000ULL)
    failures |= 1ULL << 32;
  if ((frame->spsr & 0x3c0U) != (kernel_context->spsr & 0x3c0U))
    failures |= 1ULL << 33;
  return failures;
}

bool el0_test_handle_exception(exception_frame_t *frame) {
  if (stage == TEST_IDLE || !kernel_context || frame->vector != 8U ||
      (frame->spsr & 0x1fU) != 0U)
    return false;

  uint64_t check_esr = (0x15ULL << 26) | (1ULL << 25) |
      EL0_TEST_CHECK_SVC;
  uint64_t exit_esr = (0x15ULL << 26) | (1ULL << 25) |
      EL0_TEST_EXIT_SVC;

  if (stage == TEST_ENTERED && frame->esr == check_esr &&
      frame->elr == (uintptr_t)el0_test_check_resume) {
    report.failures |= context_failures(frame);
    stage = TEST_CHECKED;
    // Preserve all EL0 registers, flags and stack across this SVC.
    return true;
  }

  if (stage == TEST_CHECKED && frame->esr == (1ULL << 25) &&
      frame->elr == (uintptr_t)el0_test_privileged) {
    report.failures |= context_failures(frame);
    report.privileged_fault = true;
    stage = TEST_FAULTED;
    // An undefined instruction saves the faulting PC, unlike SVC.
    frame->elr = (uintptr_t)el0_test_fault_resume;
    return true;
  }

  if ((stage == TEST_CHECKED || stage == TEST_FAULTED) &&
      frame->esr == exit_esr &&
      frame->elr == (uintptr_t)el0_test_exit_resume) {
    report.failures |= context_failures(frame);
    if (!report.privileged_fault)
      report.failures |= 1ULL << 34;
    report.svc_roundtrip = true;
    report.completed = true;
    report.result = frame->x[0];
    // Restore from trusted EL1 storage. No EL0-supplied return address,
    // mode or GPR value is used to construct the kernel continuation.
    *frame = *kernel_context;
    kernel_context = NULL;
    stage = TEST_IDLE;
    return true;
  }

  return false;
}

bool el0_test_run(el0_test_result_t *result) {
  if (!result || stage != TEST_IDLE)
    return false;

  report = (el0_test_result_t){0};
  report.mappings = mappings_ready();
  if (report.mappings) {
    // This NOLOAD page is reserved with the kernel, not allocated or
    // shared with drivers. Clear it on every serial diagnostic run.
    memset(__el0_stack_bottom, 0, 4096U);
    report.kernel_failures = el0_test_enter();
  }
  *result = report;
  return report.mappings && report.completed && report.svc_roundtrip &&
      report.privileged_fault && report.failures == 0U &&
      report.kernel_failures == 0U &&
      report.result == EL0_TEST_RESULT;
}
