#include "mmu_c_island.h"

// No global references or external calls. Only this translation unit has
// sibling-call/jump-table suppression; the kernel code model is unchanged.
#define ISLAND __attribute__((section(".h2_c.text"), noipa, used, \
                              visibility("hidden")))
#ifdef __aarch64__
#define OBSERVE(index, name, slot) do { \
  uint64_t pc, sp, lr; \
  __asm__ volatile(".global " #name "\n" #name ":\n" \
      "adr %0, " #name "\nmov %1, sp\nmov %2, x30" \
      : "=r"(pc), "=r"(sp), "=r"(lr)); \
  e->pc[index] = pc; e->sp[index] = sp; \
  if (index) e->caller[(index) - 1] = lr; \
  e->local[index] = (uintptr_t)&slot; \
} while (0)
#else
// Native arithmetic tests cannot observe AArch64 execution evidence.
#define OBSERVE(index, name, slot) do { \
  e->pc[index] = 0; e->sp[index] = 0; \
  e->local[index] = (uintptr_t)&slot; \
} while (0)
#endif

ISLAND uint64_t mmu_c_leaf(mmu_c_evidence_t *e, uint64_t a,
                                  uint64_t b) {
  volatile uint64_t product = a * b;
  OBSERVE(2, mmu_c_leaf_site, product);
  e->local_value[2] = product;
  return product;
}
ISLAND uint64_t mmu_c_middle(mmu_c_evidence_t *e, uint64_t a,
                                    const volatile uint64_t *ro) {
  volatile uint64_t factor = ro[2];
  OBSERVE(1, mmu_c_middle_site, factor);
  e->ro_value[1] = factor;
  uint64_t result = mmu_c_leaf(e, a, factor);
  e->local_value[1] = factor;
  return result;
}
ISLAND uint64_t mmu_c_outer(mmu_c_evidence_t *e,
    const volatile uint64_t *ro, volatile mmu_c_storage_t *rw) {
  volatile uint64_t operand = ro[1];
  OBSERVE(0, mmu_c_outer_site, operand);
  e->ro_address = (uintptr_t)&ro[1];
  e->rw_address = (uintptr_t)&rw->value;
  e->ro_value[0] = operand;
  uint64_t result = mmu_c_middle(e, operand, ro);
  e->local_value[0] = operand;
  rw->value = result;
  e->written = rw->value;
  return result;
}
