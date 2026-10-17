
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

  // Read the existing EL1 system control.
  mrs x3, sctlr_el1

  // Enable stage-1 address translation.
  // SCTLR_EL1.M = bit 0.
  orr x3, x3, #1

  // Enable Write eXecute Never.
  // SCTLR_EL1.WXN = bit 19.
  //
  // This prevents execution through writable
  // stage-1 mappings, providing an additional
  // safeguard against accidental RWX pages.
  //
  // Kernel .text is mapped read-only, so it
  // remains executable.
  mov x4, #0x80000
  orr x3, x3, x4

  // Commit both settings together, then
  // synchronize instruction execution.
  msr sctlr_el1, x3
  isb

  ret

.size mmu_enable_el1, . - mmu_enable_el1

