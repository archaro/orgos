#pragma once

#define STACK_LOW_SP 0
#define STACK_LOW_FRAME 8
#define STACK_HIGH_TOP 16
#define STACK_OBSERVED_SP 24
#define STACK_LOWEST_SP 32
#define STACK_RESULT 40
#define STACK_LOCAL_FAILURES 48
#define STACK_CALLEE_FAILURES 56
#define STACK_LOW_MATCH 64
#define STACK_INITIAL_NZCV 72
#define STACK_FINAL_NZCV 80
#define STACK_DAIF 88
#define STACK_SAVED_FP 96
#define STACK_LOW_RETURN 104
#define STACK_COPY_BYTES 112
#define STACK_COPY_POINTER 120
#define STACK_PROBE_SIZE 128
#define STACK_PAGE_BYTES 4096
#define STACK_WITNESS_BYTES 16384

#ifndef __ASSEMBLER__
#include "mmu_high_test.h"
#include <stddef.h>

typedef struct {
  vaddr_t low_sp, low_frame, high_top, observed_sp, lowest_sp;
  uint64_t result, local_failures, callee_failures, low_match;
  uint64_t initial_nzcv, final_nzcv, daif;
  vaddr_t saved_fp, low_return;
  uint64_t copy_bytes;
  vaddr_t copy_pointer;
} mmu_stack_probe_t;
#define STACK_OFFSET(field, offset) \
  _Static_assert(offsetof(mmu_stack_probe_t, field) == offset, \
                 "stack probe offset: " #field)
STACK_OFFSET(low_sp, STACK_LOW_SP);
STACK_OFFSET(low_frame, STACK_LOW_FRAME);
STACK_OFFSET(high_top, STACK_HIGH_TOP);
STACK_OFFSET(observed_sp, STACK_OBSERVED_SP);
STACK_OFFSET(lowest_sp, STACK_LOWEST_SP);
STACK_OFFSET(result, STACK_RESULT);
STACK_OFFSET(local_failures, STACK_LOCAL_FAILURES);
STACK_OFFSET(callee_failures, STACK_CALLEE_FAILURES);
STACK_OFFSET(low_match, STACK_LOW_MATCH);
STACK_OFFSET(initial_nzcv, STACK_INITIAL_NZCV);
STACK_OFFSET(final_nzcv, STACK_FINAL_NZCV);
STACK_OFFSET(daif, STACK_DAIF);
STACK_OFFSET(saved_fp, STACK_SAVED_FP);
STACK_OFFSET(low_return, STACK_LOW_RETURN);
STACK_OFFSET(copy_bytes, STACK_COPY_BYTES);
STACK_OFFSET(copy_pointer, STACK_COPY_POINTER);
#undef STACK_OFFSET
_Static_assert(sizeof(mmu_stack_probe_t) == STACK_PROBE_SIZE,
               "stack probe size");

typedef struct {
  uint64_t *page, *witness; // CPU aliases, distinct from registered PAs.
  paddr_t physical, witness_physical;
} mmu_stack_storage_t;
typedef struct {
  vaddr_t entry, end, wrapper_return, nested_return;
} mmu_stack_code_t;
typedef struct {
  bool validated, executed, translated, canaries, contents, low_intact;
  bool restored, registers_preserved, masks_restored, passed;
  const char *status;
  paddr_t physical, observed_physical, lowest_physical;
  vaddr_t high_base, high_top;
  uint64_t original_daif, final_daif;
  mmu_high_hw_state_t before, after;
  mmu_stack_probe_t probe;
} mmu_stack_result_t;

// One foreground experiment on core 0. Storage must be exclusive static
// kernel BSS, disjoint from tables, owner/report objects and each other.
// No allocation, mapping edits, exceptions or C calls on the high stack.
bool mmu_stack_run(mmu_high_test_control_t *owner,
    const mmu_stack_storage_t *storage, const mmu_stack_code_t *code,
    mmu_stack_result_t *result);
// The old runtime binding is retired; this controller is a historical
// regression and explicitly rejects the permanent HIGH phase.
// Private assembly boundary; callable only with validated diagnostic data.
void mmu_stack_call(vaddr_t high_top, mmu_stack_probe_t *probe,
    uint64_t *witness, vaddr_t normal_top);
#endif
