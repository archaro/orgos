// The kernel wrapper and EL0 payload deliberately occupy different pages.
#include "el0_test.h"
#include "exceptions.h"
#include "exception_frame.inc"

.section ".text", "ax"
.global el0_test_enter
.type el0_test_enter, %function
el0_test_enter:
  // Keep a complete EL1 continuation on its private stack. EL0 traps
  // allocate their frames below this; neither stack is switched in C.
  SAVE_FRAME 4
  adr x0, el0_test_kernel_resume
  str x0, [sp, #EXCEPTION_FRAME_ELR]
  mrs x0, nzcv
  mrs x1, daif
  orr x0, x0, x1
  mov x1, #5
  orr x0, x0, x1 // EL1h continuation, original masks and flags.
  str x0, [sp, #EXCEPTION_FRAME_SPSR]
  mov x0, sp
  bl el0_test_begin

  // Protect ELR/SPSR setup: an IRQ here would overwrite the pending
  // exception-return state. ERET restores the original masks at EL0.
  msr daifset, #0xf
  ldr x0, =__el0_stack_top
  msr sp_el0, x0
  ldr x0, =el0_test_entry
  msr elr_el1, x0
  // Preserve incoming DAIF masks while running the bounded EL0 payload.
  // IRQs may interrupt EL0 through vector 9 and return normally.
  ldr x0, [sp, #EXCEPTION_FRAME_SPSR]
  and x0, x0, #0x3c0
  msr spsr_el1, x0 // EL0t, AArch64, initially clear NZCV.
  eret

// The final SVC replaces its frame with the saved EL1 continuation.
el0_test_kernel_resume:
  // Snapshot the resumed kernel GPRs before using scratch registers. The
  // expected continuation is still immediately above this new frame.
  SAVE_FRAME 4
  mrs x1, nzcv
  mrs x2, daif
  orr x1, x1, x2
  str x1, [sp, #EXCEPTION_FRAME_SPSR]
  mov x0, #0 // Return a kernel-context mismatch mask to C.
  mov x1, #0
  add x5, sp, #EXCEPTION_FRAME_SIZE
  mov x4, #1
1:
  ldr x2, [sp, x1, lsl #3]
  ldr x3, [x5, x1, lsl #3]
  cmp x2, x3
  csel x2, xzr, x4, eq
  orr x0, x0, x2
  add x1, x1, #1
  lsl x4, x4, #1
  cmp x1, #31
  b.ne 1b
  ldr x1, [sp, #EXCEPTION_FRAME_SP]
  ldr x2, [x5, #EXCEPTION_FRAME_SP]
  sub x2, x2, #EXCEPTION_FRAME_SIZE
  cmp x1, x2
  csel x3, xzr, x4, eq
  orr x0, x0, x3
  lsl x4, x4, #1
  ldr x1, [sp, #EXCEPTION_FRAME_SPSR]
  ldr x2, [x5, #EXCEPTION_FRAME_SPSR]
  eor x1, x1, x2
  tst x1, #0xf0000000
  csel x3, xzr, x4, eq
  orr x0, x0, x3
  lsl x4, x4, #1
  tst x1, #0x3c0
  csel x3, xzr, x4, eq
  orr x0, x0, x3
  lsl x4, x4, #1
  ldr x1, [sp, #EXCEPTION_FRAME_SP_EL0]
  ldr x2, [x5, #EXCEPTION_FRAME_SP_EL0]
  cmp x1, x2
  csel x3, xzr, x4, eq
  orr x0, x0, x3
  add sp, sp, #EXCEPTION_FRAME_SIZE
  // Return the diagnostic status using the ordinary C convention.
  ldp x19, x20, [sp, #152]
  ldp x21, x22, [sp, #168]
  ldp x23, x24, [sp, #184]
  ldp x25, x26, [sp, #200]
  ldp x27, x28, [sp, #216]
  ldp x29, x30, [sp, #232]
  ldr x1, [sp, #EXCEPTION_FRAME_SPSR]
  msr nzcv, x1
  add sp, sp, #EXCEPTION_FRAME_SIZE
  ret
.size el0_test_enter, . - el0_test_enter
.ltorg

.section ".el0.text", "ax"
.macro SEED reg, low
  movz \reg, #\low
  movk \reg, #0x1234, lsl #16
  movk \reg, #0x5678, lsl #32
  movk \reg, #0x9abc, lsl #48
.endm

.global el0_test_entry
.type el0_test_entry, %function
el0_test_entry:
  // No kernel data references, literal pools or C runtime dependencies.
  SEED x0, 256
  SEED x1, 257
  SEED x2, 258
  SEED x3, 259
  SEED x4, 260
  SEED x5, 261
  SEED x6, 262
  SEED x7, 263
  SEED x8, 264
  SEED x9, 265
  SEED x10, 266
  SEED x11, 267
  SEED x12, 268
  SEED x13, 269
  SEED x14, 270
  SEED x15, 271
  SEED x16, 272
  SEED x17, 273
  SEED x18, 274
  SEED x19, 275
  SEED x20, 276
  SEED x21, 277
  SEED x22, 278
  SEED x23, 279
  SEED x24, 280
  SEED x25, 281
  SEED x26, 282
  SEED x27, 283
  SEED x28, 284
  SEED x29, 285
  SEED x30, 286
  sub sp, sp, #16
  mov x0, #0xa0000000
  msr nzcv, x0 // NZCV access is permitted at EL0.
  mov x0, #21
  add x0, x0, x0
  stp x0, x1, [sp]
  mov x0, xzr
  ldr x0, [sp] // Prove that the dedicated EL0 stack is accessible.
  svc #EL0_TEST_CHECK_SVC
.global el0_test_check_resume
el0_test_check_resume:
.global el0_test_privileged
el0_test_privileged:
  // This EL1 register is UNDEFINED at EL0 (EC=0), not a syscall.
  mrs x9, sctlr_el1
.global el0_test_fault_resume
el0_test_fault_resume:
  svc #EL0_TEST_EXIT_SVC
.global el0_test_exit_resume
el0_test_exit_resume:
  brk #0 // A successful exit never returns here at EL0.
.size el0_test_entry, . - el0_test_entry
