
#include <stddef.h>
#include <stdbool.h>

#include "console.h"
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
#include "midi.h"

#define CONSOLE_LINE_SIZE 256

typedef void (*console_command_fn)(const char *args);

static bool end_of_args(const char *p) {
  while (*p == ' ' || *p == '\t')
    p++;

  return *p == '\0';
}

static void console_puthex8(uint8_t value) {
  // Print one byte as two hexadecimal digits.
  static const char hex[] = "0123456789ABCDEF";

  console_putc(hex[value >> 4]);
  console_putc(hex[value & 0x0f]);
}

void console_puthex(uint32_t value, unsigned int digits) {
  static const char hex[] = "0123456789ABCDEF";

  for (unsigned int i = 0; i < digits; i++) {
    unsigned int shift = (digits - 1U - i) * 4U;
    console_putc(hex[(value >> shift) & 0x0fU]);
  }
}

static void console_puthex64(uint64_t value) {
  static const char hex[] = "0123456789ABCDEF";

  for (unsigned int i = 0; i < 16; i++) {
    unsigned int shift = (15U - i) * 4U;
    console_putc(hex[(value >> shift) & 0x0fU]);
  }
}

static void cmd_mailbox(const char *args) {
  if (!end_of_args(args)) {
    console_puts("Usage: mailbox\n");
    return;
  }

  uint32_t revision;
  uint32_t clock_hz;
  uint32_t base;
  uint32_t size;
  console_puts("Firmware mailbox diagnostics:\n");
  console_puts("  Board revision : ");
  if (mailbox_get_board_revision(&revision)) {
    console_puts("0x");
    console_puthex(revision, 8);
    console_putc('\n');
  } else {
    console_puts("FAILED\n");
  }
  console_puts("  UART clock     : ");
  if (mailbox_get_clock_rate(
          MAILBOX_CLOCK_UART, &clock_hz)) {
    console_putu32(clock_hz);
    console_puts(" Hz\n");
  } else {
    console_puts("FAILED\n");
  }
  console_puts("  ARM low memory : ");
  if (mailbox_get_arm_memory(&base, &size)) {
    console_putu32(size / (1024U * 1024U));
    console_puts(" MiB\n");
  } else {
    console_puts("FAILED\n");
  }
}

static void cmd_timer(const char *args) {
  (void)args;

  // This function tells us some interesting information about the timer.
  uint64_t tick = timer_now_us();

  console_puts("System timer:\n");
  console_puts("  Counter    : ");
  console_putu64(tick);
  console_puts(" ticks\n");
  console_puts("  Frequency  : 1000000 Hz\n");
  console_puts("  Resolution : 1 us\n");
  console_puts("  Elapsed    : ");
  console_putu64(tick / 1000000U);
  console_putc('.');

  uint32_t fraction = (uint32_t)(tick % 1000000U);
  uint32_t divisor = 100000U;
  while (divisor) {
    console_putc('0' + (fraction / divisor) % 10U);
    divisor /= 10U;
  }
  console_puts(" seconds since time began.\n");
}

static void cmd_uptime(const char *args) {
  (void)args;

  uint64_t seconds = timer_uptime_us() / 1000000U;
  uint64_t days = seconds / 86400U;
  seconds %= 86400U;
  uint32_t hours = (uint32_t)(seconds / 3600U);
  seconds %= 3600U;
  uint32_t minutes = (uint32_t)(seconds / 60U);
  seconds %= 60U;

  console_puts("OrgOS uptime: ");
  if (days) {
    console_putu64(days);
    console_puts(days == 1 ? " day, " : " days, ");
  }
  console_putu32(hours);
  console_puts("h ");
  console_putu32(minutes);
  console_puts("m ");
  console_putu32((uint32_t)seconds);
  console_puts("s\n");
}

static void cmd_irq(const char *args) {
  (void)args;

  console_puts("Interrupts handled: ");
  console_putu64(irq_count());
  console_putc('\n');

  console_puts("UART RX overruns : ");
  console_putu32(uart_rx_overruns());
  console_putc('\n');
}

static void cmd_help(const char *args);

// Return true if the next whitespace-delimited word
// matches the supplied string.
// Advance the input pointer past the matched word.
static bool next_word_is(const char **input, const char *word) {
  const char *p = *input;
  const char *w = word;

  while (*p == ' ' || *p == '\t')
    p++;

  while (*w && *p == *w) {
    p++;
    w++;
  }

  if (*w || (*p && *p != ' ' && *p != '\t'))
    return false;

  *input = p;
  return true;
}


// Parse an unsigned 64-bit hexadecimal address.
// Accept either:
//   0x100000000
//   100000000
// Reject missing digits, invalid characters,
// overflow, and trailing non-whitespace text.
static bool next_hex64(const char **input, uint64_t *value) {
  const char *p = *input;
  uint64_t result = 0;
  bool found = false;

  while (*p == ' ' || *p == '\t')
    p++;
  if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
    p += 2;
  for (;;) {
    unsigned int digit;
    if (*p >= '0' && *p <= '9')
      digit = (unsigned int)(*p - '0');
    else if (*p >= 'a' && *p <= 'f')
      digit = (unsigned int)(*p - 'a') + 10U;
    else if (*p >= 'A' && *p <= 'F')
      digit = (unsigned int)(*p - 'A') + 10U;
    else
      break;
    if (result > (UINT64_MAX - digit) / 16ULL)
      return false;
    result = result * 16ULL + digit;
    found = true;
    p++;
  }
  if (!found || (*p && *p != ' ' && *p != '\t'))
    return false;
  *value = result;
  *input = p;
  return true;
}

static bool next_uint(const char **input, unsigned int *value) {
  // Read an unsigned decimal integer.
  // Reject missing values, non-decimal characters and overflow.
  const char *p = *input;
  unsigned int result = 0;
  bool found = false;

  while (*p == ' ' || *p == '\t')
    p++;

  while (*p >= '0' && *p <= '9') {
    unsigned int digit = (unsigned int)(*p - '0');

    if (result > (UINT32_MAX - digit) / 10U)
      return false;

    result = result * 10U + digit;
    found = true;
    p++;
  }

  if (!found || (*p && *p != ' ' && *p != '\t'))
    return false;

  *value = result;
  *input = p;
  return true;
}

static void print_mmu_translation(uint64_t address) {
  mmu_translation_t result;

  console_puts("  VA 0x");
  console_puthex64(address);
  if (!mmu_translate(address, &result)) {
    console_puts(" : diagnostic unavailable\n");
    return;
  }
  if (result.fault) {
    console_puts(" : FAULT (status 0x");
    console_puthex(result.fault_status, 2);
    console_puts(")\n");
  } else {
    console_puts(" -> PA 0x");
    console_puthex64(result.physical_address);
    console_putc('\n');
  }
}

static void print_mmu_write_test(uint64_t address) {
  mmu_translation_t result;

  console_puts("  VA 0x");
  console_puthex64(address);
  if (!mmu_translate_write(address, &result)) {
    console_puts(" : diagnostic unavailable\n");
  } else if (result.fault) {
    console_puts(" : WRITE DENIED (status 0x");
    console_puthex(result.fault_status, 2);
    console_puts(")\n");
  } else {
    console_puts(" : WRITE ALLOWED\n");
  }
}

static void cmd_pci_dma_test(void) {
  static const struct {
    uint64_t physical;
    uint64_t length;
    uint64_t expected;
    bool valid;
  } tests[] = {
    { 0x00000000ULL, 4096, 0x100000000ULL, true },
    { 0x00001000ULL, 4096, 0x100001000ULL, true },
    { 0x3B000000ULL, 4096, 0x13B000000ULL, true },
    { 0xBFFFF000ULL, 4096, 0x1BFFFF000ULL, true },
    { 0xBFFFF000ULL, 4097, 0, false },
    { 0xC0000000ULL, 4096, 0, false },
    { 0xFFFFFFFFULL, 4096, 0, false },
    { 0x00001000ULL, 0, 0, false },
    { 0x00001000ULL, UINT64_MAX, 0, false }
  };

  unsigned int passed = 0;
  unsigned int failed = 0;
  console_puts("PCIe DMA address translation self-test:\n");
  for (unsigned int i = 0;
       i < sizeof(tests) / sizeof(tests[0]); i++) {
    uint64_t result = 0;
    bool success = pcie_phys_to_dma(
        tests[i].physical, tests[i].length, &result);
    bool correct = success == tests[i].valid;
    if (success && tests[i].valid)
      correct = result == tests[i].expected;
    console_puts("  Test ");
    console_putu32(i + 1);
    console_puts(": ");
    console_puts(correct ? "PASS\n" : "FAIL\n");
    if (correct)
      passed++;
    else
      failed++;
  }
  // A NULL output pointer must always be rejected.
  if (!pcie_phys_to_dma(0x1000, 4096, NULL))
    passed++;
  else
    failed++;
  console_puts("\nPassed: ");
  console_putu32(passed);
  console_puts("\nFailed: ");
  console_putu32(failed);
  console_putc('\n');
  console_puts("\nResult: ");
  console_puts(failed == 0 ?  "ALL TESTS PASSED\n" : "TEST FAILED\n");
}

static void cmd_pci_dma(void) {
  pcie_dma_info_t info;

  if (!pcie_dma_probe(&info)) {
    console_puts("PCIe DMA registers unavailable.\n");
    return;
  }

  console_puts("BCM2711 PCIe inbound DMA configuration:\n");
  console_puts("  MISC_CTRL   : 0x");
  console_puthex(info.misc_ctrl, 8);
  console_putc('\n');
  console_puts("  BAR2 low    : 0x");
  console_puthex(info.bar2_lo, 8);
  console_putc('\n');
  console_puts("  BAR2 high   : 0x");
  console_puthex(info.bar2_hi, 8);
  console_putc('\n');
  console_puts("  BAR3 low    : 0x");
  console_puthex(info.bar3_lo, 8);
  console_putc('\n');
  console_puts("  Endian ctrl : 0x");
  console_puthex(info.endian_ctrl, 8);
  console_putc('\n');
  console_puts("\n");
  console_puts("  BAR2 size encoding : ");
  console_putu32(info.bar2_lo & 0x1fU);
  console_putc('\n');
  console_puts("  SCB0 size encoding : ");
  console_putu32((info.misc_ctrl >> 27) & 0x1fU);
  console_putc('\n');
  console_puts("\n");
  console_puts("  Note: Inbound DMA configuration is read-only here.\n");
}

static void cmd_pci(const char *args) {
  if (next_word_is(&args, "dma")) {
    if (next_word_is(&args, "test") && end_of_args(args)) {
      cmd_pci_dma_test();
      return;
    }
    if (end_of_args(args)) {
      cmd_pci_dma();
      return;
    }
    console_puts("Usage: pci dma [test]\n");
    return;
  }
  if (next_word_is(&args, "init") && end_of_args(args)) {
    console_puts("Initialising BCM2711 PCIe link...\n");
    pcie_init_result_t result = pcie_init_link();
    switch (result) {
      case PCIE_INIT_OK:
        console_puts("PCIe link established.\n");
        break;
      case PCIE_INIT_ALREADY_UP:
        console_puts("PCIe link is already active.\n");
        break;
      case PCIE_INIT_NOT_ROOT_COMPLEX:
        console_puts("PCIe initialization failed: not in RC mode.\n");
        break;
      case PCIE_INIT_LINK_TIMEOUT:
        console_puts("PCIe initialization failed: link timeout.\n");
        break;
    }
    console_putc('\n');
  } else if (!end_of_args(args)) {
    console_puts("Usage: pci [init]\n");
    return;
  }

  pcie_info_t info;
  pcie_probe(&info);

  console_puts("BCM2711 PCIe controller:\n");
  console_puts("  Status      : 0x");
  console_puthex(info.status, 8);
  console_putc('\n');
  console_puts("  Revision    : 0x");
  console_puthex(info.revision, 8);
  console_putc('\n');
  console_puts("  Mode        : ");
  console_puts(info.root_complex ?
      "Root complex\n" : "Not root complex\n");
  console_puts("  Link        : ");
  console_puts(info.link_up ? "Up\n" : "Down\n");
  if (!info.root_complex || !info.link_up) {
    console_puts(
        "\nPCIe host bridge is not ready.\n");
    return;
  }

  console_puts("\nRoot bridge:\n");
  console_puts("  Vendor ID   : 0x");
  console_puthex(info.root_vendor, 4);
  console_putc('\n');
  console_puts("  Device ID   : 0x");
  console_puthex(info.root_device, 4);
  console_putc('\n');
  console_puts("  Secondary   : ");
  console_putu32(info.secondary_bus);
  console_putc('\n');
  console_puts("  Subordinate : ");
  console_putu32(info.subordinate_bus);
  console_putc('\n');
  if (info.secondary_bus == 0 ||
      info.subordinate_bus < info.secondary_bus) {
    console_puts(
        "\nDownstream bus is not configured.\n");
    return;
  }

  if (!info.endpoint_present) {
    console_puts("\nNo downstream device found.\n");
    return;
  }

  console_puts("\nDownstream device:\n");
  console_puts("  Vendor ID   : 0x");
  console_puthex(info.endpoint_vendor, 4);
  console_putc('\n');
  console_puts("  Device ID   : 0x");
  console_puthex(info.endpoint_device, 4);
  console_putc('\n');
  console_puts("  Class       : 0x");
  console_puthex(info.endpoint_class, 6);
  console_putc('\n');
  console_puts("  BAR0        : 0x");
  console_puthex(info.endpoint_bar0, 8);
  console_putc('\n');
  console_puts("  BAR1        : 0x");
  console_puthex(info.endpoint_bar1, 8);
  console_putc('\n');
  if (info.endpoint_vendor == 0x1106U &&
      info.endpoint_device == 0x3483U) {
    console_puts("\nVIA VL805 USB controller detected.\n");
  } else {
    console_puts("Unknown USB device detected.\n");
  }
}

static void cmd_dma_owner_test(void) {
  dma_buffer_t buffer;
  page_stats_t before, after;
  unsigned int failed = 0;

  console_puts("DMA ownership self-test:\n");
  if (!page_get_stats(&before)) {
    console_puts("FAIL: Page allocator unavailable.\n");
    return;
  }
  if (!dma_alloc(4096, 4096, &buffer)) {
    console_puts("FAIL: Allocation failed.\n");
    return;
  }
  // 1. Newly allocated memory belongs to the CPU.
  if (buffer.owner != DMA_OWNER_CPU) {
    console_puts("FAIL: Initial ownership.\n");
    failed++;
  } else {
    console_puts("  Initial CPU ownership : PASS\n");
  }
  // 2. Cannot return memory from a device which
  // does not currently own it.
  if (dma_end_device(&buffer)) {
    console_puts("FAIL: Invalid return accepted.\n");
    failed++;
  } else {
    console_puts("  Invalid return        : PASS\n");
  }
  // 3. Reject an invalid transfer direction.
  if (dma_begin_device(&buffer, (dma_direction_t)99) ||
      buffer.owner != DMA_OWNER_CPU) {
    console_puts("FAIL: Invalid direction accepted.\n");
    failed++;
  } else {
    console_puts("  Direction validation  : PASS\n");
  }
  // 4. Transfer ownership to the device.
  if (!dma_begin_device(&buffer, DMA_FROM_DEVICE) ||
      buffer.owner != DMA_OWNER_DEVICE ||
      buffer.direction != DMA_FROM_DEVICE) {
    console_puts("FAIL: Device handoff.\n");
    failed++;
  } else {
    console_puts("  Device handoff        : PASS\n");
  }
  // 5. Reject repeated handoff.
  if (dma_begin_device(&buffer, DMA_TO_DEVICE)) {
    console_puts("FAIL: Double handoff accepted.\n");
    failed++;
  } else {
    console_puts("  Double handoff        : PASS\n");
  }
  // 6. Device-owned memory must not be freed.
  if (dma_free(&buffer)) {
    console_puts("FAIL: Device-owned buffer freed.\n");
    failed++;
  } else {
    console_puts("  Free protection       : PASS\n");
  }
  // 7. Return ownership to the CPU.
  if (!dma_end_device(&buffer) ||
      buffer.owner != DMA_OWNER_CPU) {
    console_puts("FAIL: CPU handoff.\n");
    failed++;
  } else {
    console_puts("  CPU handoff           : PASS\n");
  }
  // 8. A second return must be rejected.
  if (dma_end_device(&buffer)) {
    console_puts("FAIL: Double return accepted.\n");
    failed++;
  } else {
    console_puts("  Double return         : PASS\n");
  }
  // 9. Test another direction on the same buffer.
  if (!dma_begin_device(&buffer, DMA_BIDIRECTIONAL) ||
      buffer.direction != DMA_BIDIRECTIONAL || !dma_end_device(&buffer)) {
    console_puts("FAIL: Bidirectional transition.\n");
    failed++;
  } else {
    console_puts("  Bidirectional cycle   : PASS\n");
  }
  // 10. Release the buffer and check accounting.
  if (!dma_free(&buffer)) {
    console_puts("FAIL: Final release.\n");
    failed++;
  } else if (dma_free(&buffer)) {
    console_puts("FAIL: Double free accepted.\n");
    failed++;
  } else {
    console_puts("  Final release         : PASS\n");
  }
  if (!page_get_stats(&after) ||
      before.total_pages != after.total_pages ||
      before.reserved_pages != after.reserved_pages ||
      before.allocated_pages != after.allocated_pages ||
      before.free_pages != after.free_pages) {
    console_puts("FAIL: Page accounting mismatch.\n");
    failed++;
  } else {
    console_puts("  Page accounting       : PASS\n");
  }
  console_puts("\nResult: ");
  console_puts(failed == 0 ?
      "ALL TESTS PASSED\n" : "TEST FAILED\n");
}

static void cmd_dma_cache_test(void) {
  dma_buffer_t buffer;
  page_stats_t before, after;
  unsigned int failed = 0;

  console_puts("DMA cache synchronisation self-test:\n");
  if (!page_get_stats(&before)) {
    console_puts("FAIL: Page allocator unavailable.\n");
    return;
  }
  if (!dma_alloc(4096, 4096, &buffer)) {
    console_puts("FAIL: Allocation failed.\n");
    return;
  }

  volatile uint8_t *memory = (volatile uint8_t *)buffer.virtual;

  // Exercise each DMA direction.
  static const dma_direction_t directions[] = {
    DMA_TO_DEVICE,
    DMA_FROM_DEVICE,
    DMA_BIDIRECTIONAL
  };

  static const char *names[] = {
    "CPU to device",
    "Device to CPU",
    "Bidirectional"
  };

  for (unsigned int direction = 0; direction < 3; direction++) {
    // CPU owns the buffer here.
    // Write a distinctive pattern to every byte.
    for (unsigned int i = 0; i < PAGE_SIZE; i++)
      memory[i] = (uint8_t)(i ^ (i >> 6) ^ (direction * 37U) ^ 0xA5U);
    bool ok = dma_begin_device( &buffer, directions[direction]);
    // No actual DMA occurs in this test. We
    // immediately simulate completion by returning
    // ownership without touching the buffer.
    if (ok)
      ok = dma_end_device(&buffer);
    if (ok) {
      // CPU ownership has been restored.
      for (unsigned int i = 0; i < PAGE_SIZE; i++) {
        uint8_t expected = (uint8_t)(
            i ^ (i >> 6) ^ (direction * 37U) ^ 0xA5U);
        if (memory[i] != expected) {
          ok = false;
          break;
        }
      }
    }
    console_puts("  ");
    console_puts(names[direction]);
    console_puts("  : ");
    console_puts(ok ? "PASS\n" : "FAIL\n");
    if (!ok) {
      failed++;
      // Do not access a potentially device-owned
      // buffer or continue testing.
      break;
    }
  }
  if (buffer.owner == DMA_OWNER_CPU) {
    if (!dma_free(&buffer)) {
      console_puts("FAIL: Release failed.\n");
      failed++;
    }
  } else {
    console_puts("FAIL: Buffer remains device-owned.\n");
    failed++;
  }
  if (!page_get_stats(&after) ||
      before.total_pages != after.total_pages ||
      before.reserved_pages != after.reserved_pages ||
      before.allocated_pages != after.allocated_pages ||
      before.free_pages != after.free_pages) {
    console_puts("FAIL: Page accounting mismatch.\n");
    failed++;
  } else {
    console_puts("  Page accounting : PASS\n");
  }
  console_puts("\nResult: ");
  console_puts(failed == 0 ?
      "ALL TESTS PASSED\n" : "TEST FAILED\n");
}

static void cmd_dma_edge_test(void) {
  static const uint64_t sizes[] = {100, 4097};
  static const dma_direction_t directions[] = {
    DMA_TO_DEVICE,
    DMA_FROM_DEVICE,
    DMA_BIDIRECTIONAL
  };
  page_stats_t before, after;
  unsigned int failed = 0;
  console_puts("DMA edge-case self-test:\n");
  if (!page_get_stats(&before)) {
    console_puts("FAIL: Page allocator unavailable.\n");
    return;
  }
  cache_info_t cache;
  if (!cache_get_info(&cache)) {
    console_puts("FAIL: Cache information unavailable.\n");
    return;
  }
  console_puts("  Data cache line size : ");
  console_putu64(cache.data_line_size);
  console_puts(" bytes\n");
  bool alignment_ok = cache.data_line_size != 0 &&
      cache.data_line_size <= PAGE_SIZE &&
      (cache.data_line_size & (cache.data_line_size - 1U)) == 0 &&
      (PAGE_SIZE % cache.data_line_size) == 0;
  if (!alignment_ok) {
    console_puts("FAIL: Unsupported cache-line size.\n");
    return;
  }
  // Invalid arguments must be rejected.
  dma_buffer_t empty = {0};
  bool invalid_ok =
      !dma_begin_device(NULL, DMA_TO_DEVICE) && !dma_end_device(NULL) &&
      !dma_free(NULL) && !dma_begin_device(&empty, DMA_TO_DEVICE) &&
      !dma_end_device(&empty) && !dma_free(&empty);
  console_puts("  Invalid descriptors   : ");
  console_puts(invalid_ok ? "PASS\n" : "FAIL\n");
  if (!invalid_ok)
    failed++;
  for (unsigned int test = 0; test < 2; test++) {
    dma_buffer_t buffer = {0};
    uint64_t requested = sizes[test];
    if (!dma_alloc(requested, 4096, &buffer)) {
      console_puts("FAIL: Allocation failed.\n");
      failed++;
      break;
    }
    bool ok = true;
    uint64_t capacity = (uint64_t)buffer.pages * PAGE_SIZE;
    if (buffer.size != requested || capacity < requested ||
        ((uintptr_t)buffer.virtual % cache.data_line_size) != 0 ||
        buffer.owner != DMA_OWNER_CPU)
      ok = false;
    volatile uint8_t *memory = (volatile uint8_t *)buffer.virtual;
    // Include unused padding in the test.
    // Cache maintenance covers the entire allocation.
    for (uint64_t i = 0; i < capacity; i++)
      memory[i] = (uint8_t)(i ^ (i >> 8) ^ 0x5AU);
    for (unsigned int cycle = 0; cycle < 9 && ok; cycle++) {
      dma_direction_t direction = directions[cycle % 3];
      if (!dma_begin_device(&buffer, direction)) {
        ok = false;
        break;
      }
      // Reject repeated handoff while device-owned.
      if (dma_begin_device(&buffer, direction))
        ok = false;
      // Reject freeing a device-owned allocation.
      if (dma_free(&buffer))
        ok = false;
      // No actual device DMA is performed.
      if (!dma_end_device(&buffer)) {
        ok = false;
        break;
      }
      // Reject returning ownership twice.
      if (dma_end_device(&buffer))
        ok = false;
      // Verify that cache maintenance preserved
      // every byte, including padding.
      for (uint64_t i = 0; i < capacity; i++) {
        uint8_t expected = (uint8_t)(i ^ (i >> 8) ^ 0x5AU);
        if (memory[i] != expected) {
          ok = false;
          break;
        }
      }
    }
    console_puts("  ");
    console_putu64(requested);
    console_puts("-byte buffer cycles : ");
    console_puts(ok ? "PASS\n" : "FAIL\n");
    if (!ok)
      failed++;
    if (buffer.owner != DMA_OWNER_CPU || !dma_free(&buffer)) {
      console_puts("FAIL: Buffer cleanup.\n");
      failed++;
      // Do not risk reusing this allocation.
      break;
    }
  }
  if (!page_get_stats(&after) || before.total_pages != after.total_pages ||
      before.reserved_pages != after.reserved_pages ||
      before.allocated_pages != after.allocated_pages ||
      before.free_pages != after.free_pages) {
    console_puts("FAIL: Page accounting mismatch.\n");
    failed++;
  } else {
    console_puts("  Page accounting      : PASS\n");
  }
  console_puts("\nResult: ");
  console_puts(failed == 0 ?
      "ALL TESTS PASSED\n" : "TEST FAILED\n");
}

static void cmd_dma(const char *args) {
  if (next_word_is(&args, "cache") && end_of_args(args)) {
    cmd_dma_cache_test();
    return;
  }
  if (next_word_is(&args, "owner") && end_of_args(args)) {
    cmd_dma_owner_test();
    return;
  }
  if (next_word_is(&args, "edge") && end_of_args(args)) {
    cmd_dma_edge_test();
    return;
  }
  console_puts("Usage: dma [test | owner | cache | edge]\n");
}

static void cmd_usb_state(void) {
  uint16_t pci_command;
  xhci_state_t state;

  if (!pcie_xhci_get_command(&pci_command)) {
    console_puts("Cannot read VL805 PCI command register.\n");
    return;
  }

  if (!xhci_get_state(&state)) {
    console_puts("Cannot read xHCI operational registers.\n");
    return;
 }

  console_puts("VL805 PCI configuration:\n");
  console_puts("  PCI command    : 0x");
  console_puthex(pci_command, 4);
  console_putc('\n');
  console_puts("  Memory decode  : ");
  console_puts((pci_command & (1U << 1)) ?  "Enabled\n" : "Disabled\n");
  console_puts("  Bus mastering  : ");
  console_puts((pci_command & (1U << 2)) ?  "Enabled\n" : "Disabled\n");
  console_puts("\nxHCI operational state:\n");
  console_puts("  USBCMD         : 0x");
  console_puthex(state.command, 8);
  console_putc('\n');
  console_puts("  USBSTS         : 0x");
  console_puthex(state.status, 8);
  console_putc('\n');
  console_puts("  Controller     : ");
  console_puts((state.status & 1U) ?  "Halted\n" : "Running\n");
  console_puts("  Not ready      : ");
  console_puts((state.status & (1U << 11)) ?  "Yes\n" : "No\n");
  console_puts("  Page size bits : 0x");
  console_puthex(state.pagesize, 8);
  console_putc('\n');
  console_puts("  CONFIG         : 0x");
  console_puthex(state.config, 8);
  console_putc('\n');
  console_puts("  CRCR           : 0x");
  console_puthex64(state.command_ring);
  console_putc('\n');
  console_puts("  DCBAAP         : 0x");
  console_puthex64(state.dcbaa);
  console_putc('\n');
}

static void cmd_usb_dma(void) {
  xhci_dma_t dma;
  page_stats_t before, after;
  bool passed = true;
  if (!page_get_stats(&before)) {
    console_puts("Page allocator unavailable.\n");
    return;
  }
  if (!xhci_dma_prepare(&dma)) {
    console_puts("xHCI DMA preparation failed.\n");
    return;
  }
  console_puts("xHCI DMA structures:\n");
  console_puts("  Command ring : 0x");
  console_puthex64(dma.command_ring.bus);
  console_putc('\n');
  console_puts("  Event ring   : 0x");
  console_puthex64(dma.event_ring.bus);
  console_putc('\n');
  console_puts("  ERST         : 0x");
  console_puthex64(dma.erst.bus);
  console_putc('\n');
  console_puts("  DCBAA        : 0x");
  console_puthex64(dma.dcbaa.bus);
  console_putc('\n');
  xhci_trb_t *commands = (xhci_trb_t *)dma.command_ring.virtual;
  xhci_trb_t *link = &commands[255];
  if (link->parameter != dma.command_ring.bus ||
      ((link->control >> 10) & 0x3fU) != 6U || (link->control & 3U) != 3U) {
    console_puts("  Command ring : FAIL\n");
    passed = false;
  } else {
    console_puts("  Command ring : PASS\n");
  }
  xhci_erst_entry_t *erst = (xhci_erst_entry_t *)dma.erst.virtual;
  if (erst[0].base != dma.event_ring.bus || erst[0].size != 256) {
    console_puts("  ERST         : FAIL\n");
    passed = false;
  } else {
    console_puts("  ERST         : PASS\n");
  }
  // Confirm every DMA address has 64-byte alignment.
  if ((dma.command_ring.bus & 63U) || (dma.event_ring.bus & 63U) ||
      (dma.erst.bus & 63U) || (dma.dcbaa.bus & 63U)) {
    console_puts("  Alignment    : FAIL\n");
    passed = false;
  } else {
    console_puts("  Alignment    : PASS\n");
  }
    console_puts("  Scratchpads  : ");
  console_putu32(dma.scratchpad_count);
  console_putc('\n');

  bool scratchpads_ok = true;

  uint64_t *dcbaa = (uint64_t *)dma.dcbaa.virtual;
  uint64_t *pointers =
      (uint64_t *)dma.scratchpad_array.virtual;

  // This development controller requires exactly
  // 31 scratchpad buffers.
  if (dma.scratchpad_count != 31U ||
      dma.scratchpad_array.pages != 1U ||
      dcbaa[0] != dma.scratchpad_array.bus ||
      (dma.scratchpad_array.bus & 63U))
    scratchpads_ok = false;

  if (scratchpads_ok) {
    for (unsigned int i = 0;
        i < dma.scratchpad_count; i++) {

      dma_buffer_t *buffer = &dma.scratchpads[i];

      if (buffer->pages != 1U ||
          !buffer->virtual ||
          (buffer->bus & (PAGE_SIZE - 1U)) ||
          pointers[i] != buffer->bus ||
          buffer->owner != DMA_OWNER_CPU) {
        scratchpads_ok = false;
        break;
      }

      // Every scratchpad page must start zeroed.
      const uint8_t *bytes =
          (const uint8_t *)buffer->virtual;

      for (unsigned int j = 0; j < PAGE_SIZE; j++) {
        if (bytes[j] != 0) {
          scratchpads_ok = false;
          break;
        }
      }

      if (!scratchpads_ok)
        break;
    }
  }

  // No device contexts have been configured yet.
  for (unsigned int i = 1; i <= 32U; i++) {
    if (dcbaa[i] != 0) {
      scratchpads_ok = false;
      break;
    }
  }
  console_puts("  Scratchpads  : ");
  console_puts(scratchpads_ok ? "PASS\n" : "FAIL\n");
  if (!scratchpads_ok)
    passed = false;
  // Five structural pages plus 31 scratchpad pages.
  if (dma.scratchpad_count != 31U ||
      dma.command_ring.pages +
      dma.event_ring.pages +
      dma.erst.pages +
      dma.dcbaa.pages +
      dma.scratchpad_array.pages +
      dma.scratchpad_count != 36U) {
    console_puts("  Page count   : FAIL\n");
    passed = false;
  } else {
    console_puts("  Page count   : PASS (36 pages)\n");
  }
  if (!xhci_dma_release(&dma)) {
    console_puts("  Release      : FAIL\n");
    passed = false;
  } else {
    console_puts("  Release      : PASS\n");
  }
  if (!page_get_stats(&after) || before.free_pages != after.free_pages ||
      before.allocated_pages != after.allocated_pages) {
    console_puts("  Accounting   : FAIL\n");
    passed = false;
  } else {
    console_puts("  Accounting   : PASS\n");
  }
  console_puts("\nResult: ");
  console_puts(passed ?  "ALL TESTS PASSED\n" : "TEST FAILED\n");
}

static void cmd_usb_ready(void) {
  xhci_readiness_t result;

  if (!xhci_check_readiness(&result)) {
    console_puts("Cannot inspect xHCI readiness.\n");
    return;
  }

  console_puts("VL805 readiness:\n");
  console_puts("  HCSPARAMS2       : 0x");
  console_puthex(result.hcsparams2, 8);
  console_putc('\n');
  console_puts("  Scratchpad count : ");
  console_putu32(result.scratchpad_buffers);
  console_putc('\n');
  console_puts("  Controller ready : ");
  console_puts(result.ready ? "Yes\n" : "No\n");
  console_puts("  Controller halted: ");
  console_puts(result.halted ? "Yes\n" : "No\n");
}

static void cmd_usb_inspect(void) {
  pcie_xhci_diagnostic_t pci;
  xhci_cap_diagnostic_t hc;

  if (!pcie_xhci_diagnose(&pci)) {
    console_puts("VL805 PCI diagnostics unavailable.\n");
    return;
  }

  if (!xhci_diagnose_capabilities(&hc)) {
    console_puts("xHCI capability diagnostics unavailable.\n");
    return;
  }
  console_puts("VL805 PCI diagnostics:\n");
  console_puts("  Revision ID    : 0x");
  console_puthex(pci.class_revision & 0xffU, 2);
  console_putc('\n');
  console_puts("  Command/status : 0x");
  console_puthex(pci.command_status, 8);
  console_putc('\n');
  console_puts("  Header         : 0x");
  console_puthex(pci.header, 8);
  console_putc('\n');
  console_puts("  Capabilities   : ");
  console_puts(pci.capabilities_present ?  "Present\n" : "Absent\n");
  console_puts("  PM capability  : ");
  console_puts(pci.pm_present ?  "Present\n" : "Absent\n");
  if (pci.pm_present) {
    console_puts("  PM offset      : 0x");
    console_puthex(pci.pm_offset, 2);
    console_putc('\n');
    console_puts("  PM capabilities: 0x");
    console_puthex(pci.pm_capabilities, 4);
    console_putc('\n');
    console_puts("  PMCSR          : 0x");
    console_puthex(pci.pm_control, 4);
    console_putc('\n');
    console_puts("  Power state    : D");
    console_putu32(pci.pm_control & 3U);
    console_putc('\n');
  }
  console_puts("\nxHCI capability diagnostics:\n");
  console_puts("  HCSPARAMS1     : 0x");
  console_puthex(hc.hcsparams1, 8);
  console_putc('\n');
  console_puts("  HCSPARAMS2     : 0x");
  console_puthex(hc.hcsparams2, 8);
  console_putc('\n');
  console_puts("  HCSPARAMS3     : 0x");
  console_puthex(hc.hcsparams3, 8);
  console_putc('\n');
  console_puts("  HCCPARAMS1     : 0x");
  console_puthex(hc.hccparams1, 8);
  console_putc('\n');
  console_puts("  Ext cap offset : 0x");
  console_puthex(hc.ext_cap_offset, 8);
  console_putc('\n');
  console_puts("  Ext cap header : 0x");
  console_puthex(hc.ext_cap_header, 8);
  console_putc('\n');
  console_puts("  VL805 firmware : ");
  uint32_t firmware_version;
  if (pcie_xhci_get_firmware_version(&firmware_version)) {
    console_puts("0x");
    console_puthex(firmware_version, 8);
    console_putc('\n');
  } else {
    console_puts("Unavailable\n");
  }
}

static void cmd_usb_noops(void) {
  const unsigned int count = 600U;
  console_puts("Executing 600 sequential No Op commands...\n");
  for (unsigned int i = 0; i < count; i++) {
    xhci_noop_info_t info;
    xhci_noop_result_t status = xhci_noop(&info);
    if (status != XHCI_NOOP_OK) {
      console_puts("FAILED at command ");
      console_putu32(i + 1U);
      console_puts(" (result ");
      console_putu32((uint32_t)status);
      console_puts(").\n");
      return;
    }
    if ((i + 1U) % 100U == 0) {
      console_puts("  Completed ");
      console_putu32(i + 1U);
      console_putc('\n');
    }
  }
  console_puts("All 600 commands completed successfully.\n");
}

static const char *usb_speed_name(uint8_t speed) {
  switch (speed) {
    case 1: return "Full (12 Mb/s)";
    case 2: return "Low (1.5 Mb/s)";
    case 3: return "High (480 Mb/s)";
    case 4: return "Super (5 Gb/s)";
    case 5: return "SuperSpeedPlus";
    default: return "Unknown or not negotiated";
  }
}

static const char *usb_link_name(uint8_t state) {
  switch (state) {
    case 0: return "U0";
    case 1: return "U1";
    case 2: return "U2";
    case 3: return "U3";
    case 4: return "Disabled";
    case 5: return "RxDetect";
    case 6: return "Inactive";
    case 7: return "Polling";
    case 8: return "Recovery";
    case 9: return "Hot Reset";
    case 10: return "Compliance";
    case 11: return "Test";
    case 15: return "Resume";
    default: return "Reserved";
  }
}

static void cmd_usb_ports(void) {
  uint8_t count;
  if (!xhci_port_count(&count)) {
    console_puts("USB root ports unavailable.\n");
    return;
  }
  console_puts("xHCI root ports: ");
  console_putu32(count);
  console_putc('\n');
  for (uint8_t n = 1; n <= count; n++) {
    xhci_port_t port;
    if (!xhci_port_read(n, &port)) {
      console_puts("Unable to inspect port ");
      console_putu32(n);
      console_puts(".\n");
      return;
    }
    console_puts("\nPort ");
    console_putu32(n);
    console_puts(":\n");
    console_puts("  PORTSC       : 0x");
    console_puthex(port.portsc, 8);
    console_putc('\n');
    console_puts("  Connected    : ");
    console_puts(port.connected ? "Yes\n" : "No\n");
    console_puts("  Enabled      : ");
    console_puts(port.enabled ? "Yes\n" : "No\n");
    console_puts("  Powered      : ");
    console_puts(port.powered ? "Yes\n" : "No\n");
    console_puts("  Overcurrent  : ");
    console_puts(port.overcurrent ? "Yes\n" : "No\n");
    console_puts("  Reset active : ");
    console_puts(port.resetting ? "Yes\n" : "No\n");
    console_puts("  Speed        : ");
    console_puts(usb_speed_name(port.speed));
    console_putc('\n');
    console_puts("  Link state   : ");
    console_puts(usb_link_name(port.link_state));
    console_putc('\n');
    console_puts("  Change flags :");
    bool changed = false;
#define PRINT_CHANGE(flag, name)                   \
    if (port.flag) {                               \
      console_puts(" " name);                      \
      changed = true;                              \
    }
    PRINT_CHANGE(connect_change, "CSC")
    PRINT_CHANGE(enable_change, "PEC")
    PRINT_CHANGE(warm_reset_change, "WRC")
    PRINT_CHANGE(overcurrent_change, "OCC")
    PRINT_CHANGE(reset_change, "PRC")
    PRINT_CHANGE(link_change, "PLC")
    PRINT_CHANGE(config_error_change, "CEC")
#undef PRINT_CHANGE
    if (!changed)
      console_puts(" None");
    console_putc('\n');
  }
}

static void cmd_usb_protocols(void) {
  xhci_state_t state;
  if (!xhci_get_state(&state)) {
    console_puts("Unable to read xHCI controller state.\n");
    return;
  }
  console_puts("xHCI Supported Protocol Capabilities\n");
  console_puts("  USBCMD : 0x");
  console_puthex(state.command, 8);
  console_putc('\n');
  console_puts("  USBSTS : 0x");
  console_puthex(state.status, 8);
  console_putc('\n');
  unsigned int count = 0;
  for (unsigned int i = 0; i <= 8U; i++) {
    xhci_protocol_t protocol;
    bool found;
    if (!xhci_protocol_get(i, &protocol, &found)) {
      console_puts("Extended capability traversal failed.\n");
      return;
    }
    if (!found)
      break;
    if (i == 8U) {
      console_puts("More than eight protocol records found.\n");
      return;
    }
    count++;
    console_puts("\nProtocol ");
    console_putu32(count);
    console_puts(":\n");
    console_puts("  Offset       : 0x");
    console_puthex(protocol.offset, 4);
    console_putc('\n');
    console_puts("  Header       : 0x");
    console_puthex(protocol.header, 8);
    console_putc('\n');
    console_puts("  Name         : ");
    for (unsigned int c = 0; c < 4U; c++) {
      uint8_t ch = (uint8_t)(protocol.name >> (c * 8U));
      console_putc(ch >= 32U && ch <= 126U ? (char)ch : '.');
    }
    console_putc('\n');
    console_puts("  Revision     : ");
    console_putu32(protocol.major);
    console_putc('.');
    console_puthex(protocol.minor, 2);
    console_putc('\n');
    console_puts("  First port   : ");
    console_putu32(protocol.first_port);
    console_putc('\n');
    console_puts("  Port count   : ");
    console_putu32(protocol.port_count);
    console_putc('\n');
    console_puts("  Speed IDs    : ");
    console_putu32(protocol.speed_id_count);
    console_putc('\n');
    console_puts("  Slot type    : ");
    console_putu32(protocol.slot_type);
    console_putc('\n');
    console_puts("  Raw ports    : 0x");
    console_puthex(protocol.ports, 8);
    console_putc('\n');
  }
  console_puts("\nSupported protocols found: ");
  console_putu32(count);
  console_putc('\n');
}

static void cmd_usb_events(void) {
  unsigned int consumed = 0;

  for (unsigned int i = 0; i < 256U; i++) {
    xhci_event_t event;
    xhci_event_result_t status = xhci_event_poll(&event);

    if (status == XHCI_EVENT_ERROR) {
      console_puts("Event ring read failed.\n");
      return;
    }

    if (status == XHCI_EVENT_NONE)
      break;

    consumed++;

    console_puts("Event type ");
    console_putu32(event.type);
    console_puts(", completion ");
    console_putu32(event.completion_code);

    if (event.type == 34U) {
      console_puts(", port ");
      console_putu32(event.port_id);
    }

    console_putc('\n');

    if (!xhci_event_record(&event)) {
      console_puts("Unhandled event encountered; stopping.\n");
      return;
    }
  }

  xhci_event_stats_t stats;
  if (!xhci_event_get_stats(&stats)) {
    console_puts("Unable to read event statistics.\n");
    return;
  }

  console_puts("Events consumed: ");
  console_putu32(consumed);
  console_putc('\n');

  console_puts("Port change events recorded: ");
  console_putu32(stats.port_changes);
  console_putc('\n');

  console_puts("Last changed port: ");
  console_putu32(stats.last_port);
  console_putc('\n');
}

static void cmd_usb_descriptor(void) {
  const xhci_setup_packet_t setup = {
    .request_type = 0x80U,
    .request = 0x06U,
    .value = 0x0100U,
    .index = 0U,
    .length = 18U
  };
  xhci_control_info_t info;
  xhci_control_result_t status = xhci_control_in(&setup, &info);
  console_puts("USB Device Descriptor:\n");
  console_puts("  Result          : ");
  console_putu32((uint32_t)status);
  console_putc('\n');
  console_puts("  Completion code : ");
  console_putu32(info.completion_code);
  console_putc('\n');
  console_puts("  Event type      : ");
  console_putu32(info.event_type);
  console_putc('\n');
  console_puts("  Event parameter : 0x");
  console_puthex64(info.event_parameter);
  console_putc('\n');
  console_puts("  Expected TRB    : 0x");
  console_puthex64(info.status_trb);
  console_putc('\n');
  console_puts("  Elapsed         : ");
  console_putu32((uint32_t)info.elapsed_us);
  console_puts(" us\n");
  if (status != XHCI_CONTROL_OK) {
    console_puts("Device descriptor read: FAILED\n");
    return;
  }
  const uint8_t *d = info.data;
  console_puts("  Raw bytes       :");
  for (unsigned int i = 0; i < info.length; i++) {
    console_putc(' ');
    console_puthex(d[i], 2);
  }
  console_putc('\n');
  if (info.length != 18U || d[0] != 18U || d[1] != 1U) {
    console_puts("Device descriptor: INVALID\n");
    return;
  }
  console_puts("  USB version     : 0x");
  console_puthex((uint16_t)d[2] | ((uint16_t)d[3] << 8), 4);
  console_putc('\n');
  console_puts("  Device class    : 0x");
  console_puthex(d[4], 2);
  console_putc('\n');
  console_puts("  Device subclass : 0x");
  console_puthex(d[5], 2);
  console_putc('\n');
  console_puts("  Device protocol : 0x");
  console_puthex(d[6], 2);
  console_putc('\n');
  console_puts("  EP0 max packet  : ");
  console_putu32(d[7]);
  console_putc('\n');
  console_puts("  Vendor ID       : 0x");
  console_puthex((uint16_t)d[8] | ((uint16_t)d[9] << 8), 4);
  console_putc('\n');
  console_puts("  Product ID      : 0x");
  console_puthex((uint16_t)d[10] | ((uint16_t)d[11] << 8), 4);
  console_putc('\n');
  console_puts("  Device release  : 0x");
  console_puthex((uint16_t)d[12] | ((uint16_t)d[13] << 8), 4);
  console_putc('\n');
  console_puts("  Configurations  : ");
  console_putu32(d[17]);
  console_putc('\n');
  console_puts("Device descriptor read: PASS\n");
}

static uint16_t usb_u16(const uint8_t *data) {
  return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static bool usb_show_configuration(const uint8_t *data,
    unsigned int length) {
  unsigned int offset = 0;

  while (offset < length) {
    if (length - offset < 2U)
      return false;
    uint8_t size = data[offset];
    uint8_t type = data[offset + 1U];
    if (size < 2U || size > length - offset)
      return false;
    const uint8_t *d = &data[offset];
    console_puts("  Descriptor at ");
    console_putu32(offset);
    console_puts(": type 0x");
    console_puthex(type, 2);
    console_puts(", length ");
    console_putu32(size);
    console_putc('\n');
    switch (type) {
      case 0x02U:  // Configuration
        if (size < 9U)
          return false;
        console_puts("    Total length : ");
        console_putu32(usb_u16(&d[2]));
        console_putc('\n');
        console_puts("    Interfaces   : ");
        console_putu32(d[4]);
        console_putc('\n');
        console_puts("    Config value : ");
        console_putu32(d[5]);
        console_putc('\n');
        console_puts("    Attributes   : 0x");
        console_puthex(d[7], 2);
        console_putc('\n');
        console_puts("    Max power    : ");
        console_putu32((uint32_t)d[8] * 2U);
        console_puts(" mA\n");
        break;
      case 0x04U:  // Interface
        if (size < 9U)
          return false;
        console_puts("    Interface    : ");
        console_putu32(d[2]);
        console_putc('\n');
        console_puts("    Alternate    : ");
        console_putu32(d[3]);
        console_putc('\n');
        console_puts("    Endpoints    : ");
        console_putu32(d[4]);
        console_putc('\n');
        console_puts("    Class        : 0x");
        console_puthex(d[5], 2);
        console_putc('\n');
        console_puts("    Subclass     : 0x");
        console_puthex(d[6], 2);
        console_putc('\n');
        console_puts("    Protocol     : 0x");
        console_puthex(d[7], 2);
        console_putc('\n');
        break;
      case 0x05U:  // Endpoint
        if (size < 7U)
          return false;
        console_puts("    Address      : 0x");
        console_puthex(d[2], 2);
        console_putc('\n');
        console_puts("    Attributes   : 0x");
        console_puthex(d[3], 2);
        console_putc('\n');
        console_puts("    Max packet   : ");
        console_putu32(usb_u16(&d[4]) & 0x07ffU);
        console_putc('\n');
        console_puts("    Interval     : ");
        console_putu32(d[6]);
        console_putc('\n');
        break;
      default:
        // Unknown descriptors are not malformed merely
        // because this diagnostic does not decode them.
        break;
    }
    offset += size;
  }
  return offset == length;
}

static void cmd_usb_hubinfo(void) {
  xhci_control_info_t info;
  xhci_setup_packet_t setup = {
    .request_type = 0x80U,
    .request = 0x06U,
    .value = 0x0200U,
    .index = 0U,
    .length = 9U
  };

  console_puts("Reading USB configuration header...\n");
  xhci_control_result_t status = xhci_control_in(&setup, &info);
  if (status != XHCI_CONTROL_OK) {
    console_puts("FAILED (transfer result ");
    console_putu32((uint32_t)status);
    console_puts(", completion ");
    console_putu32(info.completion_code);
    console_puts(").\n");
    return;
  }
  if (info.length != 9U || info.data[0] < 9U ||
      info.data[1] != 0x02U) {
    console_puts("FAILED (invalid configuration header).\n");
    return;
  }
  uint16_t total = usb_u16(&info.data[2]);
  console_puts("  Configuration length: ");
  console_putu32(total);
  console_putc('\n');
  if (total < 9U || total > XHCI_CONTROL_MAX_DATA) {
    console_puts("Configuration exceeds supported transfer size.\n");
    return;
  }
  // Request precisely the number of bytes reported
  // by the Configuration Descriptor's wTotalLength.
  setup.length = total;
  console_puts("Reading complete configuration...\n");
  status = xhci_control_in(&setup, &info);
  if (status != XHCI_CONTROL_OK) {
    console_puts("FAILED (transfer result ");
    console_putu32((uint32_t)status);
    console_puts(", completion ");
    console_putu32(info.completion_code);
    console_puts(").\n");
    return;
  }
  if (info.length != total ||
      !usb_show_configuration(info.data, info.length)) {
    console_puts("FAILED (invalid configuration tree).\n");
    return;
  }
  // Class-specific GET_DESCRIPTOR(Hub).
  // bmRequestType A0 = Device-to-host, Class, Device.
  // Descriptor type 29 = USB 2.0 Hub Descriptor.
  setup.request_type = 0xa0U;
  setup.value = 0x2900U;
  setup.index = 0U;
  setup.length = 9U;
  console_puts("Reading USB 2.0 hub descriptor...\n");
  status = xhci_control_in(&setup, &info);
  if (status != XHCI_CONTROL_OK) {
    console_puts("FAILED (transfer result ");
    console_putu32((uint32_t)status);
    console_puts(", completion ");
    console_putu32(info.completion_code);
    console_puts(").\n");
    return;
  }
  const uint8_t *d = info.data;
  if (info.length != 9U || d[0] != 9U || d[1] != 0x29U || d[2] == 0U) {
    console_puts("FAILED (unexpected hub descriptor layout).\n");
    return;
  }
  uint16_t characteristics = usb_u16(&d[3]);
  console_puts("USB 2.0 Hub Descriptor:\n");
  console_puts("  Raw bytes       :");
  for (unsigned int i = 0; i < info.length; i++) {
    console_putc(' ');
    console_puthex(d[i], 2);
  }
  console_putc('\n');
  console_puts("  Downstream ports: ");
  console_putu32(d[2]);
  console_putc('\n');
  console_puts("  Characteristics : 0x");
  console_puthex(characteristics, 4);
  console_putc('\n');
  console_puts("  Power switching : ");
  switch (characteristics & 3U) {
    case 0U:
      console_puts("Ganged\n");
      break;
    case 1U:
      console_puts("Individual ports\n");
      break;
    default:
      console_puts("No power switching / reserved\n");
      break;
  }
  console_puts("  Overcurrent     : ");
  switch ((characteristics >> 3) & 3U) {
    case 0U:
      console_puts("Global\n");
      break;
    case 1U:
      console_puts("Per-port\n");
      break;
    default:
      console_puts("Not reported / reserved\n");
      break;
  }
  console_puts("  Power-on delay  : ");
  console_putu32((uint32_t)d[5] * 2U);
  console_puts(" ms\n");
  console_puts("  Hub controller  : ");
  console_putu32(d[6]);
  console_puts(" mA\n");
  console_puts("  Removable bitmap: 0x");
  console_puthex(d[7], 2);
  console_putc('\n');
  console_puts("  Power-mask byte : 0x");
  console_puthex(d[8], 2);
  console_putc('\n');
  console_puts("Hub descriptor read: PASS\n");
}

static void cmd_usb_devices(void) {
  usb_init_info_t info;

  if (!usb_get_init_info(&info)) {
    console_puts("USB subsystem state unavailable.\n");
    return;
  }
  console_puts("USB device enumeration:\n");
  console_puts("  Initialisation : ");
  if (info.result == USB_INIT_OK && info.address != 0U)
    console_puts("OK\n");
  else
    console_puts("FAILED or incomplete\n");
  console_puts("  Result code    : ");
  console_putu32((uint32_t)info.result);
  console_putc('\n');
  console_puts("  Detail         : 0x");
  console_puthex(info.detail, 8);
  console_putc('\n');
  if (info.address == 0U)
    return;
  console_puts("  Device         : Internal USB 2.0 hub\n");
  console_puts("  Root port      : ");
  console_putu32(info.root_port);
  console_putc('\n');
  console_puts("  Speed          : ");
  switch (info.speed) {
    case 3U:
      console_puts("High Speed (480 Mbit/s)\n");
      break;
    case 4U:
      console_puts("SuperSpeed (5 Gbit/s)\n");
      break;
    default:
      console_putu32(info.speed);
      console_puts(" (xHCI speed ID)\n");
      break;
  }
  console_puts("  Slot ID        : ");
  console_putu32(info.slot_id);
  console_putc('\n');
  console_puts("  USB address    : ");
  console_putu32(info.address);
  console_putc('\n');
  console_puts("  Slot state     : ");
  console_putu32(info.slot_state);
  console_puts(info.slot_state == 2U ? " (Addressed)\n" : "\n");
  console_puts("  EP0 state      : ");
  console_putu32(info.ep0_state);
  console_puts(info.ep0_state == 1U ? " (Running)\n" : "\n");
}

static void cmd_usb_configure(void) {
  static bool configured;

  xhci_control_info_t info;
  xhci_setup_packet_t setup = {
    .request_type = 0x80U,
    .request = 0x08U,  // GET_CONFIGURATION
    .value = 0U,
    .index = 0U,
    .length = 1U
  };
  xhci_control_result_t status = xhci_control_in(&setup, &info);
  if (status != XHCI_CONTROL_OK || info.length != 1U) {
    console_puts("GET_CONFIGURATION failed.\n");
    return;
  }
  console_puts("USB configuration before: ");
  console_putu32(info.data[0]);
  console_putc('\n');
  if (!configured) {
    setup.request_type = 0x00U;
    setup.request = 0x09U;  // SET_CONFIGURATION
    setup.value = 1U;
    setup.length = 0U;
    status = xhci_control_no_data(&setup, &info);
    console_puts("SET_CONFIGURATION result: ");
    console_putu32((uint32_t)status);
    console_puts(", completion: ");
    console_putu32(info.completion_code);
    console_putc('\n');
    if (status != XHCI_CONTROL_OK)
      return;
    configured = true;
  }
  setup.request_type = 0x80U;
  setup.request = 0x08U;
  setup.value = 0U;
  setup.length = 1U;
  status = xhci_control_in(&setup, &info);
  if (status != XHCI_CONTROL_OK || info.length != 1U) {
    console_puts("Verification GET_CONFIGURATION failed.\n");
    return;
  }
  console_puts("USB configuration after : ");
  console_putu32(info.data[0]);
  console_putc('\n');
  if (info.data[0] == 1U)
    console_puts("Hub configuration: PASS\n");
  else
    console_puts("Hub configuration: FAILED\n");
}

static void cmd_usb_hubports(void) {
  console_puts("Internal USB hub downstream ports:\n");

  for (uint8_t port = 1U; port <= 4U; port++) {
    usb_hub_port_t info;
    usb_hub_result_t result = usb_hub_port_status(port, &info);
    console_puts("  Port ");
    console_putu32(port);
    console_puts(":\n");
    if (result != USB_HUB_OK) {
      console_puts("    FAILED (hub result ");
      console_putu32((uint32_t)result);
      console_puts(", transfer ");
      console_putu32((uint32_t)info.transfer_result);
      console_puts(", completion ");
      console_putu32(info.completion_code);
      console_puts(")\n");
      continue;
    }
    uint16_t status = info.status;
    uint16_t change = info.change;
    console_puts("    Status     : 0x");
    console_puthex(status, 4);
    console_putc('\n');
    console_puts("    Change     : 0x");
    console_puthex(change, 4);
    console_putc('\n');
    console_puts("    Connected  : ");
    console_puts((status & (1U << 0)) ? "Yes\n" : "No\n");
    console_puts("    Enabled    : ");
    console_puts((status & (1U << 1)) ? "Yes\n" : "No\n");
    console_puts("    Suspended  : ");
    console_puts((status & (1U << 2)) ? "Yes\n" : "No\n");
    console_puts("    Overcurrent: ");
    console_puts((status & (1U << 3)) ? "Yes\n" : "No\n");
    console_puts("    Resetting  : ");
    console_puts((status & (1U << 4)) ? "Yes\n" : "No\n");
    console_puts("    Powered    : ");
    console_puts((status & (1U << 8)) ? "Yes\n" : "No\n");
    console_puts("    Speed      : ");
    if (status & (1U << 10))
      console_puts("High Speed\n");
    else if (status & (1U << 9))
      console_puts("Low Speed\n");
    else if (status & (1U << 0))
      console_puts("Full Speed or not yet established\n");
    else
      console_puts("No device\n");
    console_puts("    Changes    :");
    if (change == 0U) {
      console_puts(" None");
    } else {
      if (change & (1U << 0))
        console_puts(" Connection");
      if (change & (1U << 1))
        console_puts(" Enable");
      if (change & (1U << 2))
        console_puts(" Suspend");
      if (change & (1U << 3))
        console_puts(" Overcurrent");
      if (change & (1U << 4))
        console_puts(" Reset");
    }
    console_putc('\n');
  }
}

static void cmd_usb_hubpower(void) {
  usb_hub_port_t info;
  usb_hub_result_t result = usb_hub_power_on(&info);
  console_puts("Internal USB hub power:\n");
  console_puts("  Result          : ");
  console_putu32((uint32_t)result);
  console_putc('\n');
  console_puts("  Transfer result : ");
  console_putu32((uint32_t)info.transfer_result);
  console_putc('\n');
  console_puts("  Completion code : ");
  console_putu32(info.completion_code);
  console_putc('\n');
  console_puts("  Port 1 status   : 0x");
  console_puthex(info.status, 4);
  console_putc('\n');
  if (result != USB_HUB_OK) {
    console_puts("Hub power: FAILED or NOT READY\n");
    return;
  }
  console_puts("Hub power: PASS\n");
  // Inspect the entire power group, not just port 1.
  cmd_usb_hubports();
}

static void cmd_usb(const char *args) {
  // The VL805 is configured at boot.
  // This command merely inspects the controller.
  if (next_word_is(&args, "hubpower") && end_of_args(args)) {
    cmd_usb_hubpower();
    return;
  }
  if (next_word_is(&args, "hubports") && end_of_args(args)) {
    cmd_usb_hubports();
    return;
  }
  if (next_word_is(&args, "configure") && end_of_args(args)) {
    cmd_usb_configure();
    return;
  }
  if (next_word_is(&args, "devices") && end_of_args(args)) {
    cmd_usb_devices();
    return;
  }
  if (next_word_is(&args, "hubinfo") && end_of_args(args)) {
    cmd_usb_hubinfo();
    return;
  }
  if (next_word_is(&args, "descriptor") && end_of_args(args)) {
    cmd_usb_descriptor();
    return;
  }
  if (next_word_is(&args, "events") && end_of_args(args)) {
    cmd_usb_events();
    return;
  }
  if (next_word_is(&args, "protocols") && end_of_args(args)) {
    cmd_usb_protocols();
    return;
  }
  if (next_word_is(&args, "ports") && end_of_args(args)) {
    cmd_usb_ports();
    return;
  }
  if (next_word_is(&args, "noops") && end_of_args(args)) {
    cmd_usb_noops();
    return;
  }
  if (next_word_is(&args, "inspect") && end_of_args(args)) {
    cmd_usb_inspect();
    return;
  }
  if (next_word_is(&args, "ready") && end_of_args(args)) {
    cmd_usb_ready();
    return;
  }
  if (next_word_is(&args, "dma") && end_of_args(args)) {
    cmd_usb_dma();
    return;
  }
  if (next_word_is(&args, "state") && end_of_args(args)) {
    cmd_usb_state();
    return;
  }

  if (!end_of_args(args)) {
    console_puts(
    "Usage: usb [state | dma | ready | inspect | noops | ports | protocols |"
    " events | devices | descriptor | hubinfo | configure | hubports |"
    " hubpower]\n");
    return;
  }

  if (!pcie_xhci_ready()) {
    console_puts("VL805 PCI resources are not configured.\n");
    return;
  }

  xhci_info_t info;
  if (!xhci_probe(&info)) {
    console_puts("xHCI capability probe failed.\n");
    return;
  }
  console_puts("\nVL805 xHCI controller:\n");
  console_puts("  Capability length : ");
  console_putu32(info.capability_length);
  console_putc('\n');
  console_puts("  xHCI version      : 0x");
  console_puthex(info.version, 4);
  console_putc('\n');
  console_puts("  Maximum slots     : ");
  console_putu32(info.max_slots);
  console_putc('\n');
  console_puts("  Maximum interrupters: ");
  console_putu32(info.max_interrupters);
  console_putc('\n');
  console_puts("  Maximum ports     : ");
  console_putu32(info.max_ports);
  console_putc('\n');
  console_puts("  Doorbell offset   : 0x");
  console_puthex(info.doorbell_offset, 8);
  console_putc('\n');
  console_puts("  Runtime offset    : 0x");
  console_puthex(info.runtime_offset, 8);
  console_putc('\n');
}

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

static void cmd_fdt(const char *args) {
  if (next_word_is(&args, "dma") && end_of_args(args)) {
    cmd_fdt_dma();
    return;
  }
  if (next_word_is(&args, "memory") && end_of_args(args)) {
    cmd_fdt_memory();
    return;
  }

  if (!end_of_args(args)) {
    console_puts("Usage: fdt [memory]\n");
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

static void cmd_physmem(const char *args) {
  if (!end_of_args(args)) {
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

static void print_mmu_descriptor(uint64_t address) {
  mmu_descriptor_info_t info;

  if (!mmu_inspect(address, &info)) {
    console_puts("Descriptor inspection unavailable.\n");
    return;
  }

  console_puts("MMU descriptor inspection:\n");
  console_puts("  Virtual address : 0x");
  console_puthex64(info.virtual_address);
  console_putc('\n');
  console_puts("  L1 descriptor   : 0x");
  console_puthex64(info.l1_descriptor);
  console_putc('\n');

  if (info.level >= 2) {
    console_puts("  L2 descriptor   : 0x");
    console_puthex64(info.l2_descriptor);
    console_putc('\n');
  }

  if (info.level >= 3) {
    console_puts("  L3 descriptor   : 0x");
    console_puthex64(info.l3_descriptor);
    console_putc('\n');
  }

  if (info.kind == MMU_DESC_UNMAPPED) {
    console_puts("  Mapping         : Unmapped\n");
    console_puts("  Fault level     : ");
    console_putu32(info.level);
    console_putc('\n');
    return;
  }
  console_puts("  Mapping         : ");
  console_puts(info.kind == MMU_DESC_PAGE ?
      "4 KiB page\n" : "2 MiB block\n");
  console_puts("  Physical address: 0x");
  console_puthex64(info.physical_address);
  console_putc('\n');
  console_puts("  Attribute index : ");
  console_putu32(info.attr_index);
  if (info.attr_index == 0)
    console_puts(" (Device-nGnRnE)\n");
  else if (info.attr_index == 1)
    console_puts(" (Normal non-cacheable)\n");
  else if (info.attr_index == 2)
    console_puts(" (Normal write-back)\n");
  else
    console_puts(" (Unknown)\n");
  console_puts("  Access          : ");
  console_puts(info.read_only ?  "Read-only\n" : "Read/write\n");
  console_puts("  EL0 access      : ");
  console_puts((info.access_permissions & 1U) ?
      "Permitted by AP\n" : "Denied by AP\n");
  console_puts("  Access flag     : ");
  console_puts(info.access_flag ?  "Set\n" : "Clear\n");
  console_puts("  PXN             : ");
  console_puts(info.pxn ? "Yes\n" : "No\n");
  console_puts("  UXN             : ");
  console_puts(info.uxn ? "Yes\n" : "No\n");
  console_puts("  WXN             : ");
  mmu_info_t registers;
  mmu_get_info(&registers);
  console_puts((registers.sctlr & (1ULL << 19)) ?
      "Enabled\n" : "Disabled\n");
  bool el1_executable =
      !info.pxn && !(registers.sctlr & (1ULL << 19) && !info.read_only);
  console_puts("  EL1 execution   : ");
  console_puts(el1_executable ?
      "Permitted by leaf permissions\n" :
      "Prevented by leaf permissions/WXN\n");
}

static void cmd_mmu(const char *args) {
  // Inspect transalation descriptors
  if (next_word_is(&args, "inspect")) {
    uint64_t address;
    if (!next_hex64(&args, &address) ||
        !end_of_args(args)) {
      console_puts("Usage: mmu inspect <hex-address>\n");
      return;
    }
    print_mmu_descriptor(address);
    return;
  }

  // Non-destructive permissions diagnostics.
  if (next_word_is(&args, "permissions") && end_of_args(args)) {
    extern char __text_start[];
    extern char __rodata_start[];
    extern char __data_start[];
    extern char __bss_start[];
    extern char __stack_bottom[];
    console_puts("EL1 kernel write permissions:\n");
    console_puts("Text:\n");
    print_mmu_write_test( (uint64_t)(uintptr_t)__text_start);
    console_puts("Read-only data:\n");
    print_mmu_write_test( (uint64_t)(uintptr_t)__rodata_start);
    console_puts("Initialized data:\n");
    print_mmu_write_test( (uint64_t)(uintptr_t)__data_start);
    console_puts("BSS:\n");
    print_mmu_write_test( (uint64_t)(uintptr_t)__bss_start);
    console_puts("Kernel stack:\n");
    print_mmu_write_test( (uint64_t)(uintptr_t)__stack_bottom);
    return;
  }

  // Non-destructive translation diagnostics.
  // With no address, examine representative
  // addresses from each RAM bank and the
  // currently configured MMIO regions.
  if (next_word_is(&args, "translate")) {
    if (!mmu_enabled()) {
      console_puts("MMU is disabled.\n");
      return;
    }
    if (end_of_args(args)) {
      static const uint64_t test_addresses[] = {
        0x0000000000008000ULL,  // Bank 0
        0x000000003B400000ULL,  // Gap after Bank 0
        0x0000000040000000ULL,  // Bank 1
        0x00000000FC000000ULL,  // Peripheral MMIO
        0x0000000100000000ULL,  // Bank 2
        0x0000000180000000ULL,  // Bank 3
        0x0000000200000000ULL,  // Gap after Bank 3
        0x0000000600000000ULL   // VL805 MMIO
      };
      console_puts("EL1 stage-1 read translations:\n");
      for (unsigned int i = 0; i < sizeof(test_addresses) /
               sizeof(test_addresses[0]); i++) {
        print_mmu_translation(test_addresses[i]);
      }
      return;
    }
    uint64_t address;
    if (!next_hex64(&args, &address) || !end_of_args(args)) {
      console_puts(
        "Usage: mmu [inspect <hex-address> | "
        "permissions | translate [hex-address] | fault]\n");
      return;
    }
    console_puts("EL1 stage-1 read translation:\n");
    print_mmu_translation(address);
    return;
  }

  // Deliberately access an unmapped virtual address.
  // Address 0x3c000000 has no valid translation.
  // This is a destructive diagnostic: a successful
  // test enters exception_panic() and halts OrgOS.
    if (next_word_is(&args, "fault") && end_of_args(args)) {
    if (!mmu_enabled()) {
      console_puts("MMU is disabled; refusing fault test.\n");
      return;
    }

    console_puts(
        "MMU translation fault test.\n"
        "Reading unmapped virtual address 0x3c000000ULL...\n"
        "Expected result: synchronous EL1 data abort.\n"
        "OrgOS will halt. Reboot required.\n\n");

    // The volatile load ensures that the compiler
    // actually emits a memory access.
    // Because the virtual address is unmapped,
    // the load should never complete.
    volatile const uint32_t *address =
        (volatile const uint32_t *)(uintptr_t)0x3c000000ULL;

    uint32_t value = *address;

    // If execution reaches here, the fault test
    // did not behave as expected.
    console_puts("UNEXPECTED: read succeeded: 0x");
    console_puthex(value, 8);
    console_putc('\n');
    return;
  }

  if (!end_of_args(args)) {
    console_puts("Usage: mmu [translate [hex-address] | fault]\n");
    return;
  }

  // Existing MMU diagnostic code follows here.
  mmu_info_t info;
  mmu_get_info(&info);

  console_puts("AArch64 EL1 memory management:\n");
  console_puts("  Translation       : ");
  console_puts((info.sctlr & 1U) ?  "Enabled\n" : "Disabled\n");
  console_puts("  Data cache        : ");
  console_puts((info.sctlr & (1U << 2)) ?  "Enabled\n" : "Disabled\n");
  console_puts("  Instruction cache : ");
  console_puts((info.sctlr & (1U << 12)) ?  "Enabled\n" : "Disabled\n");
  console_puts("  Alignment check   : ");
  console_puts((info.sctlr & (1U << 1)) ?  "Enabled\n" : "Disabled\n");
  console_puts("  Write-execute-never: ");
  console_puts((info.sctlr & (1ULL << 19)) ?  "Enabled\n" : "Disabled\n");
  console_puts("  Physical addr bits: ");
  unsigned int pa_bits = mmu_physical_address_bits();
  if (pa_bits)
    console_putu32(pa_bits);
  else
    console_puts("Unknown encoding");
  console_putc('\n');
  console_puts("  4 KiB granule     : ");
  console_puts(mmu_granule_4k_supported() ?
      "Supported\n" : "Unsupported/reserved\n");
  console_puts("\nArchitectural registers:\n");
  console_puts("  ID_AA64MMFR0_EL1 : 0x");
  console_puthex64(info.id_aa64mmfr0);
  console_putc('\n');
  console_puts("  SCTLR_EL1        : 0x");
  console_puthex64(info.sctlr);
  console_putc('\n');
  console_puts("  TCR_EL1          : 0x");
  console_puthex64(info.tcr);
  console_putc('\n');
  console_puts("  MAIR_EL1         : 0x");
  console_puthex64(info.mair);
  console_putc('\n');
  console_puts("  TTBR0_EL1        : 0x");
  console_puthex64(info.ttbr0);
  console_putc('\n');
  console_puts("  TTBR1_EL1        : 0x");
  console_puthex64(info.ttbr1);
  console_putc('\n');
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

static void cmd_page(const char *args) {
  if (next_word_is(&args, "runtest") && end_of_args(args)) {
    cmd_page_run_test();
    return;
  }
  if (next_word_is(&args, "test") && end_of_args(args)) {
    cmd_page_test();
    return;
  }
  if (next_word_is(&args, "alloc") && end_of_args(args)) {
    paddr_t address;
    if (!page_alloc(&address)) {
      console_puts("Page allocation failed.\n");
      return;
    }
    console_puts("Allocated physical page: 0x");
    console_puthex64(address);
    console_putc('\n');
    return;
  }

  if (next_word_is(&args, "free")) {
    uint64_t address;
    if (!next_hex64(&args, &address) ||
        !end_of_args(args)) {
      console_puts("Usage: page free <hex-address>\n");
      return;
    }
    if (!page_free(address)) {
      console_puts("Page release rejected.\n");
      return;
    }
    console_puts("Released physical page: 0x");
    console_puthex64(address);
    console_putc('\n');
    return;
  }
  if (!end_of_args(args)) {
    console_puts(
        "Usage: page [alloc | free <hex-address> | test | runtest]\n");
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

static void cmd_cache(const char *args) {
  if (!end_of_args(args)) {
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

static void cmd_midi(const char *args) {
  midi_message_t msg;
  const char *p = args;
  unsigned int channel, a, b;
  bool valid = false;

  if (next_word_is(&p, "noteon")) {
    if (next_uint(&p, &channel) && next_uint(&p, &a) &&
        next_uint(&p, &b) && end_of_args(p))
      valid = midi_note_on(&msg, channel, a, b);

  } else if (next_word_is(&p, "noteoff")) {
    if (next_uint(&p, &channel) && next_uint(&p, &a) &&
        next_uint(&p, &b) && end_of_args(p))
      valid = midi_note_off(&msg, channel, a, b);

  } else if (next_word_is(&p, "cc")) {
    if (next_uint(&p, &channel) && next_uint(&p, &a) &&
        next_uint(&p, &b) && end_of_args(p))
      valid = midi_control_change(&msg, channel, a, b);

  } else if (next_word_is(&p, "pc")) {
    if (next_uint(&p, &channel) && next_uint(&p, &a) &&
        end_of_args(p))
      valid = midi_program_change(&msg, channel, a);
  }

  if (!valid) {
    console_puts("Invalid MIDI command or arguments.\n");
    console_puts("Usage:\n");
    console_puts("  midi noteon  channel note velocity\n");
    console_puts("  midi noteoff channel note velocity\n");
    console_puts("  midi cc      channel controller value\n");
    console_puts("  midi pc      channel program\n");
    return;
  }

  console_puts("MIDI: ");
  for (unsigned int i = 0; i < msg.length; i++) {
    if (i)
      console_putc(' ');
    console_puthex8(msg.data[i]);
  }
  console_putc('\n');
}

typedef struct {
  const char *name;
  console_command_fn handler;
  const char *description;
} console_command_t;


// Decode the RAM capacity field in the modern
// Raspberry Pi board revision encoding.
// Bits [22:20] identify the installed RAM size.
// Bit 23 distinguishes the modern encoding from
// the original revision-number scheme.
// Return zero if the code cannot be decoded.
static uint32_t board_ram_mib(uint32_t revision) {
  if (!(revision & (1U << 23)))
    return 0;

  static const uint32_t sizes[] = {
    256, 512, 1024, 2048, 4096, 8192, 16384
  };

  uint32_t code = (revision >> 20) & 7U;
  if (code >= sizeof(sizes) / sizeof(sizes[0]))
    return 0;

  return sizes[code];
}

static void cmd_memory(const char *args) {
  (void)args;

  uint32_t base;
  uint32_t size;

  console_puts("ARM memory allocation:\n");
  if (!mailbox_get_arm_memory(&base, &size)) {
    console_puts("  Firmware memory query failed.\n");
    return;
  }

  uint32_t revision;
  if (mailbox_get_board_revision(&revision)) {
    console_puts("  Board revision : 0x");
    console_puthex(revision, 8);
    console_putc('\n');
    if (revision & (1U << 23)) {
      uint32_t model = (revision >> 4) & 0xffU;
      uint32_t processor = (revision >> 12) & 0x0fU;
      uint32_t ram_mib = board_ram_mib(revision);
      console_puts("  Board model    : ");
      if (model == 0x11U)
        console_puts("Raspberry Pi 4 Model B\n");
      else {
        console_puts("Type 0x");
        console_puthex(model, 2);
        console_putc('\n');
      }
      console_puts("  Processor      : ");
      if (processor == 3U)
        console_puts("BCM2711\n");
      else {
        console_puts("Code ");
        console_putu32(processor);
        console_putc('\n');
      }
      console_puts("  Installed RAM  : ");
      if (ram_mib) {
        console_putu32(ram_mib);
        console_puts(" MiB\n");
      } else {
        console_puts("Unknown\n");
      }
    } else {
      console_puts("  Legacy revision encoding\n");
    }
  } else {
    console_puts("  Board revision query failed.\n");
  }
  console_putc('\n');

  uint64_t end = (uint64_t)base + size;
  uint64_t mapped = mmu_ram_mapped_bytes();
  console_puts("  Base address : 0x");
  console_puthex(base, 8);
  console_putc('\n');
  console_puts("  Size         : ");
  console_putu32(size / (1024U * 1024U));
  console_puts(" MiB\n");
  console_puts("  End address  : 0x");
  console_puthex64(end);
  console_puts(" (exclusive)\n");
  console_puts("  MMU-mapped RAM: ");
  console_putu64(mapped / (1024ULL * 1024ULL));
  console_puts(" MiB\n");
  if (mapped < size) {
    console_puts("  Note: A partial final RAM block is not mapped.\n");
  }
}

// Commands will be added here as they are implemented.
// The NULL entry terminates the table.
static const console_command_t commands[] = {
  { "mailbox", cmd_mailbox, "Test firmware property mailbox communication" },
  { "timer",  cmd_timer,  "Display system timer information" },
  { "uptime", cmd_uptime, "Show time since OrgOS started" },
  { "help",   cmd_help,   "List available commands" },
  { "memory", cmd_memory, "Display firmware ARM memory allocation" },
  { "irq", cmd_irq, "Display interrupt statistics" },
  { "fdt", cmd_fdt, "Inspect firmware Device Tree, memory and DMA" },
  { "physmem", cmd_physmem, "Display physical RAM and reservations" },
  { "mmu", cmd_mmu, "Display AArch64 memory management state" },
  { "page", cmd_page, "Inspect, allocate or release physical pages" },
  { "cache", cmd_cache, "Display CPU cache configuration" },
  { "pci", cmd_pci, "Inspect PCIe controller and DMA configuration" },
  { "dma", cmd_dma, "Test DMA buffer ownership" },
  { "usb", cmd_usb, "Inspect VL805 xHCI capabilities" },
  { "midi", cmd_midi, "Construct and inspect MIDI messages" },
  { NULL, NULL, NULL }
};

static void cmd_help(const char *args) {
  (void)args;

  console_puts("Available commands:\n\n");

  for (size_t i = 0; commands[i].name != NULL; i++) {
    console_puts("  ");
    console_puts(commands[i].name);

    // Align descriptions at column 12
    size_t length = 0;
    const char *p = commands[i].name;

    while (*p++) {
      length++;
    }

    do {
      console_putc(' ');
      length++;
    } while (length < 10);

    console_puts(commands[i].description);
    console_putc('\n');
  }
}

static bool is_space(char c) {
  return c == ' '  ||
         c == '\t' ||
         c == '\n' ||
         c == '\r' ||
         c == '\v' ||
         c == '\f';
}

static bool string_equal(const char *a, const char *b) {
  while (*a && *b) {
    if (*a != *b)
      return false;

    a++;
    b++;
  }

  return *a == *b;
}

static void console_prompt(void) {
  console_puts("OrgOS> ");
}

// Dispatch a completed, NUL-terminated input line.
// The buffer may be modified in place.
static void console_dispatch(char *line) {
  char *command = line;
  char *args;

  // Skip leading whitespace.
  while (is_space(*command))
    command++;

  // An empty line is not a command.
  if (*command == '\0')
    return;

  // Locate the end of the first word.
  char *p = command;

  while (*p && !is_space(*p))
    p++;

  if (*p) {
    // Terminate the command name and skip
    // whitespace preceding its arguments.
    *p++ = '\0';

    while (is_space(*p))
      p++;
  }

  args = p;

  // Look up the command.
  for (size_t i = 0; commands[i].name != NULL; i++) {
    if (string_equal(command, commands[i].name)) {
      commands[i].handler(args);
      return;
    }
  }

  console_puts("No such command.\n");
}

void console_run(void) {
  char line[CONSOLE_LINE_SIZE];
  size_t length = 0;
  bool last_was_cr = false;

  console_puts("Console ready.\n");
  console_prompt();

  for (;;) {
    unsigned char c = (unsigned char)uart_getc();

    // Treat CRLF as one newline.
    if (c == '\n' && last_was_cr) {
      last_was_cr = false;
      continue;
    }

    last_was_cr = (c == '\r');

    // A completed input line.
    if (c == '\r' || c == '\n') {
      console_puts("\n");

      line[length] = '\0';
      console_dispatch(line);

      length = 0;
      console_prompt();
      continue;
    }

    // Backspace (0x08) or DEL (0x7f).
    // Remove one character from the buffer
    // and erase it from the terminal.
    if (c == '\b' || c == 0x7f) {
      if (length > 0) {
        length--;
        console_puts("\b \b");
      }

      continue;
    }

    // Convert TAB to one ordinary space.
    // The display and buffer must agree.
    if (c == '\t')
      c = ' ';

    // Ignore unsupported control characters
    // and non-ASCII input.
    if (c < 0x20 || c > 0x7e)
      continue;

    // Reserve the final byte for '\0'.
    // If the line is full, ring the terminal
    // bell and refuse the additional character.
    // The user can still use backspace.
    if (length >= sizeof(line) - 1) {
      console_putc('\a');
      continue;
    }

    // Append and echo the character.
    line[length++] = (char)c;
    console_putc((char)c);
  }
}

void console_putc(char c) {
  if (c == '\n')
    uart_putc('\r');

  uart_putc(c);
}

void console_puts(const char *s) {
  while (*s)
    console_putc(*s++);
}

void console_putu32(uint32_t value) {
  char buffer[10];
  unsigned int length = 0;

  do {
    buffer[length++] = '0' + (value % 10U);
    value /= 10U;
  } while (value);

  while (length)
    uart_putc(buffer[--length]);
}

void console_putu64(uint64_t value) {
  char buffer[20];
  unsigned int length = 0;

  do {
    buffer[length++] = '0' + (value % 10U);
    value /= 10U;
  } while (value);

  while (length)
    uart_putc(buffer[--length]);
}

