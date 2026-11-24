#pragma once

#define EL0_TEST_CHECK_SVC 0x30
#define EL0_TEST_EXIT_SVC  0x31
#define EL0_TEST_RESULT   42

#ifndef __ASSEMBLER__
#include <stdbool.h>
#include <stdint.h>
#include "exceptions.h"

typedef struct {
  bool mappings;
  bool completed;
  bool svc_roundtrip;
  bool privileged_fault;
  uint64_t result;
  // Bits 0..30: GPRs, 31: SP, 32: NZCV/mode, 33: DAIF,
  // 34: missing privilege fault. Zero means no context mismatch.
  uint64_t failures;
  // Kernel GPRs 0..30, SP 31, NZCV 32, DAIF 33, original SP_EL0 34.
  uint64_t kernel_failures;
} el0_test_result_t;

// Serial foreground diagnostic only; not a program loader or syscall ABI.
bool el0_test_run(el0_test_result_t *result);
// Internal exception-dispatch hook. False means this trap is not owned.
bool el0_test_handle_exception(exception_frame_t *frame);
#endif
