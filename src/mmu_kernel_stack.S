#include "mmu_kernel_stack.h"
.section .text, "ax"
.balign 4
.global mmu_kernel_stack_jump
.type mmu_kernel_stack_jump, %function
// Boot-only, non-returning entry. Old frames are abandoned, not copied.
// x0=validated high top, x1=validated low void continuation. DAIF is held.
mmu_kernel_stack_jump:
  msr daifset, #0xf
  mov x16, x1
  mov sp, x0
  mov x0, xzr
  mov x1, xzr
  mov x2, xzr
  mov x3, xzr
  mov x4, xzr
  mov x5, xzr
  mov x6, xzr
  mov x7, xzr
  mov x8, xzr
  mov x9, xzr
  mov x10, xzr
  mov x11, xzr
  mov x12, xzr
  mov x13, xzr
  mov x14, xzr
  mov x15, xzr
  mov x17, xzr
  mov x18, xzr
  mov x19, xzr
  mov x20, xzr
  mov x21, xzr
  mov x22, xzr
  mov x23, xzr
  mov x24, xzr
  mov x25, xzr
  mov x26, xzr
  mov x27, xzr
  mov x28, xzr
  mov x29, xzr
  mov x30, xzr
  isb
  br x16
.size mmu_kernel_stack_jump, . - mmu_kernel_stack_jump

.global mmu_kernel_stack_call
.type mmu_kernel_stack_call, %function
// Runtime nested-call proof on the current stack. Never assigns SP.
mmu_kernel_stack_call:
  stp x19, x30, [sp, #-64]!
  mov x19, x0
  mov x1, sp
  str x1, [x19, #KSTACK_OUTER_SP]
  mov x0, #6
  mov x1, #7
  stp x0, x1, [sp, #16]
  mov x2, #0xabc
  str x2, [sp, #32]
  bl permanent_stack_inner
  str x0, [x19, #KSTACK_RESULT]
  ldp x1, x2, [sp, #16]
  cmp x1, #6
  cset x3, ne
  cmp x2, #7
  cset x4, ne
  orr x3, x3, x4
  ldr x1, [sp, #32]
  cmp x1, #0xabc
  cset x4, ne
  orr x3, x3, x4
  str x3, [x19, #KSTACK_FAILURES]
  ldp x19, x30, [sp], #64
  ret
permanent_stack_inner:
  stp x29, x30, [sp, #-32]!
  mov x2, sp
  str x2, [x19, #KSTACK_INNER_SP]
  stp x0, x1, [sp, #16]
  ldp x0, x1, [sp, #16]
  mul x0, x0, x1
  ldp x29, x30, [sp], #32
  ret
.size mmu_kernel_stack_call, . - mmu_kernel_stack_call
