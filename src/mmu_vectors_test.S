#include "mmu_vectors.h"

.section .text, "ax"
.balign 4
.global exception_vector_test
.type exception_vector_test, %function
// Reuse the original x0-x30/SP/NZCV/DAIF diagnostic. Also seed and verify
// SP_EL0 (bit 34), restoring its original value before returning to C.
exception_vector_test:
  stp x29, x30, [sp, #-32]!
  mrs x0, sp_el0
  str x0, [sp, #16]
  movz x0, #(VECTORS_TEST_SP_EL0 & 0xffff)
  movk x0, #((VECTORS_TEST_SP_EL0 >> 16) & 0xffff), lsl #16
  msr sp_el0, x0
  bl exception_test
  mrs x1, sp_el0
  movz x2, #(VECTORS_TEST_SP_EL0 & 0xffff)
  movk x2, #((VECTORS_TEST_SP_EL0 >> 16) & 0xffff), lsl #16
  cmp x1, x2
  cset x1, ne
  orr x0, x0, x1, lsl #34
  ldr x1, [sp, #16]
  msr sp_el0, x1
  ldp x29, x30, [sp], #32
  ret
.size exception_vector_test, . - exception_vector_test
