#pragma once

#define VECTORS_TEST_SP_EL0 0x5a5a1000

#ifndef __ASSEMBLER__
#include "mmu_high_test.h"
#include "exceptions.h"


typedef struct {
  bool validated, switched, observed, restored, persistent, masks_restored;
  bool passed, permanent, already_active;
  const char *status;
  vaddr_t high_vbar, expected_pc, observed_pc, continuation, entry_sp, handler_sp;
  paddr_t vector_physical, expected_physical, observed_physical, handler_stack_physical;
  uint64_t vector, esr, spsr, sp_el0, failures;
  uint64_t original_daif, final_daif;
  mmu_high_hw_state_t before, installed, after;
} mmu_vectors_result_t;

// Foreground, core 0, one armed synchronous trap; never a switching API.
bool mmu_vectors_run(mmu_high_test_control_t *owner,
    const mmu_vector_code_t *code, mmu_vectors_result_t *result);
bool mmu_vectors_test(mmu_vectors_result_t *result);
bool mmu_vectors_install(mmu_high_test_control_t *owner,
    const mmu_vector_code_t *code, mmu_vectors_result_t *result);
bool mmu_vectors_validate(mmu_high_test_control_t *owner,
    const mmu_high_hw_state_t *state);
bool mmu_vectors_inspect(mmu_vectors_result_t *result);

// Fixed one-shot evidence per IRQ path. Captured in IRQ, translated and
// reported in foreground. No retained live frame pointers.
typedef struct {
  bool observed, verified, handler_completed;
  vaddr_t pc, expected_pc;
  paddr_t physical, handler_stack_physical;
  uint32_t source;
  uint64_t count_before, count_after;
  exception_frame_t frame;
  mmu_high_hw_state_t state;
} mmu_vector_irq_record_t;
typedef struct {
  bool valid;
  mmu_vector_irq_record_t paths[2];
} mmu_vector_irq_result_t;
void exception_irq_entry(exception_frame_t *frame, vaddr_t observed_pc);
bool mmu_vectors_irq_arm_owner(mmu_high_test_control_t *owner);
bool mmu_vectors_irq_read_owner(mmu_high_test_control_t *owner,
    mmu_vector_irq_result_t *result);
bool mmu_vectors_irq_arm(void);
bool mmu_vectors_irq_read(mmu_vector_irq_result_t *result);
// Called from sync assembly only after SAVE_FRAME. Outside the armed test
// it forwards unchanged to the existing exception dispatcher.
void exception_sync_entry(exception_frame_t *frame, vaddr_t observed_pc);

// Private hardware boundary: no translation-register writes are exposed.
typedef enum { MMU_VECTOR_WRITE_VBAR, MMU_VECTOR_ISB } mmu_vector_op_t;
void mmu_vectors_hw_operation(mmu_vector_op_t operation, uint64_t value);
uint64_t mmu_vectors_hw_trigger(void);
#endif
