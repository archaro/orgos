#include "irq.h"
#include "console.h"

void irq_unexpected(uint32_t id, uint32_t token) {
  // Preserve the existing unexpected-source diagnostic and masked halt.
  __asm__ volatile("msr daifset, #0xf" ::: "memory");
  console_puts("\nUnexpected IRQ ");
  console_putu32(id);
  console_puts(" (IAR token 0x");
  console_puthex(token, 8U);
  console_puts("). System halted.\n");
  __asm__ volatile("dsb sy" ::: "memory");
  for (;;) {
    __asm__ volatile("wfe");
  }
}
