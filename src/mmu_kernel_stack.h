#pragma once

#define KSTACK_OUTER_SP 0
#define KSTACK_INNER_SP 8
#define KSTACK_RESULT 16
#define KSTACK_FAILURES 24
#define KSTACK_EXECUTION_SIZE 32

#ifndef __ASSEMBLER__
#include "mmu_high_test.h"

typedef struct {
  vaddr_t outer_sp, inner_sp;
  uint64_t result, failures;
} mmu_kernel_stack_execution_t;
_Static_assert(offsetof(mmu_kernel_stack_execution_t, outer_sp) ==
               KSTACK_OUTER_SP,
               "kernel stack SP offset");
_Static_assert(offsetof(mmu_kernel_stack_execution_t, inner_sp) ==
               KSTACK_INNER_SP,
               "kernel stack inner SP offset");
_Static_assert(offsetof(mmu_kernel_stack_execution_t, result) == KSTACK_RESULT,
               "kernel stack result offset");
_Static_assert(offsetof(mmu_kernel_stack_execution_t, failures) ==
               KSTACK_FAILURES,
               "kernel stack failures offset");
_Static_assert(sizeof(mmu_kernel_stack_execution_t) == KSTACK_EXECUTION_SIZE,
               "kernel stack execution size");
typedef struct {
  mmu_high_hw_state_t before;
  vaddr_t high_base, high_top, continuation;
} mmu_kernel_stack_plan_t;
typedef struct {
  bool valid, executed, translated, preserved;
  mmu_stack_phase_t phase;
  paddr_t physical_base, physical_top, current_physical;
  paddr_t outer_physical, inner_physical;
  vaddr_t high_base, high_top;
  uint64_t original_daif, final_daif;
  mmu_high_hw_state_t state, after;
  mmu_kernel_stack_execution_t execution;
} mmu_kernel_stack_report_t;

// Private boot preflight. Caller holds DAIF; rejection publishes no state.
bool mmu_kernel_stack_prepare(mmu_high_test_control_t *owner,
    const mmu_high_hw_state_t *state, vaddr_t continuation,
    mmu_kernel_stack_plan_t *plan);
__attribute__((noreturn))
void mmu_kernel_stack_enter_owner(mmu_high_test_control_t *owner,
    void (*continuation)(void));
void mmu_kernel_stack_complete_owner(mmu_high_test_control_t *owner);
bool mmu_kernel_stack_report_owner(mmu_high_test_control_t *owner,
    bool execute, mmu_kernel_stack_report_t *result);
// Bindings to the one boot-owned upper hierarchy.
__attribute__((noreturn))
void mmu_kernel_stack_enter(void (*continuation)(void));
void mmu_kernel_stack_complete(void);
bool mmu_kernel_stack_report(bool execute, mmu_kernel_stack_report_t *result);
// Architecture boundaries: jump cannot return to an old low frame.
__attribute__((noreturn)) void mmu_kernel_stack_jump(vaddr_t high_top,
    void (*continuation)(void));
void mmu_kernel_stack_call(mmu_kernel_stack_execution_t *result);
#endif
