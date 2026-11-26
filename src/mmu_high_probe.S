#include "mmu_high_probe.h"

.section .text, "ax"
.balign 4
.global mmu_high_probe
.global mmu_high_probe_site
.global mmu_high_probe_end
.type mmu_high_probe, %function
// Self-contained leaf. No loads/stores, external calls, literals, global
// pointers or SP changes. ADR observes the actual alias used for execution.
// Private return convention to the low wrapper: x0=result, x1=PC, x2=SP.
mmu_high_probe:
  mov x0, #6
  mov x2, #7
  mul x0, x0, x2
mmu_high_probe_site:
  adr x1, mmu_high_probe_site
  mov x2, sp
  ret
mmu_high_probe_end:
.size mmu_high_probe, . - mmu_high_probe

.macro HIGH_CHECK reg, value, bit
  mov x4, #\value
  cmp \reg, x4
  cset x5, ne
  orr x3, x3, x5, lsl #\bit
.endm

.global mmu_high_call
.global mmu_high_call_return
.type mmu_high_call, %function
// x0=validated entry VA, x1=trusted low result pointer. AAPCS64 wrapper.
// The same wrapper calls the low baseline and the higher-half leaf.
mmu_high_call:
  sub sp, sp, #128
  stp x19, x20, [sp, #0]
  stp x21, x22, [sp, #16]
  stp x23, x24, [sp, #32]
  stp x25, x26, [sp, #48]
  stp x27, x28, [sp, #64]
  stp x29, x30, [sp, #80]
  stp x0, x1, [sp, #96]
  mrs x3, nzcv
  str x3, [sp, #112]
  mov x3, sp
  str x3, [x1, #HIGH_PROBE_BEFORE_SP]
  mov x3, #0xa0000000
  msr nzcv, x3
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
  blr x16
mmu_high_call_return:
  mrs x4, nzcv
  ldr x6, [sp, #104]
  stp x0, x1, [x6, #HIGH_PROBE_RESULT]
  str x2, [x6, #HIGH_PROBE_SP]
  mov x3, sp
  str x3, [x6, #HIGH_PROBE_AFTER_SP]
  adr x5, mmu_high_call_return
  str x5, [x6, #HIGH_PROBE_RETURN]
  mov x3, #0
  ldr x7, [x6, #HIGH_PROBE_BEFORE_SP]
  cmp x7, x2
  cset x5, ne
  orr x3, x3, x5, lsl #11
  mov x8, sp
  cmp x7, x8
  cset x5, ne
  orr x3, x3, x5, lsl #11
  mov x5, #0xa0000000
  cmp x4, x5
  cset x5, ne
  orr x3, x3, x5, lsl #12
  HIGH_CHECK x19, 0x719, 0
  HIGH_CHECK x20, 0x720, 1
  HIGH_CHECK x21, 0x721, 2
  HIGH_CHECK x22, 0x722, 3
  HIGH_CHECK x23, 0x723, 4
  HIGH_CHECK x24, 0x724, 5
  HIGH_CHECK x25, 0x725, 6
  HIGH_CHECK x26, 0x726, 7
  HIGH_CHECK x27, 0x727, 8
  HIGH_CHECK x28, 0x728, 9
  HIGH_CHECK x29, 0x729, 10
  str x3, [x6, #HIGH_PROBE_FAILURES]
  ldp x19, x20, [sp, #0]
  ldp x21, x22, [sp, #16]
  ldp x23, x24, [sp, #32]
  ldp x25, x26, [sp, #48]
  ldp x27, x28, [sp, #64]
  ldp x29, x30, [sp, #80]
  ldr x4, [sp, #112]
  msr nzcv, x4
  add sp, sp, #128
  ret
.size mmu_high_call, . - mmu_high_call
