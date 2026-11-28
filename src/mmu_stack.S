#include "mmu_stack.h"

.section .text, "ax"
.balign 4
.global mmu_stack_call
.type mmu_stack_call, %function
// x0=high top; x1=static report; x2=static 16 KiB witness; x3=normal top.
// Freeze the live low stack, including this frame, before changing SP.
mmu_stack_call:
  sub sp, sp, #128
  stp x19, x20, [sp, #0]
  stp x21, x22, [sp, #16]
  stp x23, x24, [sp, #32]
  stp x25, x26, [sp, #48]
  stp x27, x28, [sp, #64]
  stp x29, x30, [sp, #80]
  mrs x4, nzcv
  mrs x5, daif
  stp x4, x5, [sp, #96]
  str x17, [sp, #112]
  mov x6, #0x5354
  str x6, [sp, #120]
  mov x19, sp
  mov x20, x1
  mov x17, x1 // Additional report-pointer witness; nested code leaves x17.
  mov x21, x2
  mov x22, x3
  sub x23, x22, x19
  mov x24, x0
  add x6, x19, #128
  str x6, [x20, #STACK_LOW_SP]
  str x19, [x20, #STACK_LOW_FRAME]
  str x24, [x20, #STACK_HIGH_TOP]
  str x4, [x20, #STACK_INITIAL_NZCV]
  str x29, [x20, #STACK_SAVED_FP]
  str x23, [x20, #STACK_COPY_BYTES]
  str x21, [x20, #STACK_COPY_POINTER]
  mov x4, x19
  mov x5, x21
  mov x6, x23
1:
  ldp x7, x8, [x4], #16
  stp x7, x8, [x5], #16
  subs x6, x6, #16
  b.ne 1b
  mov x25, #0x725
  mov x26, #0x726
  mov x27, #0x727
  mov x28, #0x728
  mov sp, x24
  bl stack_outer
.global mmu_stack_wrapper_return
mmu_stack_wrapper_return:
  // Do not restore an untrusted SP. Check the anchors before using them.
  cmp x20, x17
  b.ne stack_fatal
  ldr x4, [x20, #STACK_LOW_FRAME]
  cmp x19, x4
  b.ne stack_fatal
  ldr x4, [x20, #STACK_COPY_POINTER]
  cmp x21, x4
  b.ne stack_fatal
  ldr x4, [x20, #STACK_COPY_BYTES]
  cmp x23, x4
  b.ne stack_fatal
  add x4, x19, x23
  cmp x22, x4
  b.ne stack_fatal
  ldr x4, [x20, #STACK_HIGH_TOP]
  cmp x24, x4
  b.ne stack_fatal
  mov x4, sp
  cmp x24, x4
  b.ne stack_fatal
  mrs x4, daif
  str x4, [x20, #STACK_DAIF]
  ldr x5, [x21, #104]
  cmp x4, x5
  b.ne stack_fatal
  // Exact active low-stack comparison while still on the high stack.
  mov x4, x19
  mov x5, x21
  mov x6, x23
2:
  ldp x7, x8, [x4], #16
  ldp x9, x10, [x5], #16
  cmp x7, x9
  b.ne stack_fatal
  cmp x8, x10
  b.ne stack_fatal
  subs x6, x6, #16
  b.ne 2b
  mov x4, #1
  str x4, [x20, #STACK_LOW_MATCH]
  mov x3, #0
.macro STACK_CHECK reg, value, bit
  mov x4, #\value
  cmp \reg, x4
  cset x5, ne
  orr x3, x3, x5, lsl #\bit
.endm
  STACK_CHECK x25, 0x725, 0
  STACK_CHECK x26, 0x726, 1
  STACK_CHECK x27, 0x727, 2
  STACK_CHECK x28, 0x728, 3
  ldr x4, [x20, #STACK_SAVED_FP]
  cmp x29, x4
  cset x5, ne
  orr x3, x3, x5, lsl #4
  str x3, [x20, #STACK_CALLEE_FAILURES]
  adr x4, mmu_stack_wrapper_return
  str x4, [x20, #STACK_LOW_RETURN]
  mov sp, x19
  ldr x4, [sp, #96]
  msr nzcv, x4
  mrs x4, nzcv
  str x4, [x20, #STACK_FINAL_NZCV]
  ldr x17, [sp, #112]
  ldp x19, x20, [sp, #0]
  ldp x21, x22, [sp, #16]
  ldp x23, x24, [sp, #32]
  ldp x25, x26, [sp, #48]
  ldp x27, x28, [sp, #64]
  ldp x29, x30, [sp, #80]
  add sp, sp, #128
  ret
.size mmu_stack_call, . - mmu_stack_call

// Ordinary low-address code, with all frame accesses through high SP.
stack_outer:
  stp x29, x30, [sp, #-64]!
  mov x29, sp
  str x29, [x20, #STACK_OBSERVED_SP]
  mov x0, #6
  mov x1, #7
  stp x0, x1, [sp, #16]
  mov x2, #0xabc
  mov x3, #0xdef
  stp x2, x3, [sp, #32]
  bl stack_inner
.global mmu_stack_nested_return
mmu_stack_nested_return:
  str x0, [x20, #STACK_RESULT]
  ldp x2, x3, [sp, #16]
  cmp x2, #6
  cset x4, ne
  cmp x3, #7
  cset x5, ne
  orr x4, x4, x5
  ldp x2, x3, [sp, #32]
  cmp x2, #0xabc
  cset x5, ne
  orr x4, x4, x5
  cmp x3, #0xdef
  cset x5, ne
  orr x4, x4, x5
  cmp x0, #42
  cset x5, ne
  orr x4, x4, x5
  str x4, [x20, #STACK_LOCAL_FAILURES]
  ldp x29, x30, [sp], #64
  ret

stack_inner:
  stp x29, x30, [sp, #-32]!
  mov x2, sp
  str x2, [x20, #STACK_LOWEST_SP]
  // Real nested frame stores, loads and calculation, not SP-only evidence.
  ldp x0, x1, [x29, #16]
  stp x0, x1, [sp, #16]
  ldp x0, x1, [sp, #16]
  mul x0, x0, x1
  ldp x29, x30, [sp], #32
  ret

stack_fatal:
  adr x0, stack_failure
  bl mmu_high_hw_fatal
3:
  wfe
  b 3b
stack_failure:
  .asciz "High-stack anchors, masks or original low frames were corrupted"
.balign 4
.global mmu_stack_code_end
mmu_stack_code_end:
