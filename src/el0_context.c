#include "el0_context.h"
#include "el0_test.h"
#include "string.h"

extern char __el0_stack_bottom[], __el0_stack_top[];
extern const char el0_context_program[], el0_context_program_end[];
extern const char el0_context_program_resume[], el0_context_exit_resume[];
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
      info.state == EL0_CONTEXT_RUNNING || info.state == EL0_CONTEXT_YIELDED;
}

bool el0_context_start(void) {
  if (el0_context_owns_stack() || el0_test_active() ||
      !el0_test_mappings_ready())
    return false;

  memset(__el0_stack_bottom, 0, 4096U);
  kernel_return = NULL;
  expected_daif = 0;
  saved = (exception_frame_t){0};
  for (unsigned int i = 2; i < 31U; i++)
    saved.x[i] = pattern(i);
  saved.elr = (uintptr_t)el0_context_program;
  saved.sp = saved.sp_el0 = (uintptr_t)__el0_stack_top;
  saved.spsr = 0xa0000000ULL; // EL0t. DAIF added on first switch.
  info = (el0_context_info_t){.state = EL0_CONTEXT_READY, .stack_ok = true};
  return true;
}

bool el0_context_reset(void) {
  if (info.state == EL0_CONTEXT_RUNNING)
    return false;
  saved = (exception_frame_t){0};
  kernel_return = NULL;
  info = (el0_context_info_t){0};
  return true;
}

void el0_context_get_info(el0_context_info_t *result) {
  if (result)
    *result = info;
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
  // Own only this program's aligned SVC continuations. Aborts, undefined
  // instructions and other SVCs retain the normal fatal path.
  if ((!yield && !exit) || (frame->elr & 3U) ||
      frame->elr < (uintptr_t)el0_context_program ||
      frame->elr >= (uintptr_t)el0_context_program_end)
    return false;

  saved = *frame;
  info.state = yield ? EL0_CONTEXT_YIELDED : EL0_CONTEXT_EXITED;
  if (yield)
    info.yields++;
  info.result = frame->x[0];
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

bool el0_context_resume(void) {
  if ((info.state != EL0_CONTEXT_READY && info.state != EL0_CONTEXT_YIELDED) ||
      info.context_failures || info.kernel_failures || !info.stack_ok)
    return false;

  // The previous yield is the expected state at the very first resumed
  // instruction. The EL0 payload snapshots it before touching scratch GPRs.
  exception_frame_t previous = saved;
  bool resuming = info.state == EL0_CONTEXT_YIELDED;
  info.kernel_failures |= el0_context_switch();
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
  info.passed = info.state == EL0_CONTEXT_EXITED &&
      info.resumptions == EL0_CONTEXT_SLICES && info.yields == 5U &&
      info.result == 42U && info.stack_ok &&
      info.context_failures == 0U && info.kernel_failures == 0U;
  return true;
}
