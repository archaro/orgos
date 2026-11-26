#include "mmu_high_test.h"
#include "exceptions.h"
#include "console.h"

uint64_t mmu_high_hw_lock(void) {
  uint64_t daif;
  __asm__ volatile("mrs %0, daif\nmsr daifset, #0xf"
                   : "=r"(daif) : : "memory");
  return daif;
}
void mmu_high_hw_unlock(uint64_t daif) {
  __asm__ volatile("msr daif, %0" : : "r"(daif) : "memory");
}
uint64_t mmu_high_hw_daif(void) {
  uint64_t value;
  __asm__ volatile("mrs %0, daif" : "=r"(value));
  return value;
}
void mmu_high_hw_read(mmu_high_hw_state_t *s) {
  __asm__ volatile("mrs %0, ttbr0_el1" : "=r"(s->ttbr0));
  __asm__ volatile("mrs %0, ttbr1_el1" : "=r"(s->ttbr1));
  __asm__ volatile("mrs %0, tcr_el1" : "=r"(s->tcr));
  __asm__ volatile("mrs %0, mair_el1" : "=r"(s->mair));
  __asm__ volatile("mrs %0, sctlr_el1" : "=r"(s->sctlr));
  __asm__ volatile("mrs %0, vbar_el1" : "=r"(s->vbar));
  __asm__ volatile("mov %0, sp" : "=r"(s->sp));
  __asm__ volatile("mrs %0, sp_el0" : "=r"(s->sp_el0));
  __asm__ volatile("adr %0, ." : "=r"(s->pc));
  __asm__ volatile("mrs %0, CurrentEL" : "=r"(s->current_el));
  __asm__ volatile("mrs %0, SPSel" : "=r"(s->spsel));
}

// Arm A72 TRM 4.3.41/4.3.46 and Arm 101811, TLBs and Changing translations.
// Operation order is owned by the tested, experiment-only controller.
// No TTBR0/MAIR/SCTLR/VBAR writes and no instruction-cache invalidations.
void mmu_high_hw_operation(mmu_high_operation_t operation, uint64_t value) {
  switch (operation) {
    case MMU_HIGH_DSB:
      __asm__ volatile("dsb sy" ::: "memory"); break;
    case MMU_HIGH_ISB:
      __asm__ volatile("isb" ::: "memory"); break;
    case MMU_HIGH_TLBI:
      __asm__ volatile("tlbi vmalle1" ::: "memory"); break;
    case MMU_HIGH_WRITE_TTBR1:
      __asm__ volatile("msr ttbr1_el1, %0" : : "r"(value) : "memory"); break;
    case MMU_HIGH_WRITE_TCR:
      __asm__ volatile("msr tcr_el1, %0" : : "r"(value) : "memory"); break;
    default: mmu_high_hw_fatal("Invalid TTBR1 experiment operation");
  }
}

bool mmu_high_hw_translate(vaddr_t address, bool write,
                           mmu_translation_t *translation) {
  return write ? mmu_translate_write(address, translation) :
      mmu_translate(address, translation);
}

void mmu_high_hw_fatal(const char *reason) {
  // These are kernel invariant failures, never recoverable EL0 traps.
  console_printf("TTBR1 experiment fatal: %s\n", reason);
  uint64_t pc;
  __asm__ volatile("adr %0, ." : "=r"(pc));
  exception_panic(4, 0, pc, 0, 5);
}
