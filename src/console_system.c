#include <stddef.h>
#include <stdarg.h>

#include "format.h"
#include "console.h"
#include "console_args.h"
#include "board.h"
#include "mailbox.h"
#include "timer.h"
#include "uart.h"
#include "fdt.h"
#include "physmem.h"
#include "irq.h"
#include "mmu.h"
#include "page.h"
#include "cache.h"
#include "dma.h"
#include "pcie.h"
#include "xhci.h"
#include "usb.h"
#include "usb_hub.h"
#include "usb_enumerate.h"
#include "usb_midi.h"
#include "usb_midi_rx.h"
#include "midi_output.h"
#include "usb_midi_test.h"

#include "console_commands.h"

static void cmd_fdt_dma(void) {
  fdt_pcie_dma_t info;

  if (!fdt_get_pcie_dma(&info)) {
    console_puts("PCIe DMA properties unavailable.\n");
    return;
  }

  console_puts("BCM2711 PCIe Device Tree DMA properties:\n");
  console_puts("  Child address cells  : ");
  console_putu32(info.child_address_cells);
  console_putc('\n');
  console_puts("  Parent address cells : ");
  console_putu32(info.parent_address_cells);
  console_putc('\n');
  console_puts("  Size cells           : ");
  console_putu32(info.size_cells);
  console_putc('\n');
  console_puts("\n  dma-ranges:\n");
  for (unsigned int i = 0; i < info.dma_cell_count; i++) {
    console_puts("    Cell ");
    console_putu32(i);
    console_puts(": 0x");
    console_puthex(info.dma_cells[i], 8);
    console_putc('\n');
  }
  console_puts("\n  brcm,scb-sizes:\n");
  if (info.scb_cell_count == 0)
    console_puts("    Not present\n");
  for (unsigned int i = 0; i < info.scb_cell_count; i++) {
    console_puts("    Cell ");
    console_putu32(i);
    console_puts(": 0x");
    console_puthex(info.scb_cells[i], 8);
    console_putc('\n');
  }
}

static void cmd_fdt_memory(void) {
  // Static storage avoids placing the entire
  // discovery report on our 16 KiB kernel stack.
  // The console is currently single-threaded.
  static fdt_memory_map_t map;

  if (!fdt_get_memory_map(&map)) {
    console_puts("FDT memory-map parsing failed.\n");
    return;
  }

  console_puts("Firmware Device Tree memory map:\n");
  console_puts("  Root address cells : ");
  console_putu32(map.address_cells);
  console_putc('\n');
  console_puts("  Root size cells    : ");
  console_putu32(map.size_cells);
  console_putc('\n');
  console_puts("\nMemory banks:\n");

  uint64_t total_ram = 0;
  if (map.bank_count == 0)
    console_puts("  None described.\n");

  for (unsigned int i = 0; i < map.bank_count; i++) {
    const fdt_memory_range_t *range = &map.banks[i];
    console_puts("  Bank ");
    console_putu32(i);
    console_puts(":\n");
    console_puts("    Base : 0x");
    console_puthex64(range->address);
    console_putc('\n');
    console_puts("    Size : ");
    console_putu64(range->size / (1024ULL * 1024ULL));
    console_puts(" MiB\n");
    total_ram += range->size;
  }
  console_puts("  Total described RAM: ");
  console_putu64(total_ram / (1024ULL * 1024ULL));
  console_puts(" MiB\n");
  console_puts("\nReserved regions:\n");
  if (map.reservation_count == 0)
    console_puts("  None explicitly described.\n");
  for (unsigned int i = 0;
       i < map.reservation_count; i++) {
    const fdt_reservation_t *entry =
        &map.reservations[i];
    console_puts("  Reservation ");
    console_putu32(i);
    console_puts(entry->kind == FDT_RESERVE_HEADER ?
        " (header):\n" : " (/reserved-memory):\n");
    console_puts("    Base : 0x");
    console_puthex64(entry->range.address);
    console_putc('\n');
    console_puts("    Size : ");
    console_putu64(entry->range.size);
    console_puts(" bytes\n");
  }
  console_puts(
      "\nNote: These are firmware-described ranges,\n"
      "not an allocation-ready free-memory map.\n");
}

void cmd_fdt(const char *args) {
  if (console_arg_word(&args, "dma") && console_arg_end(args)) {
    cmd_fdt_dma();
    return;
  }
  if (console_arg_word(&args, "memory") && console_arg_end(args)) {
    cmd_fdt_memory();
    return;
  }

  if (!console_arg_end(args)) {
    console_puts("Usage: fdt [dma | memory]\n");
    return;
  }

  fdt_info_t info;
  console_puts("Firmware Flattened Device Tree:\n");
  console_puts("  Firmware address : 0x");
  console_puthex64(fdt_original_address());
  console_putc('\n');

  if (fdt_get_info(&info)) {
    console_puts("  OrgOS copy       : 0x");
    console_puthex64(info.address);
    console_putc('\n');
  } else {
    console_puts("  Status           : Not available or invalid\n");
    return;
  }
  console_puts("  Status           : Valid header\n");
  console_puts("  Total size       : ");
  console_putu32(info.total_size);
  console_puts(" bytes\n");
  console_puts("  Version          : ");
  console_putu32(info.version);
  console_putc('\n');
  console_puts("  Compatible with  : ");
  console_putu32(info.last_compatible_version);
  console_putc('\n');
  console_puts("  Structure offset : 0x");
  console_puthex(info.structure_offset, 8);
  console_putc('\n');
  console_puts("  Structure size   : ");
  console_putu32(info.structure_size);
  console_puts(" bytes\n");
  console_puts("  Strings offset   : 0x");
  console_puthex(info.strings_offset, 8);
  console_putc('\n');
  console_puts("  Strings size     : ");
  console_putu32(info.strings_size);
  console_puts(" bytes\n");
  console_puts("  Reservations     : 0x");
  console_puthex(info.reservations_offset, 8);
  console_puts(" (offset)\n");
}

void cmd_physmem(const char *args) {
  if (!console_arg_end(args)) {
    console_puts("Usage: physmem\n");
    return;
  }

  const physmem_map_t *map = physmem_get_map();
  if (!map) {
    console_puts("Physical memory map unavailable.\n");
    return;
  }
  console_puts("OrgOS physical memory inventory:\n");
  console_puts("\nRAM banks:\n");

  for (unsigned int i = 0; i < map->bank_count; i++) {
    const fdt_memory_range_t *bank = &map->banks[i];
    console_puts("  Bank ");
    console_putu32(i);
    console_puts(": 0x");
    console_puthex64(bank->address);
    console_puts(" - 0x");
    console_puthex64(bank->address + bank->size);
    console_puts(" (exclusive)\n");
  }
  console_puts("\nFirmware-described RAM: ");
  console_putu64( map->described_ram_bytes / (1024ULL * 1024ULL));
  console_puts(" MiB\n");
  console_puts("\nReserved physical ranges:\n");

  static const char *names[] = {
    "FDT header",
    "/reserved-memory",
    "OrgOS kernel",
    "Original FDT",
    "Low memory page"
  };

  for (unsigned int i = 0;
       i < map->reservation_count; i++) {
    const physmem_reservation_t *entry = &map->reservations[i];
    console_puts("  ");
    console_puts(names[entry->kind]);
    console_puts(": 0x");
    console_puthex64(entry->address);
    console_puts(" - 0x");
    console_puthex64(entry->address + entry->size);
    console_puts(" (exclusive)\n");
  }
  console_puts( "\nStatus: Inventory constructed; allocation not enabled.\n");
}

#define PAGE_TEST_COUNT 32U

// Give each byte a predictable value depending on
// its page number and its offset within that page.
static uint8_t page_test_pattern(unsigned int page, unsigned int offset) {
  return (uint8_t)(
      (page * 37U) ^
      offset ^
      (offset >> 5) ^
      (offset >> 9) ^
      0xA5U);
}

static void cmd_page_test(void) {
  // Static storage avoids unnecessarily enlarging
  // the kernel stack.
  static paddr_t addresses[PAGE_TEST_COUNT];
  unsigned int count = 0;
  bool passed = true;
  page_stats_t before, after;
  if (!page_get_stats(&before)) {
    console_puts("Page allocator not initialised.\n");
    return;
  }
  if (before.free_pages < PAGE_TEST_COUNT) {
    console_puts("Insufficient free pages for test.\n");
    return;
  }
  console_puts("Physical page allocator self-test:\n");
  // 1. Allocate pages and check their addresses.
  for (unsigned int i = 0; i < PAGE_TEST_COUNT; i++) {
    paddr_t address;
    if (!page_alloc(&address)) {
      console_puts("FAIL: Allocation failed.\n");
      passed = false;
      goto cleanup;
    }
    // Record immediately so that cleanup owns
    // this page even if a later check fails.
    addresses[count++] = address;

    if (address & (PAGE_SIZE - 1U)) {
      console_puts("FAIL: Unaligned page address.\n");
      passed = false;
      goto cleanup;
    }

    for (unsigned int j = 0; j < i; j++) {
      if (addresses[j] == address) {
        console_puts("FAIL: Duplicate allocation.\n");
        passed = false;
        goto cleanup;
      }
    }
  }
  console_puts("  Allocation and uniqueness : PASS\n");
  // 2. Write a distinctive pattern to each page.
  // OrgOS currently identity-maps physical RAM.
  for (unsigned int i = 0; i < count; i++) {
    volatile uint8_t *memory =
        (volatile uint8_t *)(uintptr_t)addresses[i];

    for (unsigned int offset = 0; offset < PAGE_SIZE; offset++)
      memory[offset] = page_test_pattern(i, offset);
  }
  // 3. Verify everything after all writes finish.
  // This detects both corruption and overlapping pages.
  for (unsigned int i = 0; i < count; i++) {
    volatile const uint8_t *memory =
        (volatile const uint8_t *)(uintptr_t)addresses[i];
    for (unsigned int offset = 0; offset < PAGE_SIZE; offset++) {
      if (memory[offset] != page_test_pattern(i, offset)) {
        console_puts("FAIL: Memory corruption at 0x");
        console_puthex64(addresses[i] + offset);
        console_putc('\n');
        passed = false;
        goto cleanup;
      }
    }
  }
  console_puts("  Memory integrity          : PASS\n");
  // 4. Test invalid releases without changing ownership.
  if (page_free(addresses[0] + 1U) ||
      page_free(0)) {
    console_puts("FAIL: Invalid release accepted.\n");
    passed = false;
    goto cleanup;
  }
  console_puts("  Invalid release rejection : PASS\n");

cleanup:
  // Release all pages owned by this test.
  for (unsigned int i = 0; i < count; i++) {
    if (!page_free(addresses[i])) {
      console_puts("FAIL: Cleanup release failed.\n");
      passed = false;
    }
  }
  // 5. A released page must not be releasable again.
  if (count > 0 && page_free(addresses[0])) {
    console_puts("FAIL: Double free accepted.\n");
    passed = false;
  } else if (count > 0) {
    console_puts("  Double free rejection     : PASS\n");
  }
  if (!page_get_stats(&after)) {
    console_puts("FAIL: Cannot read final statistics.\n");
    return;
  }
  if (after.total_pages != before.total_pages ||
      after.reserved_pages != before.reserved_pages ||
      after.allocated_pages != before.allocated_pages ||
      after.free_pages != before.free_pages) {
    console_puts("FAIL: Page accounting mismatch.\n");
    passed = false;
  } else {
    console_puts("  Page accounting           : PASS\n");
  }
  console_puts("\nResult: ");
  console_puts(passed ? "ALL TESTS PASSED\n" : "TEST FAILED\n");
}

static void cmd_page_run_test(void) {
  const unsigned int count = 4;
  const unsigned int alignment = 16;

  page_stats_t before, after;
  paddr_t address;
  bool passed = true;

  if (!page_get_stats(&before) ||
      before.free_pages < count) {
    console_puts("Insufficient free pages.\n");
    return;
  }

  console_puts("Contiguous page allocation test:\n");

  if (!page_alloc_run(count, alignment, &address)) {
    console_puts("FAIL: Allocation failed.\n");
    return;
  }

  console_puts("  Physical address : 0x");
  console_puthex64(address);
  console_putc('\n');

  console_puts("  Size             : ");
  console_putu32(count * PAGE_SIZE);
  console_puts(" bytes\n");

  if (address % (alignment * PAGE_SIZE) != 0) {
    console_puts("FAIL: Incorrect alignment.\n");
    passed = false;
  } else {
    console_puts("  Alignment        : PASS\n");
  }

  // Current OrgOS mappings are identity mappings.
  volatile uint8_t *memory =
      (volatile uint8_t *)(uintptr_t)address;

  // Write across the entire contiguous allocation.
  for (unsigned int i = 0; i < count * PAGE_SIZE; i++)
    memory[i] = (uint8_t)((i ^ (i >> 8) ^ 0xA5U) & 0xFFU);

  // Verify every byte.
  for (unsigned int i = 0; i < count * PAGE_SIZE; i++) {
    uint8_t expected = (uint8_t)((i ^ (i >> 8) ^ 0xA5U) & 0xFFU);

    if (memory[i] != expected) {
      console_puts("FAIL: Memory corruption at 0x");
      console_puthex64(address + i);
      console_putc('\n');
      passed = false;
      break;
    }
  }

  if (passed)
    console_puts("  Memory integrity : PASS\n");

  // An oversized free must fail without changing
  // the ownership of any of our pages.
  if (page_free_run(address + 1U, count)) {
    console_puts("FAIL: Invalid run release accepted.\n");
    passed = false;
  }

  if (!page_free_run(address, count)) {
    console_puts("FAIL: Release failed.\n");
    passed = false;
  } else if (page_free_run(address, count)) {
    console_puts("FAIL: Double release accepted.\n");
    passed = false;
  } else {
    console_puts("  Release checks   : PASS\n");
  }

  if (!page_get_stats(&after) ||
      before.total_pages != after.total_pages ||
      before.reserved_pages != after.reserved_pages ||
      before.allocated_pages != after.allocated_pages ||
      before.free_pages != after.free_pages) {
    console_puts("FAIL: Accounting mismatch.\n");
    passed = false;
  } else {
    console_puts("  Page accounting  : PASS\n");
  }

  console_puts("\nResult: ");
  console_puts(passed ? "ALL TESTS PASSED\n" : "TEST FAILED\n");
}

void cmd_page(const char *args) {
  if (console_arg_word(&args, "runtest") && console_arg_end(args)) {
    cmd_page_run_test();
    return;
  }
  if (console_arg_word(&args, "test") && console_arg_end(args)) {
    cmd_page_test();
    return;
  }
  if (!console_arg_end(args)) {
    console_puts(
        "Usage: page [test | runtest]\n");
    return;
  }
  page_stats_t stats;
  if (!page_get_stats(&stats)) {
    console_puts("Page allocator not initialised.\n");
    return;
  }
  console_puts("Physical page allocator:\n");
  console_puts("  Page size       : 4096 bytes\n");
  console_puts("  Total pages     : ");
  console_putu64(stats.total_pages);
  console_putc('\n');
  console_puts("  Reserved pages  : ");
  console_putu64(stats.reserved_pages);
  console_putc('\n');
  console_puts("  Allocated pages : ");
  console_putu64(stats.allocated_pages);
  console_putc('\n');
  console_puts("  Free pages      : ");
  console_putu64(stats.free_pages);
  console_putc('\n');
}

void cmd_cache(const char *args) {
  if (!console_arg_end(args)) {
    console_puts("Usage: cache\n");
    return;
  }

  cache_info_t info;
  if (!cache_get_info(&info)) {
    console_puts("Cache information unavailable.\n");
    return;
  }
  console_puts("AArch64 cache information:\n");
  console_puts("  CTR_EL0             : 0x");
  console_puthex64(info.ctr_el0);
  console_putc('\n');
  console_puts("  Data cache line     : ");
  console_putu64(info.data_line_size);
  console_puts(" bytes\n");
  console_puts("  Instruction line    : ");
  console_putu64(info.instruction_line_size);
  console_puts(" bytes\n");
  mmu_info_t mmu;
  mmu_get_info(&mmu);
  console_puts("  Instruction cache   : ");
  console_puts((mmu.sctlr & (1ULL << 12)) ?  "Enabled\n" : "Disabled\n");
  console_puts("  Data cache          : ");
  console_puts((mmu.sctlr & (1ULL << 2)) ?  "Enabled\n" : "Disabled\n");
}

