#pragma once

// The fixed context's SVC convention. No pointer arguments or return GPRs:
// YIELD suspends with all registers unchanged; EXIT terminates with x0.
#define EL0_CONTEXT_COUNT 2
#define EL0_CONTEXT_YIELD_SVC 0x40
#define EL0_CONTEXT_EXIT_SVC  0x41
#define EL0_CONTEXT_SLICES    6
#define EL0_CONTEXT_STACK_BYTES 304
#define EL0_CONTEXT_PROGRESS_OFFSET 272
#define EL0_CONTEXT_GUARD_OFFSET 280
#define EL0_CONTEXT_VARIANT_OFFSET 288
#define EL0_CONTEXT_PATTERN_OFFSET 296

#ifndef __ASSEMBLER__
#include <stdbool.h>
#include <stdint.h>
#include "exceptions.h"

typedef enum {
  EL0_CONTEXT_EMPTY,
  EL0_CONTEXT_READY,
  EL0_CONTEXT_RUNNING,
  EL0_CONTEXT_YIELDED,
  EL0_CONTEXT_EXITED,
  EL0_CONTEXT_FAULTED
} el0_context_state_t;

typedef struct {
  el0_context_state_t state;
  uint64_t resumptions;
  uint64_t yields;
  uint64_t result;
  // GPRs 0..30, SP 31, NZCV/mode 32, DAIF 33, PC 34, lifecycle 35.
  uint64_t context_failures;
  uint64_t kernel_failures;
  bool stack_ok;
  bool passed;
  bool fault_test;
  bool fault_test_passed;
} el0_context_info_t;

typedef struct {
  exception_frame_t frame; // Includes raw ESR/FAR, fault PC and every GPR.
  bool far_valid;
  unsigned int context_id;
} el0_context_fault_t;

// Exactly two static contexts (IDs 0 and 1), foreground calls only.
// start/reset are refused while running; start also refuses a ready,
// yielded or faulted context. Reset discards only the selected context
// and its retained fault record; the peer and its stack remain unchanged.
// FAULTED requires reset before starting again. No allocation occurs.
// Invalid IDs are rejected. No mutation is allowed during another switch.
// Both stacks are EL0-accessible in the same address space: no mutual
// memory isolation is claimed. State and ownership are independent.
bool el0_context_start(unsigned int id);
bool el0_context_start_fault_test(unsigned int id);
bool el0_context_resume(unsigned int id);
bool el0_context_reset(unsigned int id);
bool el0_context_get_info(unsigned int id, el0_context_info_t *info);
// Returns false (and clears the output) unless a fault record is retained.
bool el0_context_get_fault(unsigned int id, el0_context_fault_t *fault);
bool el0_context_owns_stack(unsigned int id);
bool el0_context_handle_exception(exception_frame_t *frame);
#endif
