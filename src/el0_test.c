#include "el0_test.h"
#include "mmu.h"
#include "string.h"

extern char __el0_text_start[], __el0_text_end[];
extern char __el0_stack_bottom[], __el0_stack_top[];
extern const char el0_test_check_resume[], el0_test_privileged[];
extern const char el0_test_fault_resume[], el0_test_exit_resume[];
extern uint64_t el0_test_enter(void);
extern void el0_test_prepare_stack(void *stack);
extern const char el0_test_memory_begin_resume[], el0_test_read_done_resume[];
extern const char el0_test_write_done_resume[], el0_test_exec_resume[];
extern const char el0_test_kernel_read[], el0_test_kernel_read_resume[];
extern const char el0_test_code_write[], el0_test_code_write_resume[];
// The latter symbol is supplied by assembly in the EL0 executable page.
extern volatile uint64_t el0_test_code_canary;
volatile uint64_t el0_test_kernel_canary = EL0_TEST_KERNEL_CANARY;

enum test_stage {
  TEST_IDLE, TEST_ENTERED, TEST_CHECKED, TEST_FAULTED,
  TEST_READ_ARMED, TEST_READ_FAULTED,
  TEST_WRITE_ARMED, TEST_WRITE_FAULTED,
  TEST_EXEC_ARMED, TEST_EXEC_FAULTED
};
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

static bool svc_at(const exception_frame_t *frame, unsigned int immediate,
                   const char *resume) {
  return frame->esr == ((0x15ULL << 26) | (1ULL << 25) | immediate) &&
      frame->elr == (uintptr_t)resume;
}

// This is a diagnostic whitelist, not a general data-abort recovery path.
// All targets are in existing L3 pages: require permission fault level 3,
// the lower-EL EC, AArch64 IL, WnR, valid FAR, and no EA/CM/S1PTW.
// ISV and its optional instruction-syndrome fields are not required.
static bool memory_fault(exception_frame_t *frame,
    el0_test_memory_fault_t test, const char *site, const char *resume,
    uintptr_t target, bool write, bool execute) {
  uint64_t expected_esr = ((execute ? 0x20ULL : 0x24ULL) << 26) |
      (1ULL << 25) | (write ? 1ULL << 6 : 0) | 0xfU;
  if ((frame->esr & 0xfe0007ffULL) != expected_esr ||
      frame->far != target || frame->elr != (uintptr_t)site ||
      frame->x[10] != target ||
      (execute && frame->x[12] != (uintptr_t)resume))
    return false;

  // The address/continuation scratch registers were checked above.
  uint64_t scratch = (1ULL << 10) | (execute ? 1ULL << 12 : 0);
  report.failures |= context_failures(frame) & ~scratch;
  report.memory_faults[test] = (el0_test_fault_t){
    .observed = true, .esr = frame->esr, .far = frame->far
  };
  frame->elr = (uintptr_t)resume;
  return true;
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
    report.stack_read = frame->x[0] == EL0_TEST_RESULT;
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
      svc_at(frame, EL0_TEST_MEMORY_BEGIN_SVC, el0_test_memory_begin_resume)) {
    report.failures |= context_failures(frame);
    stage = TEST_READ_ARMED;
    return true;
  }

  if (stage == TEST_READ_ARMED && memory_fault(frame, EL0_TEST_KERNEL_READ,
      el0_test_kernel_read, el0_test_kernel_read_resume,
      (uintptr_t)&el0_test_kernel_canary, false, false)) {
    stage = TEST_READ_FAULTED;
    return true;
  }
  if ((stage == TEST_READ_ARMED || stage == TEST_READ_FAULTED) &&
      svc_at(frame, EL0_TEST_READ_DONE_SVC, el0_test_read_done_resume)) {
    report.failures |= context_failures(frame);
    stage = TEST_WRITE_ARMED;
    return true;
  }

  if (stage == TEST_WRITE_ARMED && memory_fault(frame, EL0_TEST_CODE_WRITE,
      el0_test_code_write, el0_test_code_write_resume,
      (uintptr_t)&el0_test_code_canary, true, false)) {
    stage = TEST_WRITE_FAULTED;
    return true;
  }
  if ((stage == TEST_WRITE_ARMED || stage == TEST_WRITE_FAULTED) &&
      svc_at(frame, EL0_TEST_WRITE_DONE_SVC, el0_test_write_done_resume)) {
    report.failures |= context_failures(frame);
    stage = TEST_EXEC_ARMED;
    return true;
  }

  // For an instruction abort, ELR identifies the faulting fetch target,
  // not the BR instruction. Only this prepared stack address is owned.
  if (stage == TEST_EXEC_ARMED && memory_fault(frame, EL0_TEST_STACK_EXEC,
      __el0_stack_bottom, el0_test_exec_resume,
      (uintptr_t)__el0_stack_bottom, false, true)) {
    stage = TEST_EXEC_FAULTED;
    return true;
  }

  if ((stage == TEST_EXEC_ARMED || stage == TEST_EXEC_FAULTED) &&
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
    el0_test_prepare_stack(__el0_stack_bottom);
    report.kernel_failures = el0_test_enter();
    // Only dedicated diagnostic sentinels are checked: IRQ-driven kernel
    // state is expected to change normally while the test is running.
    report.kernel_memory_unchanged =
        el0_test_kernel_canary == EL0_TEST_KERNEL_CANARY;
    report.code_unchanged = el0_test_code_canary == EL0_TEST_CODE_CANARY;
  }
  *result = report;
  return report.mappings && report.completed && report.svc_roundtrip &&
      report.privileged_fault && report.failures == 0U &&
      report.kernel_failures == 0U && report.stack_read &&
      report.kernel_memory_unchanged && report.code_unchanged &&
      report.memory_faults[EL0_TEST_KERNEL_READ].observed &&
      report.memory_faults[EL0_TEST_CODE_WRITE].observed &&
      report.memory_faults[EL0_TEST_STACK_EXEC].observed &&
      report.result == EL0_TEST_RESULT;
}
