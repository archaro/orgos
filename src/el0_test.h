#pragma once

#define EL0_TEST_CHECK_SVC 0x30
#define EL0_TEST_EXIT_SVC  0x31
#define EL0_TEST_MEMORY_BEGIN_SVC 0x32
#define EL0_TEST_READ_DONE_SVC    0x33
#define EL0_TEST_WRITE_DONE_SVC   0x34
#define EL0_TEST_KERNEL_CANARY    0x13579bdf2468ace0
#define EL0_TEST_CODE_CANARY      0xfedcba9876543210
#define EL0_TEST_RESULT   42

#ifndef __ASSEMBLER__
#include <stdbool.h>
#include <stdint.h>
#include "exceptions.h"

typedef enum {
  EL0_TEST_KERNEL_READ,
  EL0_TEST_CODE_WRITE,
  EL0_TEST_STACK_EXEC,
  EL0_TEST_MEMORY_FAULT_COUNT
} el0_test_memory_fault_t;

typedef struct {
  bool observed;
  uint64_t esr;
  uint64_t far;
} el0_test_fault_t;

typedef struct {
  bool mappings;
  bool completed;
  bool svc_roundtrip;
  bool privileged_fault;
  bool stack_read;
  bool kernel_memory_unchanged;
  bool code_unchanged;
  el0_test_fault_t memory_faults[EL0_TEST_MEMORY_FAULT_COUNT];
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
