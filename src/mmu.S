
// OrgOS - Enable AArch64 EL1 stage-1 translation.
// Arguments:
//   x0 = physical address of L1 translation table
//   x1 = MAIR_EL1 value
//   x2 = TCR_EL1 value
// Preconditions:
//   - Running at EL1 with SP_EL1
//   - MMU currently disabled
//   - IRQs masked
//   - Translation tables initialized in RAM
//   - Current PC, stack and exception vectors
//     are all identity-mapped
// The routine preserves the normal C calling
// convention. It does not enable caches.

.section ".text.mmu", "ax"

.global mmu_enable_el1
.type mmu_enable_el1, %function

mmu_enable_el1:

  // Ensure all translation-table stores made by C
  // are globally visible before table walks begin.
  dsb sy

  // MAIR_EL1 defines the memory attributes referred
  // to by AttrIndx fields in translation descriptors.
  msr mair_el1, x1

  // Set the translation control:
  // address size, granule, shareability and IPS.
  msr tcr_el1, x2

  // TTBR0_EL1 contains the physical address of our
  // level-1 translation table.
  // TTBR1 translation walks are disabled by TCR.
  msr ttbr0_el1, x0

  // Ensure the new translation configuration is
  // recognized before invalidating old translations.
  isb

  // Discard any stale EL1 translation entries.
  // At this point we have no useful existing
  // translations to preserve.
  tlbi vmalle1
  dsb sy
  isb

  // Read SCTLR_EL1 and enable its M bit.
  // M = 1 enables stage-1 address translation.
  // Do not set C or I: both caches remain disabled.
  // Preserve the remaining SCTLR configuration.
  mrs x3, sctlr_el1
  orr x3, x3, #1
  msr sctlr_el1, x3

  // After this synchronization barrier, subsequent
  // instruction fetches and memory accesses use
  // the newly installed translation regime.
  // Our identity mappings ensure execution can
  // continue at the same numerical PC.
  isb

  ret

.size mmu_enable_el1, . - mmu_enable_el1

