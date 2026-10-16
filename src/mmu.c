#include <stdint.h>
#include <stdbool.h>

#include "mmu.h"

// Read the AArch64 memory-model feature register.
// ID_AA64MMFR0_EL1 describes, among other things:
//   Bits [3:0]   PARange  Physical address range
//   Bits [31:28] TGran4  4 KiB translation support
// This is a read-only architectural identification
// register, not an MMU configuration register.
static uint64_t read_mmfr0(void) {
  uint64_t value;

  __asm__ volatile(
      "mrs %0, id_aa64mmfr0_el1"
      : "=r"(value));
  return value;
}

// Collect the current EL1 translation settings.
// Reading these registers does not enable translation
// or otherwise change processor state.
void mmu_get_info(mmu_info_t *info) {
  if (!info)
    return;

  info->id_aa64mmfr0 = read_mmfr0();
  __asm__ volatile(
      "mrs %0, sctlr_el1"
      : "=r"(info->sctlr));
  __asm__ volatile(
      "mrs %0, tcr_el1"
      : "=r"(info->tcr));
  __asm__ volatile(
      "mrs %0, mair_el1"
      : "=r"(info->mair));
  __asm__ volatile(
      "mrs %0, ttbr0_el1"
      : "=r"(info->ttbr0));
  __asm__ volatile(
      "mrs %0, ttbr1_el1"
      : "=r"(info->ttbr1));
}

// SCTLR_EL1.M, bit 0, enables stage-1 translation
// for EL1 and EL0.
bool mmu_enabled(void) {
  uint64_t sctlr;

  __asm__ volatile(
      "mrs %0, sctlr_el1"
      : "=r"(sctlr));
  return (sctlr & 1U) != 0;
}

// For TGran4:
//   0x0 = 4 KiB granule supported
//   0xf = 4 KiB granule not supported
// Other encodings are reserved for this register.
bool mmu_granule_4k_supported(void) {
  uint64_t field = (read_mmfr0() >> 28) & 0xfU;
  return field == 0;
}

// Decode the PARange field.
// These are architectural encodings of the maximum
// supported physical address size.
unsigned int mmu_physical_address_bits(void) {
  static const unsigned int bits[] = {32, 36, 40, 42, 44, 48, 52};
  unsigned int parange = (unsigned int)(read_mmfr0() & 0xfU);

  if (parange >= sizeof(bits) / sizeof(bits[0]))
    return 0;

  return bits[parange];
}

