// EL1-only diagnostic. Returns a mismatch bitmask (zero means PASS).
// Bits 0..30: x0..x30, 31: SP, 32: NZCV, 33: DAIF.
#include "exceptions.h"
.section ".text", "ax"
// Distinct 64-bit patterns detect truncation as well as swapped slots.
.macro SEED reg, low
  movz \reg, #\low
  movk \reg, #0x1234, lsl #16
  movk \reg, #0x5678, lsl #32
  movk \reg, #0x9abc, lsl #48
.endm

.global exception_test
.type exception_test, %function
exception_test:
  // Snapshot area 0..271; caller's callee-saved registers 272..367;
  // original NZCV/DAIF 368..383 and expected SP at 384.
  // Preserve the normal C calling convention.
  sub sp, sp, #400
  stp x19, x20, [sp, #272]
  stp x21, x22, [sp, #288]
  stp x23, x24, [sp, #304]
  stp x25, x26, [sp, #320]
  stp x27, x28, [sp, #336]
  stp x29, x30, [sp, #352]
  mrs x0, nzcv
  mrs x1, daif
  stp x0, x1, [sp, #368]
  mov x0, sp
  str x0, [sp, #384]
  mov x0, #0xa0000000
  msr nzcv, x0
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
  svc #EXCEPTION_TEST_SVC
.global exception_test_resume
exception_test_resume:
  // Store every register before using any scratch register or flags.
  stp x0, x1, [sp, #0]
  stp x2, x3, [sp, #16]
  stp x4, x5, [sp, #32]
  stp x6, x7, [sp, #48]
  stp x8, x9, [sp, #64]
  stp x10, x11, [sp, #80]
  stp x12, x13, [sp, #96]
  stp x14, x15, [sp, #112]
  stp x16, x17, [sp, #128]
  stp x18, x19, [sp, #144]
  stp x20, x21, [sp, #160]
  stp x22, x23, [sp, #176]
  stp x24, x25, [sp, #192]
  stp x26, x27, [sp, #208]
  stp x28, x29, [sp, #224]
  str x30, [sp, #240]
  mov x0, sp
  mrs x1, nzcv
  stp x0, x1, [sp, #248]
  mrs x0, daif
  str x0, [sp, #264]

  mov x0, #0
  mov x1, #0
  movz x2, #0x100
  movk x2, #0x1234, lsl #16
  movk x2, #0x5678, lsl #32
  movk x2, #0x9abc, lsl #48
  mov x4, #1
1:
  ldr x3, [sp, x1, lsl #3]
  cmp x3, x2
  csel x3, xzr, x4, eq
  orr x0, x0, x3
  add x1, x1, #1
  add x2, x2, #1
  lsl x4, x4, #1
  cmp x1, #31
  b.ne 1b

  ldr x1, [sp, #248]
  ldr x2, [sp, #384]
  cmp x1, x2
  csel x3, xzr, x4, eq
  orr x0, x0, x3
  lsl x4, x4, #1
  ldr x1, [sp, #256]
  mov x2, #0xa0000000
  cmp x1, x2
  csel x3, xzr, x4, eq
  orr x0, x0, x3
  lsl x4, x4, #1
  ldr x1, [sp, #264]
  ldr x2, [sp, #376]
  cmp x1, x2
  csel x3, xzr, x4, eq
  orr x0, x0, x3

  ldr x1, [sp, #368]
  msr nzcv, x1
  ldp x19, x20, [sp, #272]
  ldp x21, x22, [sp, #288]
  ldp x23, x24, [sp, #304]
  ldp x25, x26, [sp, #320]
  ldp x27, x28, [sp, #336]
  ldp x29, x30, [sp, #352]
  add sp, sp, #400
  ret
.size exception_test, . - exception_test
