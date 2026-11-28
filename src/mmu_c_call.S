#include "mmu_c_island.h"
.section .text, "ax"
.balign 4
.macro C_CHECK reg, value, bit
  mov x4, #\value
  cmp \reg, x4
  cset x5, ne
  orr x3, x3, x5, lsl #\bit
.endm

.global mmu_c_call
.global mmu_c_call_return
.type mmu_c_call, %function
// x0=high entry, x1=evidence on permanent stack, x2=high RO,
// x3=high RW, x4=trusted wrapper-result pointer. No stack alias switch.
mmu_c_call:
  sub sp, sp, #160
  stp x19, x20, [sp, #0]
  stp x21, x22, [sp, #16]
  stp x23, x24, [sp, #32]
  stp x25, x26, [sp, #48]
  stp x27, x28, [sp, #64]
  stp x29, x30, [sp, #80]
  stp x0, x1, [sp, #96]
  stp x2, x3, [sp, #112]
  str x4, [sp, #128]
  mrs x3, nzcv
  str x3, [sp, #136]
  mov x3, sp
  str x3, [x4, #C_CALL_BEFORE_SP]
  mov x19, #0x719
  mov x20, #0x720
  mov x21, #0x721
  mov x22, #0x722
  mov x23, #0x723
  mov x24, #0x724
  mov x25, #0x725
  mov x26, #0x726
  mov x27, #0x727
  mov x28, #0x728
  mov x29, #0x729
  ldr x16, [sp, #96]
  ldr x0, [sp, #104]
  ldp x1, x2, [sp, #112]
  blr x16
mmu_c_call_return:
  ldr x6, [sp, #128]
  str x0, [x6, #C_CALL_RESULT]
  mov x3, sp
  str x3, [x6, #C_CALL_AFTER_SP]
  adr x5, mmu_c_call_return
  str x5, [x6, #C_CALL_RETURN]
  ldr x7, [x6, #C_CALL_BEFORE_SP]
  cmp x7, x3
  cset x3, ne
  lsl x3, x3, #11
  C_CHECK x19, 0x719, 0
  C_CHECK x20, 0x720, 1
  C_CHECK x21, 0x721, 2
  C_CHECK x22, 0x722, 3
  C_CHECK x23, 0x723, 4
  C_CHECK x24, 0x724, 5
  C_CHECK x25, 0x725, 6
  C_CHECK x26, 0x726, 7
  C_CHECK x27, 0x727, 8
  C_CHECK x28, 0x728, 9
  C_CHECK x29, 0x729, 10
  str x3, [x6, #C_CALL_FAILURES]
  ldp x19, x20, [sp, #0]
  ldp x21, x22, [sp, #16]
  ldp x23, x24, [sp, #32]
  ldp x25, x26, [sp, #48]
  ldp x27, x28, [sp, #64]
  ldp x29, x30, [sp, #80]
  ldr x4, [sp, #136]
  msr nzcv, x4
  mrs x5, nzcv
  stp x4, x5, [x6, #C_CALL_NZCV_BEFORE]
  add sp, sp, #160
  ret
.size mmu_c_call, . - mmu_c_call
