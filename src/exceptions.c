// OrgOS - Exception Handlers

#include <stdint.h>

#include "console.h"

// Print a 64-bit value as sixteen hexadecimal digits.
// Fixed-width output is useful when examining addresses
// and architectural registers.
static void puthex64(uint64_t value) {
  static const char digits[] = "0123456789abcdef";

  console_puts("0x");

  for (int shift = 60; shift >= 0; shift -= 4)
    console_putc(digits[(value >> shift) & 0xf]);
}

// Display a short description of the exception class.
// ESR_EL1 bits [31:26] contain the Exception Class (EC).
static const char *exception_description(uint32_t ec) {
  switch (ec) {
    case 0x00: return "Unknown exception";
    case 0x15: return "SVC instruction";
    case 0x20: return "Instruction abort from lower EL";
    case 0x21: return "Instruction abort from current EL";
    case 0x24: return "Data abort from lower EL";
    case 0x25: return "Data abort from current EL";
    case 0x2f: return "SError";
    case 0x3c: return "Breakpoint instruction (BRK)";
    default:   return "Unclassified exception";
  }
}

// Fatal exception handler.
// Called from exceptions.S.
// All exceptions are fatal until we implement
// proper interrupt dispatch and recovery.
__attribute__((noreturn))
void exception_panic(uint64_t vector, uint64_t esr, uint64_t elr, uint64_t far, uint64_t spsr) {
  uint32_t ec = (uint32_t)((esr >> 26) & 0x3f);

  console_puts("\n\n");
  console_puts("********************************\n");
  console_puts("*       OrgOS KERNEL PANIC     *\n");
  console_puts("********************************\n\n");

  console_puts("Exception vector : ");
  console_putu64(vector);
  console_putc('\n');

  console_puts("Exception class  : ");
  puthex64(ec);
  console_puts(" (");
  console_puts(exception_description(ec));
  console_puts(")\n");

  console_puts("ESR_EL1          : ");
  puthex64(esr);
  console_putc('\n');

  console_puts("ELR_EL1          : ");
  puthex64(elr);
  console_putc('\n');

  console_puts("FAR_EL1          : ");
  puthex64(far);
  console_puts(" (not valid for all exceptions)\n");

  console_puts("SPSR_EL1         : ");
  puthex64(spsr);
  console_putc('\n');

  console_puts("\nSystem halted.\n");

  // Do not return from an unexpected exception.
  // Mask all exception classes and remain halted.
  __asm__ volatile(
    "msr daifset, #0xf\n"
    "dsb sy\n"
    "1: wfe\n"
    "b 1b\n"
    ::: "memory"
  );

  __builtin_unreachable();
}

