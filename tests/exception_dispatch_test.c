#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

#include "exceptions.h"

const char exception_test_resume[1] = {0};
static jmp_buf panic_return;
static uint64_t panic_args[5];

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
  if (setjmp(panic_return) == 0) {
    exception_dispatch(&frame);
    assert(!"unexpected trap returned");
  }
  assert(panic_args[0] == frame.vector);
  assert(panic_args[1] == frame.esr);
  assert(panic_args[2] == frame.elr);
  assert(panic_args[3] == frame.far);
  assert(panic_args[4] == frame.spsr);
}

int main(void) {
  exception_frame_t frame = {
    .sp = 0x12340000,
    .elr = (uintptr_t)exception_test_resume,
    .spsr = 0xa0000345, // Nonzero NZCV and selected DAIF masks, EL1h.
    .esr = (0x15ULL << 26) | (1ULL << 25) | EXCEPTION_TEST_SVC,
    .far = 0xabcdef1234567890ULL,
    .vector = 4,
  };
  for (unsigned int i = 0; i < 31; i++)
    frame.x[i] = 0x9abc567812340100ULL + i;
  exception_frame_t before = frame;
  exception_dispatch(&frame);
  assert(memcmp(&before, &frame, sizeof(frame)) == 0);

  // Every unsupported vector, including lower-EL SVC, remains fatal.
  for (unsigned int i = 0; i < 16; i++) {
    if (i != 4U) {
      frame = before;
      frame.vector = i;
      expect_panic(frame);
    }
  }
  // Every other exception class, including aborts, BRK and undefined.
  for (unsigned int ec = 0; ec < 64; ec++) {
    if (ec != 0x15U) {
      frame = before;
      frame.esr = (frame.esr & ~(0x3fULL << 26)) | ((uint64_t)ec << 26);
      expect_panic(frame);
    }
  }
  frame = before;
  frame.esr ^= 1; // Different SVC immediate.
  expect_panic(frame);
  frame = before;
  frame.esr &= ~(1ULL << 25); // Invalid instruction length.
  expect_panic(frame);
  frame = before;
  frame.elr += 4; // Same immediate at another site is not recognised.
  expect_panic(frame);
  for (unsigned int mode = 0; mode < 32; mode++) {
    if (mode != 5U) {
      frame = before;
      frame.spsr = (frame.spsr & ~0x1fULL) | mode;
      expect_panic(frame);
    }
  }
  puts("exception dispatch tests: PASS");
  return 0;
}
