#include "el0_context.h"
#include "el0_test.h"
#include "string.h"

extern char __el0_stack_bottom[], __el0_stack_top[];
extern const char el0_context_program[], el0_context_program_end[];
extern const char el0_context_program_resume[], el0_context_exit_resume[];
extern const char el0_context_fault_program[], el0_context_fault_read[];
extern volatile uint64_t el0_test_kernel_canary;
extern uint64_t el0_context_switch(void);

// The saved EL0 frame survives each foreground return. Only the currently
// active switch may own kernel_return, a pointer into the live EL1 stack.
static exception_frame_t saved;
static exception_frame_t *kernel_return;
static uint64_t expected_daif;
static el0_context_info_t info;

static uint64_t pattern(unsigned int reg) {
  return 0x9abc567812340100ULL + reg;
}

bool el0_context_owns_stack(void) {
  return info.state == EL0_CONTEXT_READY ||
      info.state == EL0_CONTEXT_RUNNING || info.state == EL0_CONTEXT_YIELDED ||
      info.state == EL0_CONTEXT_FAULTED;
}

static bool start_context(bool fault_test) {
  if (el0_context_owns_stack() || el0_test_active() ||
      !el0_test_mappings_ready())
    return false;

  memset(__el0_stack_bottom, 0, 4096U);
  kernel_return = NULL;
  expected_daif = 0;
  saved = (exception_frame_t){0};
  for (unsigned int i = 2; i < 31U; i++)
    saved.x[i] = pattern(i);
  saved.elr = (uintptr_t)(fault_test ? el0_context_fault_program :
                         el0_context_program);
  saved.sp = saved.sp_el0 = (uintptr_t)__el0_stack_top;
  saved.spsr = 0xa0000000ULL; // EL0t. DAIF added on first switch.
  info = (el0_context_info_t){
    .state = EL0_CONTEXT_READY, .stack_ok = true, .fault_test = fault_test
  };
  return true;
}

bool el0_context_start(void) {
  return start_context(false);
}

bool el0_context_start_fault_test(void) {
  return start_context(true);
}

bool el0_context_reset(void) {
  if (info.state == EL0_CONTEXT_RUNNING)
    return false;
  saved = (exception_frame_t){0};
  kernel_return = NULL;
  expected_daif = 0;
  info = (el0_context_info_t){0};
  return true;
}

void el0_context_get_info(el0_context_info_t *result) {
  if (result)
    *result = info;
}

static bool fault_far_valid(uint64_t esr) {
  // Cortex-A72 TRM, ESR_EL1/FAR_EL1 register descriptions:
  // https://documentation-service.arm.com/static/5e7b6c287158f500bd5bfb61
  // FAR is meaningful for aborts, PC alignment and watchpoints.
  // For aborts/watchpoints FnV (bit 10) marks an unavailable fault address.
  unsigned int ec = (unsigned int)((esr >> 26) & 0x3fU);
  if (ec == 0x22U)
    return true;
  return (ec == 0x20U || ec == 0x24U || ec == 0x34U) &&
      (esr & (1ULL << 10)) == 0;
}

bool el0_context_get_fault(el0_context_fault_t *fault) {
  if (!fault)
    return false;
  *fault = (el0_context_fault_t){0};
  if (info.state != EL0_CONTEXT_FAULTED)
    return false;
  fault->frame = saved;
  fault->far_valid = fault_far_valid(saved.esr);
  return true;
}

// Internal assembly callback; valid only inside el0_context_resume().
exception_frame_t *el0_context_enter(exception_frame_t *kernel) {
  if (info.state == EL0_CONTEXT_READY) {
    expected_daif = kernel->spsr & 0x3c0U;
    saved.spsr |= expected_daif;
  }
  kernel_return = kernel;
  info.state = EL0_CONTEXT_RUNNING;
  info.resumptions++;
  return &saved;
}

bool el0_context_handle_exception(exception_frame_t *frame) {
  if (info.state != EL0_CONTEXT_RUNNING || !kernel_return ||
      frame->vector != 8U || (frame->spsr & 0x1fU) != 0U)
    return false;

  bool yield = frame->esr == ((0x15ULL << 26) | (1ULL << 25) |
                              EL0_CONTEXT_YIELD_SVC);
  bool exit = frame->esr == ((0x15ULL << 26) | (1ULL << 25) |
                             EL0_CONTEXT_EXIT_SVC);
  // SError is a system-level failure, never a context termination case,
  // even if an inconsistent record claims the synchronous vector.
  if (((frame->esr >> 26) & 0x3fU) == 0x2fU)
    return false;

  bool service = (yield || exit) && (frame->elr & 3U) == 0 &&
      frame->elr >= (uintptr_t)el0_context_program &&
      frame->elr < (uintptr_t)el0_context_program_end;
  saved = *frame;
  if (service) {
    info.state = yield ? EL0_CONTEXT_YIELDED : EL0_CONTEXT_EXITED;
    if (yield)
      info.yields++;
    info.result = frame->x[0];
  } else {
    // Ownership is established by RUNNING + live kernel continuation +
    // vector 8 + saved EL0t, not by the user PC, SP, or an expected site.
    // Keep the faulting PC unchanged for diagnosis; never return to it.
    info.state = EL0_CONTEXT_FAULTED;
  }
  // The EL1 PC, mode, GPRs and original SP_EL0 come only from trusted
  // storage for this switch. Never retain that stack pointer after return.
  *frame = *kernel_return;
  kernel_return = NULL;
  return true;
}

static uint64_t check_frame(void) {
  uint64_t failures = 0;
  for (unsigned int i = 0; i < 31U; i++) {
    uint64_t expected = i == 0U ? info.resumptions * 7U :
        i == 1U ? info.resumptions : pattern(i);
    if (saved.x[i] != expected)
      failures |= 1ULL << i;
  }
  uintptr_t stack = (uintptr_t)__el0_stack_top - EL0_CONTEXT_STACK_BYTES;
  if (saved.sp != stack || saved.sp_el0 != stack)
    failures |= 1ULL << 31;
  if ((saved.spsr & 0xf000001fULL) != 0xa0000000ULL)
    failures |= 1ULL << 32;
  if ((saved.spsr & 0x3c0U) != expected_daif)
    failures |= 1ULL << 33;
  const char *pc = info.state == EL0_CONTEXT_YIELDED ?
      el0_context_program_resume : el0_context_exit_resume;
  if (saved.elr != (uintptr_t)pc)
    failures |= 1ULL << 34;
  if (info.resumptions > EL0_CONTEXT_SLICES ||
      (info.resumptions < EL0_CONTEXT_SLICES &&
       info.state != EL0_CONTEXT_YIELDED) ||
      (info.resumptions == EL0_CONTEXT_SLICES &&
       info.state != EL0_CONTEXT_EXITED))
    failures |= 1ULL << 35;
  return failures;
}

static void check_fault_test(const exception_frame_t *previous) {
  // Site/syndrome checks decide only whether this deliberate test PASSED.
  // They do not grant authority to resume an instruction after an abort.
  bool expected = info.fault_test && info.resumptions == 2U &&
      info.yields == 1U && info.result == 7U &&
      saved.elr == (uintptr_t)el0_context_fault_read &&
      (saved.esr & 0xfe0007ffULL) == 0x9200000fULL &&
      fault_far_valid(saved.esr) &&
      saved.far == (uintptr_t)&el0_test_kernel_canary;
  if (!expected)
    return;

  uint64_t failures = 0;
  for (unsigned int i = 0; i < 31U; i++) {
    uint64_t expected_reg = i == 0U ? 7U : i == 1U ? 1U :
        i == 3U ? (uintptr_t)&el0_test_kernel_canary : pattern(i);
    if (saved.x[i] != expected_reg)
      failures |= 1ULL << i;
  }
  uintptr_t base = (uintptr_t)__el0_stack_top - EL0_CONTEXT_STACK_BYTES;
  if (saved.sp != base || saved.sp_el0 != base)
    failures |= 1ULL << 31;
  if ((saved.spsr & 0xf000001fULL) != 0xa0000000ULL)
    failures |= 1ULL << 32;
  if ((saved.spsr & 0x3c0U) != expected_daif)
    failures |= 1ULL << 33;
  info.context_failures |= failures;
  // Read only fixed, kernel-accessible diagnostic storage after confirming
  // the known test site. Never follow pointers from an arbitrary fault.
  const volatile uint64_t *stack = (const volatile uint64_t *)base;
  for (unsigned int i = 0; i < 31U; i++) {
    if (stack[i] != previous->x[i])
      info.context_failures |= 1ULL << i;
  }
  if (stack[31] != (previous->spsr & 0xf0000000ULL))
    info.context_failures |= 1ULL << 32;
  if (stack[32] != previous->sp_el0)
    info.context_failures |= 1ULL << 31;
  if (stack[EL0_CONTEXT_PROGRESS_OFFSET / 8] != 7U ||
      stack[EL0_CONTEXT_GUARD_OFFSET / 8] != pattern(2) ||
      stack[EL0_CONTEXT_VARIANT_OFFSET / 8] != 1U ||
      el0_test_kernel_canary != EL0_TEST_KERNEL_CANARY)
    info.stack_ok = false;
  info.fault_test_passed = info.context_failures == 0 &&
      info.kernel_failures == 0 && info.stack_ok;
}

bool el0_context_resume(void) {
  if ((info.state != EL0_CONTEXT_READY && info.state != EL0_CONTEXT_YIELDED) ||
      info.context_failures || info.kernel_failures || !info.stack_ok)
    return false;

  // The previous yield is the expected state at the very first resumed
  // instruction. The EL0 payload snapshots it before touching scratch GPRs.
  exception_frame_t previous = saved;
  bool resuming = info.state == EL0_CONTEXT_YIELDED;
  info.kernel_failures |= el0_context_switch();
  if (info.state == EL0_CONTEXT_FAULTED) {
    // A generic fault may precede stack initialisation or carry an invalid
    // SP. Return without reading any EL0 memory. The fixed test alone has
    // a separate verifier for its known site and established stack.
    check_fault_test(&previous);
    return true;
  }
  info.context_failures |= check_frame();
  const volatile uint64_t *stack = (const volatile uint64_t *)
      ((uintptr_t)__el0_stack_top - EL0_CONTEXT_STACK_BYTES);
  if (resuming) {
    for (unsigned int i = 0; i < 31U; i++) {
      if (stack[i] != previous.x[i])
        info.context_failures |= 1ULL << i;
    }
    if (stack[31] != (previous.spsr & 0xf0000000ULL))
      info.context_failures |= 1ULL << 32;
    if (stack[32] != previous.sp_el0)
      info.context_failures |= 1ULL << 31;
  }
  if (stack[EL0_CONTEXT_PROGRESS_OFFSET / 8] != info.result ||
      stack[EL0_CONTEXT_GUARD_OFFSET / 8] != pattern(2))
    info.stack_ok = false;
  info.passed = !info.fault_test && info.state == EL0_CONTEXT_EXITED &&
      info.resumptions == EL0_CONTEXT_SLICES && info.yields == 5U &&
      info.result == 42U && info.stack_ok &&
      info.context_failures == 0U && info.kernel_failures == 0U;
  return true;
}
