#include "console.h"
#include "console_args.h"
#include "console_commands.h"
#include "mmu.h"
#include "mmu_high.h"

extern char __text_start[], __rodata_start[], __data_start[], __bss_start[];
extern char __stack_bottom[], exception_vectors[];

static void hex_value(const char *label, uint64_t value) {
  console_printf("  %s: 0x", label);
  console_puthex64(value);
  console_putc('\n');
}

static bool sample(const mmu_address_space_t *space, const char *name,
                   paddr_t physical, unsigned int ap, bool pxn,
                   unsigned int attribute) {
  vaddr_t va;
  mmu_descriptor_info_t info;
  if (!mmu_high_address(physical, &va) ||
      !mmu_space_inspect(space, va, &info) ||
      info.kind == MMU_DESC_UNMAPPED || info.physical_address != physical)
    return false;
  console_printf("  %s\n", name);
  hex_value("VA", va);
  hex_value("PA", info.physical_address);
  console_printf("    %s AP=%u Attr=%u SH=%u PXN=%u UXN=%u AF=%u\n",
      info.kind == MMU_DESC_PAGE ? "PAGE" : "BLOCK",
      info.access_permissions, info.attr_index,
      (unsigned int)((info.leaf_descriptor >> 8) & 3U),
      info.pxn, info.uxn, info.access_flag);
  return info.access_permissions == ap && info.pxn == pxn && info.uxn &&
      info.access_flag && info.attr_index == attribute &&
      ((info.leaf_descriptor >> 8) & 3U) == (attribute == 0U ? 0U : 3U);
}

void cmd_kmap(const char *args) {
  if (!console_arg_end(args)) {
    console_puts("Usage: kmap\n");
    return;
  }
  const mmu_address_space_t *space = mmu_high_address_space();
  if (!space || space->state != MMU_SPACE_SEALED ||
      space->geometry.region != MMU_REGION_TTBR1) {
    console_puts("Inactive kernel hierarchy unavailable.\n");
    return;
  }
  mmu_info_t before, after;
  mmu_get_info(&before);
  console_puts("Inactive TTBR1 kernel hierarchy (software inspection only)\n");
  console_puts("  State: SEALED; not activated\n");
  console_puts("  Geometry: upper VA=39 PA=36 granule=4KiB start=L1\n");
  hex_value("Higher-half base", MMU_TTBR1_BASE);
  hex_value("Proposed root PA", space->root_physical);
  hex_value("Proposed TCR (not live)", space->tcr);
  hex_value("Live TCR_EL1", before.tcr);
  hex_value("Live TTBR0_EL1", before.ttbr0);
  hex_value("Live TTBR1_EL1", before.ttbr1);
  console_printf("  Live TTBR1 walks: %s\n",
      (before.tcr & (1ULL << 23)) ? "DISABLED" : "ENABLED");
  bool ok = true;
  ok &= sample(space, "Kernel text", (uintptr_t)__text_start, 2, false, 2);
  ok &= sample(space, "Exception vectors", (uintptr_t)exception_vectors,
               2, false, 2);
  ok &= sample(space, "Read-only data", (uintptr_t)__rodata_start, 2, true, 2);
  ok &= sample(space, "Writable data", (uintptr_t)__data_start, 0, true, 2);
  ok &= sample(space, "BSS", (uintptr_t)__bss_start, 0, true, 2);
  ok &= sample(space, "Kernel stack", (uintptr_t)__stack_bottom, 0, true, 2);
  ok &= sample(space, "Table storage", space->root_physical, 0, true, 2);
  ok &= sample(space, "Peripheral MMIO", 0xfd500000ULL, 0, true, 0);
  ok &= sample(space, "PCIe MMIO", 0x600000000ULL, 0, true, 0);
  mmu_get_info(&after);
  bool unchanged = before.ttbr0 == after.ttbr0 &&
      before.ttbr1 == after.ttbr1 &&
      before.tcr == after.tcr && before.mair == after.mair &&
      before.sctlr == after.sctlr;
  console_printf("  Samples: %s\n", ok ? "PASS" : "FAIL");
  console_printf("  Live regime unchanged: %s\n", unchanged ? "YES" : "NO");
}
