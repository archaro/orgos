// Save before using ANY GPR, including x0 and LR. Hardware has already
// masked exceptions and saved PC/status, but does not save GPRs.
// The diagnostic launcher also uses this layout for an EL1 continuation;
// it replaces the saved PC/status with its own resume state.
.macro SAVE_FRAME vector, lower=0
  sub sp, sp, #EXCEPTION_FRAME_SIZE
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
  str x30, [sp, #EXCEPTION_FRAME_X30]
  mrs x1, sp_el0
.if \lower
  mov x0, x1
.else
  add x0, sp, #EXCEPTION_FRAME_SIZE
.endif
  str x0, [sp, #EXCEPTION_FRAME_SP]
  str x1, [sp, #EXCEPTION_FRAME_SP_EL0]
  mrs x0, elr_el1
  mrs x1, spsr_el1
  stp x0, x1, [sp, #EXCEPTION_FRAME_ELR]
  mrs x0, esr_el1
  mrs x1, far_el1
  stp x0, x1, [sp, #EXCEPTION_FRAME_ESR]
  mov x0, #\vector
  str x0, [sp, #EXCEPTION_FRAME_VECTOR]
  mov x0, sp
.endm

