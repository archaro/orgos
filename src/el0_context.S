#include "el0_context.h"
#include "exceptions.h"
#include "exception_frame.inc"

.section ".text", "ax"
.global el0_context_switch
.type el0_context_switch, %function
el0_context_switch:
  SAVE_FRAME 4
  ldr x0, =el0_kernel_context_return
  str x0, [sp, #EXCEPTION_FRAME_ELR]
  mrs x0, nzcv
  mrs x1, daif
  orr x0, x0, x1
  mov x1, #5
  orr x0, x0, x1
  str x0, [sp, #EXCEPTION_FRAME_SPSR]
  mov x0, sp
  bl el0_context_enter

  // Protect pending ELR/SPSR against IRQ entry until ERET restores DAIF.
  msr daifset, #0xf
  mov x1, x0
  sub sp, sp, #EXCEPTION_FRAME_SIZE
  mov x2, sp
  mov x5, #(EXCEPTION_FRAME_SIZE / 16)
1:
  ldp x3, x4, [x1], #16
  stp x3, x4, [x2], #16
  subs x5, x5, #1
  b.ne 1b
  // The common return pops this temporary frame, leaving the trusted
  // EL1 continuation on SP_EL1 until the next yield/exit SVC returns it.
  b exception_frame_return
.size el0_context_switch, . - el0_context_switch
.ltorg

.section ".el0.text", "ax"
.global el0_context_program
.type el0_context_program, %function
el0_context_program:
  sub sp, sp, #EL0_CONTEXT_STACK_BYTES
  stp x0, x2, [sp, #EL0_CONTEXT_PROGRESS_OFFSET]
  str x3, [sp, #EL0_CONTEXT_PATTERN_OFFSET]
  str xzr, [sp, #EL0_CONTEXT_VARIANT_OFFSET]
  b el0_context_step

.global el0_context_fault_program
el0_context_fault_program:
  sub sp, sp, #EL0_CONTEXT_STACK_BYTES
  stp x0, x2, [sp, #EL0_CONTEXT_PROGRESS_OFFSET]
  str x3, [sp, #EL0_CONTEXT_PATTERN_OFFSET]
  mov x3, #1
  str x3, [sp, #EL0_CONTEXT_VARIANT_OFFSET]
  ldr x3, [sp, #EL0_CONTEXT_PATTERN_OFFSET]
  b el0_context_step

el0_context_step:
  ldr x0, [sp, #EL0_CONTEXT_PROGRESS_OFFSET]
  add x0, x0, #7
  str x0, [sp, #EL0_CONTEXT_PROGRESS_OFFSET]
  add x1, x1, #1
  cmp x1, #EL0_CONTEXT_SLICES
  b.eq el0_context_exit
  mov x3, #0xa0000000
  msr nzcv, x3
  ldr x3, [sp, #EL0_CONTEXT_PATTERN_OFFSET]
  svc #EL0_CONTEXT_YIELD_SVC
.global el0_context_program_resume
el0_context_program_resume:
  // Snapshot EVERY resumed GPR and NZCV before using scratch registers.
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
  mrs x3, nzcv
  str x3, [sp, #248]
  mov x3, sp
  str x3, [sp, #256]
  ldr x3, [sp, #EL0_CONTEXT_VARIANT_OFFSET]
  cbnz x3, el0_context_fault_access
  ldr x3, [sp, #24]
  b el0_context_step

el0_context_fault_access:
  adrp x3, el0_test_kernel_canary
  add x3, x3, :lo12:el0_test_kernel_canary
.global el0_context_fault_read
el0_context_fault_read:
  ldr x3, [x3]
  // If the forbidden read unexpectedly succeeds, exit without reporting
  // a contained fault. The fault diagnostic must fail, not loop or hang.
  b el0_context_exit

el0_context_exit:
  mov x3, #0xa0000000
  msr nzcv, x3
  ldr x3, [sp, #EL0_CONTEXT_PATTERN_OFFSET]
  svc #EL0_CONTEXT_EXIT_SVC
.global el0_context_exit_resume
el0_context_exit_resume:
  brk #0
.global el0_context_program_end
el0_context_program_end:
.size el0_context_program, . - el0_context_program
