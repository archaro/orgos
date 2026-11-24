#include "exceptions.h"

// The only recoverable synchronous trap in H2.1a is this exact EL1 test
// site. In particular, an arbitrary SVC is not a kernel service ABI.
extern const char exception_test_resume[];

void exception_dispatch(exception_frame_t *frame) {
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
