#include "mmu_vectors.h"

// Arm AArch64 Exception Model (102412), vector-table selection; the
// context-synchronising ISB follows each VBAR_EL1 write and precedes use.
// https://support.arm.com/documentation/102437/0100/Changing-Exception-levels
void mmu_vectors_hw_operation(mmu_vector_op_t op, uint64_t value) {
  switch (op) {
    case MMU_VECTOR_WRITE_VBAR:
      __asm__ volatile("msr vbar_el1, %0" : : "r"(value) : "memory");
      break;
    case MMU_VECTOR_ISB:
      __asm__ volatile("isb" ::: "memory");
      break;
    default:
      mmu_high_hw_fatal("Invalid vector test operation");
  }
}
extern uint64_t exception_vector_test(void);
uint64_t mmu_vectors_hw_trigger(void) { return exception_vector_test(); }
