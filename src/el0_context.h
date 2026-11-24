#pragma once

// The fixed context's SVC convention. No pointer arguments or return GPRs:
// YIELD suspends with all registers unchanged; EXIT terminates with x0.
#define EL0_CONTEXT_YIELD_SVC 0x40
#define EL0_CONTEXT_EXIT_SVC  0x41
#define EL0_CONTEXT_SLICES    6
#define EL0_CONTEXT_STACK_BYTES 304
#define EL0_CONTEXT_PROGRESS_OFFSET 272
#define EL0_CONTEXT_GUARD_OFFSET 280

#ifndef __ASSEMBLER__
#include <stdbool.h>
#include <stdint.h>
#include "exceptions.h"

typedef enum {
  EL0_CONTEXT_EMPTY,
  EL0_CONTEXT_READY,
  EL0_CONTEXT_RUNNING,
  EL0_CONTEXT_YIELDED,
  EL0_CONTEXT_EXITED
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
} el0_context_info_t;

// One fixed program, serial foreground calls only. start/reset are refused
// while running; start also refuses a ready/yielded context. Reset of a
// suspended context explicitly discards its state. No allocation occurs.
bool el0_context_start(void);
bool el0_context_resume(void);
bool el0_context_reset(void);
void el0_context_get_info(el0_context_info_t *info);
bool el0_context_owns_stack(void);
bool el0_context_handle_exception(exception_frame_t *frame);
#endif
