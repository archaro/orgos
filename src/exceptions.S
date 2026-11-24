// OrgOS - AArch64 exception vectors, 16 slots of 128 bytes.
#include "exceptions.h"

.section ".text.exceptions", "ax"
.extern exception_panic
.extern exception_dispatch
.extern irq_dispatch
.global exception_vectors

// Fatal vectors retain the existing diagnostic path.
.macro PANIC_VECTOR number
  mov x0, #\number
  b exception_entry
  .space 0x80 - 8
.endm

.macro RETURN_VECTOR target
  b \target
  .space 0x80 - 4
.endm

.balign 2048
exception_vectors:
  // Current EL, SP_EL0: unsupported.
  PANIC_VECTOR 0
  PANIC_VECTOR 1
  PANIC_VECTOR 2
  PANIC_VECTOR 3
  // Current EL, SP_EL1: synchronous and IRQ can return.
  RETURN_VECTOR sync_entry
  RETURN_VECTOR irq_entry
  PANIC_VECTOR 6
  PANIC_VECTOR 7
  // Lower EL AArch64: synchronous traps and IRQs use the same frame.
  RETURN_VECTOR lower_sync_entry
  RETURN_VECTOR lower_irq_entry
  PANIC_VECTOR 10
  PANIC_VECTOR 11
  PANIC_VECTOR 12
  PANIC_VECTOR 13
  PANIC_VECTOR 14
  PANIC_VECTOR 15

exception_entry:
  mrs x1, esr_el1
  mrs x2, elr_el1
  mrs x3, far_el1
  mrs x4, spsr_el1
  bl exception_panic
1:
  wfe
  b 1b

#include "exception_frame.inc"

sync_entry:
  SAVE_FRAME 4
  bl exception_dispatch
  b exception_return

irq_entry:
  SAVE_FRAME 5
  bl irq_dispatch
  b exception_return

lower_sync_entry:
  SAVE_FRAME 8, 1
  bl exception_dispatch
  b exception_return

lower_irq_entry:
  SAVE_FRAME 9, 1
  bl irq_dispatch

// Recoverable paths restore PC/status from their own frame, not relying
// on system registers surviving the C handler. Do not unmask in handlers.
exception_return:
  ldr x0, [sp, #EXCEPTION_FRAME_SP_EL0]
  msr sp_el0, x0
  ldp x0, x1, [sp, #EXCEPTION_FRAME_ELR]
  msr elr_el1, x0
  msr spsr_el1, x1
  ldp x0, x1, [sp, #0]
  ldp x2, x3, [sp, #16]
  ldp x4, x5, [sp, #32]
  ldp x6, x7, [sp, #48]
  ldp x8, x9, [sp, #64]
  ldp x10, x11, [sp, #80]
  ldp x12, x13, [sp, #96]
  ldp x14, x15, [sp, #112]
  ldp x16, x17, [sp, #128]
  ldp x18, x19, [sp, #144]
  ldp x20, x21, [sp, #160]
  ldp x22, x23, [sp, #176]
  ldp x24, x25, [sp, #192]
  ldp x26, x27, [sp, #208]
  ldp x28, x29, [sp, #224]
  ldr x30, [sp, #EXCEPTION_FRAME_X30]
  add sp, sp, #EXCEPTION_FRAME_SIZE
  eret
