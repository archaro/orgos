// OrgOS - AArch64 exception vectors
// Each vector occupies 128 bytes.
// The complete table occupies 2048 bytes and must
// be aligned to a 2048-byte boundary.
// Exception vectors 0-15 correspond to:
//  0-3   Current EL, SP_EL0
//  4-7   Current EL, SP_EL1
//  8-11  Lower EL, AArch64
// 12-15  Lower EL, AArch32
// Within each group:
//   0 = synchronous
//   1 = IRQ
//   2 = FIQ
//   3 = SError
// All exceptions except the EL1 IRQ are fatal.

.section ".text.exceptions", "ax"

.global exception_vectors
.extern exception_panic
.extern irq_dispatch

// Generate a fatal exception vector.
// We pass its vector number to the common
// exception diagnostic handler.
.macro PANIC_VECTOR number
  mov x0, #\number
  b exception_entry
  .space 0x80 - 8
.endm

// Generate the recoverable IRQ vector.
// Branch immediately to our register-saving
// IRQ handler.
.macro IRQ_VECTOR
  b irq_entry
  .space 0x80 - 4
.endm

.balign 2048

exception_vectors:

  // Current EL, SP_EL0
  PANIC_VECTOR 0
  PANIC_VECTOR 1
  PANIC_VECTOR 2
  PANIC_VECTOR 3

  // Current EL, SP_EL1
  PANIC_VECTOR 4
  IRQ_VECTOR
  PANIC_VECTOR 6
  PANIC_VECTOR 7

  // Lower EL, AArch64
  PANIC_VECTOR 8
  PANIC_VECTOR 9
  PANIC_VECTOR 10
  PANIC_VECTOR 11

  // Lower EL, AArch32
  PANIC_VECTOR 12
  PANIC_VECTOR 13
  PANIC_VECTOR 14
  PANIC_VECTOR 15


// Fatal exception entry.
// Since the kernel will halt, we do not need to
// preserve the interrupted general-purpose registers.
exception_entry:
  mrs x1, esr_el1
  mrs x2, elr_el1
  mrs x3, far_el1
  mrs x4, spsr_el1

  bl exception_panic

1:
  wfe
  b 1b


// Recoverable IRQ entry.
// On exception entry, the processor automatically:
//   - Saves the interrupted PC in ELR_EL1.
//   - Saves processor status in SPSR_EL1.
//   - Masks further IRQs.
//   - Enters our vector at VBAR_EL1 + 0x280.
// It does NOT save x0-x30 for us.
// We must save every general-purpose register that
// the interrupted code could have been using.
// Allocate 256 bytes on the EL1 stack.
// This maintains the required 16-byte stack alignment.
irq_entry:
  sub sp, sp, #256

  // Save x0-x29 in pairs.
  stp x0,  x1,  [sp, #0]
  stp x2,  x3,  [sp, #16]
  stp x4,  x5,  [sp, #32]
  stp x6,  x7,  [sp, #48]
  stp x8,  x9,  [sp, #64]
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

  // x30 is the link register.
  str x30, [sp, #240]

  // All general-purpose registers are now safe.
  // Call the C interrupt dispatcher.
  // For this initial implementation IRQ code must not
  // use FP/SIMD registers, because we do not save them.
  bl irq_dispatch

  // Restore the interrupted register state.
  ldp x0,  x1,  [sp, #0]
  ldp x2,  x3,  [sp, #16]
  ldp x4,  x5,  [sp, #32]
  ldp x6,  x7,  [sp, #48]
  ldp x8,  x9,  [sp, #64]
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

  ldr x30, [sp, #240]

  add sp, sp, #256

  // ERET restores the PC and processor state from
  // ELR_EL1 and SPSR_EL1.
  // Execution resumes at the interrupted instruction
  // stream, not at the IRQ handler.
  eret

