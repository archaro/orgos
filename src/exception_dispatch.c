#include "exceptions.h"
#include "el0_test.h"
#include "el0_context.h"

// The EL1 diagnostic remains restricted to this exact test site.
// An arbitrary SVC is not a kernel service ABI.
extern const char exception_test_resume[];

void exception_dispatch(exception_frame_t *frame) {
  if (el0_context_handle_exception(frame))
    return;

  if (el0_test_handle_exception(frame))
    return;

  if (frame->vector == 4U &&
      frame->esr == ((0x15ULL << 26) | (1ULL << 25) |
                     EXCEPTION_TEST_SVC) &&
      (frame->spsr & 0x1fU) == 5U &&
      frame->elr == (uintptr_t)exception_test_resume) {
    // SVC already records the following instruction in ELR_EL1.
    // Leave ELR and all interrupted registers unchanged.
    return;
  }

  exception_panic(frame->vector, frame->esr, frame->elr,
                  frame->far, frame->spsr);
}
