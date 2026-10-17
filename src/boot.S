// OrgOS - AArch64 bootstrap
// This is the first bit of code executed by the Raspberry Pi
// firmware after kernel8.img has been loaded at 0x80000.
// Our responsibilities are:
//   1. Allow only core 0 to continue
//   2. Determine the current exception level
//   3. Enter EL1 if we were started at EL2
//   4. Establish a valid stack
//   5. Clear the C .bss section
//   6. Enter main()
// We do not enable interrupts, virtual memory or caches here.
// Registers:
//   x0-x30  : 64-bit general-purpose registers
//   w0-w30  : lower 32 bits of the corresponding x registers
//   sp      : current stack pointer
//   x30     : link register (return address)
// At this stage we have no C runtime and no exception handlers.
// A serious error may therefore leave the processor halted silently.

.section ".text.boot"

.global _start
.type _start, %function

// Symbols supplied by the linker script or C code
.extern main
.extern __bss_start
.extern __bss_end
.extern __stack_top
.extern exception_vectors
.extern firmware_fdt_address

// Entry point, as specified by ENTRY(_start) in link.ld.
// We must not assume that the stack pointer is valid yet.
// Consequently, the following code uses only CPU registers.
_start:
  // On a Raspberry Pi AArch64 firmware boot, x0
  // conventionally contains the physical address
  // of the Flattened Device Tree.
  // Save it before using x0 for core identification
  // and exception-level initialization.
  // x19 is a callee-saved register and our bootstrap
  // does not otherwise use it.
  mov x19, x0

  // Identify the executing core, and suspend the others.
  // We want exactly one core executing OrgOS for now.
  // MPIDR_EL1 is the Multiprocessor Affinity Register.
  // Its low byte (Aff0) identifies the core within our cluster.
  mrs x0, mpidr_el1
  and x0, x0, #0xff
  cbz x0, check_el

park:
  // WFE = Wait For Event.
  // This is a low-power waiting instruction.
  // It may return when an event occurs, hence the loop.
  //
  // Later, when we introduce multicore operation, secondary
  // cores will need proper startup and per-core stacks.
  wfe
  b park


 // Determine the current exception level.
 // CurrentEL bits [3:2] encode the level:
 //   0x4 = EL1
 //   0x8 = EL2
 //   0xc = EL3
 // Shifting right by two gives the EL number directly.
 // Raspberry Pi firmware will normally start our AArch64
 // kernel at EL2, but we support direct entry at EL1 too.
check_el:
  mrs x0, CurrentEL
  lsr x0, x0, #2
  and x0, x0, #3

  cmp x0, #2
  b.eq from_el2

  cmp x0, #1
  b.eq at_el1

  // An unexpected exception level means that our startup
  // assumptions are wrong. Halt rather than proceeding.
  // We don't have enough environment loaded to explain
  // what has gone wrong.
halt:
  wfe
  b halt


 // Switch to EL1
 // OrgOS is an operating-system kernel, so we want EL1.
 // Configure:
 //   HCR_EL2  - environment for execution below EL2
 //   SPSR_EL2 - processor state to restore
 //   ELR_EL2  - address at which EL1 execution begins
 // ERET normally returns from an exception, but it can also
 // be used to establish our initial execution state.
from_el2:

  // HCR_EL2 - Hypervisor Configuration Register.
  // Bit 31 (RW) = 1 selects AArch64 execution for EL1.
  // Setting the remaining configurable bits to zero avoids
  // enabling hypervisor facilities that OrgOS does not use.
  mov x0, #0x80000000
  msr hcr_el2, x0

  // CNTHCTL_EL2 controls lower-level access to the ARM
  // architectural counter and timer registers.
  // Bit 0 (EL1PCTEN): allow EL1 physical counter access.
  // Bit 1 (EL1PCEN) : allow EL1 physical timer access.
  // Our current BCM2711 system timer is memory-mapped and
  // does not depend on these settings, but we may use the
  // architectural timer when implementing interrupts.
  mov x0, #3
  msr cnthctl_el2, x0

  // The architectural virtual counter uses an offset
  // relative to the physical counter.
  // Set that offset to zero for a simple initial state.
  msr cntvoff_el2, xzr

  // SCTLR_EL1 - System Control Register for EL1.
  // Establish the required reserved-one bits and leave:
  //   M = 0 : MMU disabled
  //   C = 0 : data cache disabled
  //   I = 0 : instruction cache disabled
  // 0x30d00800 is the conventional initial value for
  // this AArch64 startup configuration.
  // We will revisit SCTLR_EL1 when we implement the MMU.
  ldr x0, =0x30d00800
  msr sctlr_el1, x0

  // CPTR_EL2 controls trapping of floating-point/SIMD
  // operations to EL2.
  // Clear the trap controls. We do not require EL2 to
  // intercept those instructions.
  // This does not, by itself, enable FP/SIMD access at EL1:
  // CPACR_EL1 also controls that.
  msr cptr_el2, xzr

  // Establish the EL1 stack pointer before entering EL1.
  // SP_EL1 is a banked register: it holds EL1's own stack.
  // The linker reserves 16 KiB and supplies __stack_top.
  // ARM stacks conventionally grow downwards.
  ldr x0, =__stack_top
  msr sp_el1, x0

  // SPSR_EL2 - Saved Program Status Register.
  // Value 0x3c5:
  //   D = 1 : mask debug exceptions
  //   A = 1 : mask SError exceptions
  //   I = 1 : mask IRQ interrupts
  //   F = 1 : mask FIQ interrupts
  //   M[3:0] = 0101 : return to EL1h
  // EL1h means execution at EL1 using SP_EL1.
  // Interrupts remain masked until we install our
  // exception vectors and configure the interrupt controller.
  mov x0, #0x3c5
  msr spsr_el2, x0

  // ELR_EL2 - Exception Link Register.
  // ERET will resume execution at this address, so we
  // provide the address of our common EL1 entry point.
  // ADR calculates the address relative to the current PC.
  adr x0, at_el1
  msr elr_el2, x0

  // ISB - Instruction Synchronization Barrier.
  // Ensure preceding system-register changes are visible
  // to subsequent instruction execution before ERET.
  isb

  // Enter EL1.
  // The processor restores the state in SPSR_EL2 and
  // branches to ELR_EL2.
  // Execution continues at at_el1, no longer at EL2.
  eret


// Common EL1 entry point
// Execution arrives here by one of two routes:
//   1. Firmware started us at EL1 directly.
//   2. We entered EL1 using ERET from EL2.
// Everything below this point executes at EL1.
at_el1:

  // DAIF contains the four architectural exception masks.
  // DAIFSet #0xf sets all four masks, disabling:
  //   D - debug exceptions
  //   A - SError
  //   I - IRQ
  //   F - FIQ
  // We must not allow ordinary interrupts until VBAR_EL1
  // points to a valid exception vector table.
  msr daifset, #0xf

  // Select SP_EL1 as our active stack pointer.
  // SPSel = 1 means use SP_EL1 rather than SP_EL0.
  msr spsel, #1

  // The EL1 path may have been entered directly, without
  // going through our EL2 initialisation.
  // Initialise the stack here in both cases.
  ldr x0, =__stack_top
  mov sp, x0

  // Clear the .bss section.
  // C requires objects with static storage duration and
  // no explicit initializer to begin with zero values.
  // The firmware loads our binary but does not provide a
  // C runtime to perform that initialization for us.
  // __bss_start and __bss_end come from link.ld.
  // Our linker aligns this region to 16 bytes, allowing
  // us to clear it in 8-byte units.
  // XZR is the architectural zero register: reading it
  // produces zero, and writes to it are discarded.
  ldr x0, =__bss_start
  ldr x1, =__bss_end

clear_bss:
  cmp x0, x1
  b.hs bss_complete

  // Store eight zero bytes at x0, then increment x0 by 8.
  str xzr, [x0], #8
  b clear_bss

bss_complete:
  // Preserve the firmware Device Tree address in
  // a global variable accessible from C.
  // This must happen AFTER clearing .bss, because
  // firmware_fdt_address is stored in that section.
  ldr x0, =firmware_fdt_address
  str x19, [x0]

  // Install our EL1 exception vector table.
  //
  // VBAR_EL1 is the Vector Base Address Register.
  // The processor adds a fixed offset to this address
  // when an exception is taken.
  //
  // The base address must be 2048-byte aligned.
  //
  // LDR here loads the address of the symbol, not
  // the contents stored at that address.
  ldr x0, =exception_vectors
  msr vbar_el1, x0

  // Ensure the new vector base is effective before
  // any subsequent instruction might cause an exception.
  isb

  // Branch to main() and put the return address in x30.
  // From here onward, we execute normal C code using
  // the stack and initialized static storage.
  bl main

  // main() is not expected to return.
  // If it does, park the processor rather than executing
  // whatever happens to follow in memory.
kernel_exit:
  wfe
  b kernel_exit

.size _start, . - _start

