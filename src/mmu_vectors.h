#pragma once

#define VECTORS_TEST_SP_EL0 0x5a5a1000

#ifndef __ASSEMBLER__
#include "mmu_high_test.h"
#include "exceptions.h"

// Linker-derived, immutable bounds of the shared vector/entry assembly.
// All C targets are absolute low addresses in the assembly literal pool.
typedef struct {
  vaddr_t vectors, end, observed_site, continuation;
  vaddr_t handlers[4];
} mmu_vector_code_t;

typedef struct {
  bool validated, switched, observed, restored, persistent, masks_restored;
  bool passed;
  const char *status;
  vaddr_t high_vbar, expected_pc, observed_pc, continuation, entry_sp;
  paddr_t vector_physical, expected_physical, observed_physical;
  uint64_t vector, esr, spsr, sp_el0, failures;
  uint64_t original_daif, final_daif;
  mmu_high_hw_state_t before, installed, after;
} mmu_vectors_result_t;

// Foreground, core 0, one armed synchronous trap; never a switching API.
bool mmu_vectors_run(mmu_high_test_control_t *owner,
    const mmu_vector_code_t *code, mmu_vectors_result_t *result);
bool mmu_vectors_test(mmu_vectors_result_t *result);
// Called from sync assembly only after SAVE_FRAME. Outside the armed test
// it forwards unchanged to the existing exception dispatcher.
void exception_sync_entry(exception_frame_t *frame, vaddr_t observed_pc);

// Private hardware boundary: no translation-register writes are exposed.
typedef enum { MMU_VECTOR_WRITE_VBAR, MMU_VECTOR_ISB } mmu_vector_op_t;
void mmu_vectors_hw_operation(mmu_vector_op_t operation, uint64_t value);
uint64_t mmu_vectors_hw_trigger(void);
#endif
