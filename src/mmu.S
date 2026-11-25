
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

  // OrgOS currently uses only CPU core 0.
  //
  // IC IALLUIS was tested on the Raspberry Pi 4B
  // and caused interrupt-driven UART reception to
  // stop working, although polled transmission
  // continued normally.
  //
  // IC IALLU works correctly in this configuration.
  // The cause of the IALLUIS behaviour remains
  // undetermined; revisit during multicore bring-up.
  ic iallu

  // Complete instruction-cache maintenance
  // before changing SCTLR_EL1.
  dsb ish
  isb

  // Read the existing EL1 system control.
  mrs x3, sctlr_el1

  // M: bit 0 - enable stage-1 translation.
  orr x3, x3, #1

  // WXN: bit 19 - prohibit execution through
  // writable mappings.
  mov x4, #0x80000
  orr x3, x3, x4

  // I: bit 12 - enable instruction caching.
  //
  // This does not enable the data cache.
  mov x4, #0x1000
  orr x3, x3, x4

  // C: bit 2 - enable data and unified caches.
  //
  // All ordinary RAM mappings now use Normal
  // write-back, write-allocate attributes.
  //
  // Peripheral and PCIe MMIO mappings remain
  // Device-nGnRnE and are not cacheable.
  orr x3, x3, #4

  // Activate translation, WXN and I-cache.
  msr sctlr_el1, x3

  // Ensure subsequent instruction fetches
  // observe the updated control settings.
  isb

  ret

.size mmu_enable_el1, . - mmu_enable_el1


// Private live-root replacement for the admitted, compatible hierarchies.
// Preconditions: EL1h, DAIF masked by activation controller, MMU enabled;
// identical EL1 physical outputs/cacheability/execution in old/new roots.
// All tables are complete and immutable. No MAIR/TCR/SCTLR change.
// Arm 101811, TLBs and Changing translations:
// https://developer.arm.com/documentation/101811/latest
// Publish table stores, synchronise the root before invalidation, then
// complete local EL1/EL0 invalidation (including global translations).
// Only core 0 runs; no ASIDs, broadcast or I-cache maintenance is needed.
.global mmu_replace_ttbr0
.type mmu_replace_ttbr0, %function
mmu_replace_ttbr0:
  dsb sy
  msr ttbr0_el1, x0
  isb
  tlbi vmalle1
  dsb sy
  isb
  ret
.size mmu_replace_ttbr0, . - mmu_replace_ttbr0
