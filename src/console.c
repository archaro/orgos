
#include <stddef.h>
#include <stdarg.h>

#include "format.h"
#include "console.h"
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
#include "usb_midi.h"
#include "usb_enumerate.h"
#include "usb_midi_test.h"
#include "midi.h"

#define CONSOLE_LINE_SIZE 256

typedef void (*console_command_fn)(const char *args);

static bool end_of_args(const char *p) {
  while (*p == ' ' || *p == '\t')
    p++;

  return *p == '\0';
}


static void console_format_putc(char c, void *context) {
  (void)context;
  console_putc(c);
}

void console_printf(const char *fmt, ...) {
  va_list args;

  va_start(args, fmt);
  format_vprintf(console_format_putc, NULL, fmt, args);
  va_end(args);
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

static void usb_test_delay_ms(uint32_t milliseconds) {
  uint64_t started = timer_now_us();
  uint64_t duration = (uint64_t)milliseconds * 1000ULL;
  while (timer_now_us() - started < duration) {
    usb_service();
    timer_delay_ms(1U);
  }
}

static void cmd_usb_test_quiesce(void) {
  bool quiesced = usb_device_test_quiesce();
  console_printf("USB test quiesce: %s\n", quiesced ? "OK" : "NOT READY");
}

static void cmd_usb_test_ep0match(void) {
  bool passed = xhci_ep0_match_selftest();
  console_printf("EP0 TD matching: %s\n", passed ? "PASS" : "FAILED");
}

static void cmd_usb_test_enumabort(void) {
  bool armed = usb_enumeration_test_abort_arm();
  console_printf("USB enumeration test abort: %s\n",
      armed ? "ARMED" : "NOT READY");
}

static void cmd_usb_test_enumconfigabort(void) {
  bool armed = usb_enumeration_test_config_abort_arm();

  console_printf("USB configuration test abort: %s\n",
      armed ? "ARMED" : "NOT READY");
}

static void cmd_usb_test_noops(void) {
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
      usb_service();
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

static const char *usb_device_state_name(usb_device_state_t state) {
  switch (state) {
    case USB_DEVICE_DETACHED:
      return "Detached";
    case USB_DEVICE_DETECTED:
      return "Detected";
    case USB_DEVICE_ENUMERATING:
      return "Enumerating";
    case USB_DEVICE_CONFIGURED:
      return "Configured";
    case USB_DEVICE_ACTIVE:
      return "Active";
    case USB_DEVICE_DISCONNECTING:
      return "Disconnecting";
    case USB_DEVICE_FAILED:
      return "Failed";
    default:
      return "Unknown";
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


static void cmd_usb_events(void) {
  xhci_event_stats_t stats;
  if (!xhci_event_get_stats(&stats)) {
    console_puts("Unable to read USB event statistics.\n");
    return;
  }
  console_puts("USB event statistics:\n");
  console_puts("  Port changes     : ");
  console_putu32(stats.port_changes);
  console_putc('\n');
  console_puts("  Last changed port: ");
  console_putu32(stats.last_port);
  console_putc('\n');
  console_puts("  Last event status: 0x");
  console_puthex(stats.last_status, 8);
  console_putc('\n');
  console_puts("  Last event ctrl  : 0x");
  console_puthex(stats.last_control, 8);
  console_putc('\n');
  console_printf("  Routed commands : %u\n",
      (unsigned int)stats.command_events);
  console_printf("  Routed transfers: %u\n",
      (unsigned int)stats.transfer_events);
  console_printf("  Event read errors: %u\n",
      (unsigned int)stats.read_errors);
  console_printf("  Unhandled events: %u\n",
      (unsigned int)stats.unhandled_events);
  console_printf("    Unsupported   : %u\n",
      (unsigned int)stats.unsupported_events);
  console_printf("    Unmatched     : %u\n",
      (unsigned int)stats.unmatched_events);
  console_printf("    Duplicate     : %u\n",
      (unsigned int)stats.duplicate_events);
  if (stats.unhandled_events != 0U) {
    console_puts("  Last unhandled event:\n");
    console_printf("    Parameter: %016llx\n",
        (unsigned long long)stats.last_unhandled_parameter);
    console_printf("    Status   : %08x\n",
        (unsigned int)stats.last_unhandled_status);
    console_printf("    Control  : %08x\n",
        (unsigned int)stats.last_unhandled_control);
  }
  console_printf("  Port queue depth: %u\n",
      (unsigned int)stats.port_queue_depth);
  console_printf("  Port queue peak : %u\n",
      (unsigned int)stats.port_queue_peak);
  console_printf("  Port queue overflows: %u\n",
      (unsigned int)stats.port_queue_overflows);
  console_printf("  Service calls   : %llu\n",
      (unsigned long long)stats.service_calls);
  console_printf("  Service events  : %llu\n",
      (unsigned long long)stats.service_events);
  console_printf("  Sync IN services: %llu\n",
      (unsigned long long)stats.sync_in_services);
  console_printf("  Service failures: %u\n",
      (unsigned int)stats.service_failures);
  usb_port_monitor_info_t ports = {0};
  if (usb_get_port_monitor_info(&ports)) {
    console_printf("  Port events processed: %u\n",
        (unsigned int)ports.processed);
    console_printf("  Port service failures: %u\n",
        (unsigned int)ports.failures);
    console_printf("  Last serviced port   : %u\n",
        (unsigned int)ports.last_port);
    console_printf("  Last completion code : %u\n",
        (unsigned int)ports.last_completion_code);
    console_printf("  Last PORTSC          : %08x\n",
        (unsigned int)ports.last_portsc);
    console_printf("  Internal hub fault   : %s\n",
        ports.root_fault ? "Yes" : "No");
  }
}

static void cmd_usb_scan(void) {
  usb_hub_scan_info_t scan = {0};
  usb_hub_result_t result = usb_hub_scan(&scan);
  if (result != USB_HUB_OK) {
    console_printf("USB scan failed: port %u, result %u\n",
        (unsigned int)scan.failed_port, (unsigned int)result);
    return;
  }
  console_puts("USB downstream discovery:\n");
  for (uint8_t port = 1U; port <= scan.ports; port++) {
    uint16_t bit = (uint16_t)(1U << (port - 1U));
    console_printf("  Port %u: ", (unsigned int)port);
    if (scan.overcurrent & bit)
      console_puts("Overcurrent");
    else if (scan.connected & bit)
      console_puts("Connected");
    else
      console_puts("Empty");
    if (scan.enabled & bit)
      console_puts(", enabled");
    if (scan.changed & bit)
      console_puts(", change pending");
    console_putc('\n');
  }
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
    if (!(status & (1U << 0)))
      console_puts("No device\n");
    else if (!(status & (1U << 1)))
      console_puts("Not yet established\n");
    else if (status & (1U << 10))
      console_puts("High Speed (480 Mb/s)\n");
    else if (status & (1U << 9))
      console_puts("Low Speed (1.5 Mb/s)\n");
    else
      console_puts("Full Speed (12 Mb/s)\n");
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

static void cmd_usb_device_check(void) {
  usb_device_t device = {0};
  if (!usb_get_device(&device) || device.state == USB_DEVICE_DETACHED) {
    console_puts("No tracked downstream device.\n");
    return;
  }
  usb_hub_port_t port = {0};
  usb_hub_result_t result = usb_hub_port_status(device.parent_port, &port);
  if (result != USB_HUB_OK) {
    console_printf("Hub port %u: status read failed (%u).\n",
        (unsigned int)device.parent_port, (unsigned int)result);
    return;
  }
  bool connected = (port.status & (1U << 0)) != 0U;
  bool enabled = (port.status & (1U << 1)) != 0U;
  bool powered = (port.status & (1U << 8)) != 0U;
  bool changed = (port.change & (1U << 0)) != 0U;
  console_printf("USB device connection check:\n");
  console_printf("  Hub port       : %u\n", (unsigned int)device.parent_port);
  console_printf("  Recorded state : %s\n",
      usb_device_state_name(device.state));
  console_printf("  Port status    : 0x%04x\n", (unsigned int)port.status);
  console_printf("  Port change    : 0x%04x\n", (unsigned int)port.change);
  console_printf("  Connected      : %s\n", connected ? "Yes" : "No");
  console_printf("  Enabled        : %s\n", enabled ? "Yes" : "No");
  console_printf("  Powered        : %s\n", powered ? "Yes" : "No");
  console_printf("  Conn. changed  : %s\n", changed ? "Yes" : "No");
  if (!connected || !enabled || !powered || changed)
    console_puts("  Result         : Device requires attention\n");
  else
    console_puts("  Result         : Port appears operational\n");
}

static void cmd_usb_devices(void) {
  usb_device_t device = {0};
  console_puts("USB device inventory:\n");
  if (!usb_get_device(&device)) {
    console_puts("  No device record available.\n");
    return;
  }
  console_printf("  State          : %s\n",
      usb_device_state_name(device.state));
  console_printf("  xHCI quiesced  : %s\n",
      xhci_downstream_is_quiesced() ? "Yes" : "No");
  if (device.state == USB_DEVICE_DETACHED) {
    console_puts("  No enumerated downstream device.\n");
    return;
  }
  console_printf("  Root port      : %u\n", (unsigned int)device.root_port);
  console_printf("  Parent slot    : %u\n", (unsigned int)device.parent_slot);
  console_printf("  Parent port    : %u\n", (unsigned int)device.parent_port);
  console_printf("  Speed          : %s\n",
      usb_speed_name((uint8_t)device.speed));
  console_printf("  Slot ID        : %u\n", (unsigned int)device.slot_id);
  console_printf("  USB address    : %u\n", (unsigned int)device.address);
  console_printf("  Vendor ID      : 0x%04x\n",
      (unsigned int)device.vendor_id);
  console_printf("  Product ID     : 0x%04x\n",
      (unsigned int)device.product_id);
  console_printf("  Generation     : %u\n",
      (unsigned int)device.generation);
}

static void cmd_usb_enumerate(void) {
  usb_enum_info_t info = {0};
  usb_enum_result_t result;
  if (usb_get_enumeration(&info))
    result = info.result;
  else
    result = usb_enumerate(&info);
  console_printf("USB enumeration:\n");
  console_printf("  Result          : %u\n", (unsigned int)result);
  console_printf("  Detail          : %u\n", info.detail);
  if (result == USB_ENUM_NOT_READY) {
    console_puts("USB enumeration: NOT READY\n");
    return;
  }

  if (result != USB_ENUM_OK) {
    console_puts("USB enumeration: FAILED\n");
    return;
  }
  console_printf("  Hub ports       : %u\n", (unsigned int)info.hub_ports);
  console_printf("  Connected port  : %u\n", (unsigned int)info.hub_port);
  console_printf("  Device slot     : %u\n", (unsigned int)info.slot_id);
  console_printf("  USB address     : %u\n", (unsigned int)info.address);
  console_printf("  Vendor ID       : 0x%04x\n",
      (unsigned int)info.vendor_id);
  console_printf("  Product ID      : 0x%04x\n",
      (unsigned int)info.product_id);
  if (info.quirks & USB_MIDI_QUIRK_STARTUP_EOX)
    console_puts("  MIDI quirk      : startup EOX\n");
  else
    console_puts("  MIDI quirk      : none\n");
  console_printf("  Config length   : %u\n",
      (unsigned int)info.configuration_length);
  console_printf("  MIDI interface  : %u\n",
      (unsigned int)info.midi.midi_interface);
  console_printf("  Bulk OUT        : 0x%02x, MPS %u\n",
      (unsigned int)info.midi.bulk_out,
      (unsigned int)info.midi.bulk_out_mps);
  console_printf("  Bulk IN         : 0x%02x, MPS %u\n",
      (unsigned int)info.midi.bulk_in,
      (unsigned int)info.midi.bulk_in_mps);
  console_printf("USB enumeration: PASS\n");
}

static const char *usb_enum_stage_name(usb_enum_stage_t stage) {
  switch (stage) {
    case USB_ENUM_STAGE_NONE:
      return "None";
    case USB_ENUM_STAGE_DISCOVER:
      return "Port discovery";
    case USB_ENUM_STAGE_ENABLE_SLOT:
      return "Enable Slot";
    case USB_ENUM_STAGE_PREPARE:
      return "Prepare contexts";
    case USB_ENUM_STAGE_ADDRESS:
      return "Address Device";
    case USB_ENUM_STAGE_DEVICE_DESCRIPTOR:
      return "Device descriptor";
    case USB_ENUM_STAGE_CONFIGURATION_DESCRIPTOR:
      return "Configuration descriptor";
    case USB_ENUM_STAGE_SELECT_CONFIGURATION:
      return "Select configuration";
    case USB_ENUM_STAGE_CONFIGURE_BULK:
      return "Configure Bulk endpoints";
    case USB_ENUM_STAGE_MIDI_QUIRK:
      return "MIDI startup quirk";
    case USB_ENUM_STAGE_COMPLETE:
      return "Complete";
    default:
      return "Unknown";
  }
}

static const char *xhci_ep_state_name(uint8_t state) {
  switch (state) {
    case 0U: return "Disabled";
    case 1U: return "Running";
    case 2U: return "Halted";
    case 3U: return "Stopped";
    case 4U: return "Error";
    default: return "Unknown";
  }
}

static void cmd_usb_enumstate(void) {
  usb_enum_info_t info = {0};
  if (!usb_get_enumeration(&info)) {
    console_puts("No enumeration attempt recorded.\n");
    return;
  }
  console_puts("USB enumeration state:\n");
  console_printf("  Result       : %u\n", (unsigned int)info.result);
  console_printf("  Detail       : 0x%08x\n", (unsigned int)info.detail);
  console_printf("  Stage        : %s\n", usb_enum_stage_name(info.stage));
  console_printf("  Hub port     : %u\n", (unsigned int)info.hub_port);
  console_printf("  Slot attempt : %s\n", info.slot_attempted ? "Yes" : "No");
  console_printf("  Slot known   : %s\n", info.slot_confirmed ? "Yes" : "No");
  console_printf("  Slot ID      : %u\n", (unsigned int)info.slot_id);
  console_printf("  Prepare tried: %s\n",
      info.prepare_attempted ? "Yes" : "No");
  console_printf("  Address tried: %s\n",
      info.address_attempted ? "Yes" : "No");
  console_printf("  Bulk tried   : %s\n", info.bulk_attempted ? "Yes" : "No");
  console_printf("  EP0 failed   : %s\n",
      info.ep0_operation_failed ? "Yes" : "No");
  console_printf("  EP0 result   : %u\n", (unsigned int)info.ep0_result);
  xhci_downstream_transfer_state_t transfers = {0};
  if (xhci_downstream_transfer_state(&transfers) &&
      transfers.slot_id == info.slot_id && info.slot_id != 0U) {
    if (transfers.ep0_state_valid) {
      console_printf("  EP0 endpoint : %s (%u)\n",
          xhci_ep_state_name(transfers.ep0_state),
          (unsigned int)transfers.ep0_state);
    } else {
      console_puts("  EP0 endpoint : Unavailable\n");
    }
    console_printf("  EP0 submitted: %s\n",
        transfers.ep0_submitted ? "Yes" : "No");
    console_printf("  EP0 complete : %s\n",
        transfers.ep0_completion_seen ? "Yes" : "No");
    console_printf("  EP0 code     : %u\n",
        (unsigned int)transfers.ep0_completion_code);
    console_printf("  EP0 latched  : %s\n",
        transfers.ep0_failed ? "Yes" : "No");
  }
}

static const char *usb_recovery_result_name(
    usb_recovery_result_t result) {
  switch (result) {
    case USB_RECOVERY_READY:
      return "Ready";
    case USB_RECOVERY_NO_FAILED_DEVICE:
      return "No failed device";
    case USB_RECOVERY_NO_CONFIRMED_SLOT:
      return "Slot not confirmed";
    case USB_RECOVERY_ADDRESS_UNCONFIRMED:
      return "Address not confirmed";
    case USB_RECOVERY_BULK_ATTEMPTED:
      return "Bulk configuration attempted";
    case USB_RECOVERY_ALREADY_QUIESCED:
      return "Teardown already started";
    case USB_RECOVERY_EP0_ERROR_COMPLETION:
      return "EP0 error completion";
    case USB_RECOVERY_EP0_OUTCOME_UNKNOWN:
      return "EP0 outcome uncertain";
    case USB_RECOVERY_EP0_FAULT:
      return "EP0 operation fault";
    case USB_RECOVERY_TRANSFER_BUSY:
      return "Transfers still active";
    case USB_RECOVERY_TRANSFER_STATE_UNAVAILABLE:
      return "Transfer state unavailable";
    default:
      return "Unknown";
  }
}

static void cmd_usb_recover_status(void) {
  usb_recovery_info_t info = {0};
  if (!usb_enumeration_recovery_inspect(&info)) {
    console_puts("USB recovery inspection failed.\n");
    return;
  }
  console_puts("USB enumeration recovery:\n");
  console_printf("  Decision   : %s\n",
      usb_recovery_result_name(info.result));
  console_printf("  Generation : %u\n", (unsigned int)info.generation);
  console_printf("  Slot       : %u\n", (unsigned int)info.slot_id);
  console_printf("  Submitted  : %s\n", info.ep0_submitted ? "Yes" : "No");
  console_printf("  Completed  : %s\n",
      info.ep0_completion_seen ? "Yes" : "No");
  console_printf("  Code       : %u\n",
      (unsigned int)info.ep0_completion_code);

  if (info.ep0_state_valid) {
    console_printf("  EP0 state  : %s (%u)\n",
        xhci_ep_state_name(info.ep0_state), (unsigned int)info.ep0_state);
  } else {
    console_puts("  EP0 state  : Unavailable\n");
  }
}

static void cmd_usb_recover(void) {
  if (!usb_service_available()) {
    console_puts("USB recovery: SERVICE UNAVAILABLE\n");
    return;
  }
  if (!usb_enumeration_recovery_ready()) {
    console_puts("USB recovery: NOT READY\n");
    return;
  }
  if (!usb_enumeration_recovery_begin()) {
    console_puts("USB recovery: FAILED\n");
    return;
  }
  if (!usb_lifecycle_arm()) {
    console_puts("USB recovery: QUIESCED, NOT ARMED\n");
    return;
  }
  console_puts("USB recovery: STARTED\n");
}

static void cmd_usb_stopout(void) {
  xhci_stop_info_t info = {0};
  xhci_stop_result_t result = xhci_downstream_stop_bulk_out(&info);
  console_printf("Stop Bulk OUT: %u\n", (unsigned int)result);
  console_printf("  Slot       : %u\n", (unsigned int)info.slot_id);
  console_printf("  Endpoint   : %u\n", (unsigned int)info.endpoint_id);
  console_printf("  Before     : %u\n", (unsigned int)info.before_state);
  console_printf("  After      : %u\n", (unsigned int)info.after_state);
  console_printf("  Command    : %u\n", (unsigned int)info.command_result);
  console_printf("  Completion : %u\n", (unsigned int)info.completion_code);
  console_printf("  Stop event : %s\n",
      info.stop_event_received ? "Yes" : "No");
  console_printf("  Event code : %u\n", (unsigned int)info.stop_event_code);
  console_printf("  Event TRB  : %016llx\n",
      (unsigned long long)info.stop_event_trb);
}

static void cmd_usb_stopin(void) {
  xhci_stop_info_t info = {0};
  xhci_stop_result_t result = xhci_downstream_stop_bulk_in(&info);
  console_printf("Stop Bulk IN: %u\n", (unsigned int)result);
  console_printf("  Slot       : %u\n", (unsigned int)info.slot_id);
  console_printf("  Endpoint   : %u\n", (unsigned int)info.endpoint_id);
  console_printf("  Before     : %u\n", (unsigned int)info.before_state);
  console_printf("  After      : %u\n", (unsigned int)info.after_state);
  console_printf("  Command    : %u\n", (unsigned int)info.command_result);
  console_printf("  Completion : %u\n", (unsigned int)info.completion_code);
  console_printf("  Stop event : %s\n",
      info.stop_event_received ? "Yes" : "No");
  console_printf("  Event code : %u\n", (unsigned int)info.stop_event_code);
  console_printf("  Event TRB  : %016llx\n",
      (unsigned long long)info.stop_event_trb);
}

static void cmd_usb_disable(void) {
  xhci_disable_info_t info = {0};
  xhci_disable_result_t result = xhci_downstream_disable(&info);
  console_printf("Disable Slot: %u\n", (unsigned int)result);
  console_printf("  Slot       : %u\n", (unsigned int)info.slot_id);
  console_printf("  Command    : %u\n", (unsigned int)info.command_result);
  console_printf("  Completion : %u\n", (unsigned int)info.completion_code);
}

static void cmd_usb_detach(void) {
  if (usb_device_finalize_disconnect()) {
    console_puts("USB device detached.\n");
  } else {
    console_puts("USB detach not ready.\n");
  }
}

static void cmd_usb_cleanup(void) {
  xhci_cleanup_info_t info = {0};
  xhci_cleanup_result_t result = xhci_downstream_cleanup(&info);
  console_printf("USB cleanup: %u\n", (unsigned int)result);
  console_printf("  Slot     : %u\n", (unsigned int)info.slot_id);
  console_printf("  Buffers  : %u\n", (unsigned int)info.buffers_released);
  console_printf("  Pages    : %u\n", (unsigned int)info.pages_released);
  console_printf("  Cleaned  : %s\n",
      xhci_downstream_is_cleaned() ? "Yes" : "No");
}

static void cmd_usb_retirein(void) {
  xhci_bulk_in_retire_result_t result = xhci_bulk_in_stream_retire();
  console_printf("Bulk IN retirement: %u\n", (unsigned int)result);
}

static void cmd_usb_reconcilein(void) {
  xhci_in_reconcile_result_t result = xhci_bulk_in_quiesced_reconcile();
  console_printf("Bulk IN reconciliation: %u\n", (unsigned int)result);
}

static void cmd_usb_recycle(void) {
  usb_device_t device = {0};
  if (!usb_get_device(&device) || device.state != USB_DEVICE_DETACHED) {
    console_puts("USB device is not detached.\n");
    return;
  }
  xhci_recycle_result_t result = xhci_downstream_recycle();
  console_printf("USB recycle: %u\n", (unsigned int)result);
}

static void cmd_usb_rearm(void) {
  if (usb_enumeration_rearm())
    console_puts("USB enumeration rearmed.\n");
  else
    console_puts("USB enumeration rearm not ready.\n");
}

static void cmd_usb_transfers(void) {
  xhci_downstream_transfer_state_t state = {0};
  if (!xhci_downstream_transfer_state(&state)) {
    console_puts("Transfer state unavailable.\n");
    return;
  }
  console_puts("Downstream xHCI transfers:\n");
  console_printf("  Slot          : %u\n", (unsigned int)state.slot_id);
  console_printf("  Quiesced      : %s\n", state.quiesced ? "Yes" : "No");
  console_printf("  Active waiters: %u\n",
      (unsigned int)state.active_waiters);
  console_printf("  EP0 active    : %s\n", state.ep0_active ? "Yes" : "No");
  console_printf("  EP0 fault     : %s\n", state.ep0_failed ? "Yes" : "No");
  console_printf("  EP0 submitted : %s\n",
      state.ep0_submitted ? "Yes" : "No");
  console_printf("  EP0 complete  : %s\n",
      state.ep0_completion_seen ? "Yes" : "No");
  console_printf("  EP0 code      : %u\n",
      (unsigned int)state.ep0_completion_code);
  if (state.ep0_state_valid) {
    console_printf("  EP0 endpoint  : %s (%u)\n",
        xhci_ep_state_name(state.ep0_state), (unsigned int)state.ep0_state);
  } else {
    console_puts("  EP0 endpoint  : Unavailable\n");
  }
  console_printf("  Bulk OUT      : %s\n",
      state.bulk_out_active ? "Active" : "Idle");
  console_printf("  Bulk IN stream: %s\n",
      state.bulk_in_started ? "Started" : "Not started");
  console_printf("  Bulk IN TD    : %s\n",
      state.bulk_in_pending ? "Pending" : "Not pending");
  console_printf("  IN completed  : %s\n",
      state.bulk_in_completed ? "Yes" : "No");
  console_printf("  Bulk IN failed: %s\n",
      state.bulk_in_failed ? "Yes" : "No");
  console_printf("  IN completion : %u\n",
	  (unsigned int)state.bulk_in_completion_code);
  if (state.endpoint_states_valid) {
    console_printf("  IN endpoint   : %s (%u)\n",
        xhci_ep_state_name(state.bulk_in_ep_state),
        (unsigned int)state.bulk_in_ep_state);
    console_printf("  OUT endpoint  : %s (%u)\n",
        xhci_ep_state_name(state.bulk_out_ep_state),
        (unsigned int)state.bulk_out_ep_state);
  } else {
    console_puts("  Endpoint state: Unavailable\n");
  }
}

static const char *usb_reenum_result_name(usb_reenum_result_t result) {
  switch (result) {
    case USB_REENUM_READY:
      return "Ready";
    case USB_REENUM_SERVICE_FAILED:
      return "USB service unavailable";
    case USB_REENUM_HUB_FAILED:
      return "Hub monitoring failed";
    case USB_REENUM_NO_HISTORY:
      return "No previous generation";
    case USB_REENUM_DEVICE_OWNED:
      return "Previous device still owns resources";
    case USB_REENUM_EXECUTOR_ACTIVE:
      return "Lifecycle executor active";
    case USB_REENUM_EXECUTOR_FAILED:
      return "Lifecycle executor failed";
    case USB_REENUM_NOT_REARMED:
      return "Enumeration not rearmed";
    case USB_REENUM_XHCI_BUSY:
      return "xHCI teardown incomplete";
    case USB_REENUM_PORT_UNAVAILABLE:
      return "Port status unavailable";
    case USB_REENUM_DISCONNECTED:
      return "Port disconnected";
    case USB_REENUM_NOT_POWERED:
      return "Port not powered";
    case USB_REENUM_CHANGE_PENDING:
      return "Hub changes pending";
    default:
      return "Unknown";
  }
}

static void cmd_usb_lifecycle_ready(void) {
  usb_reenum_info_t info = {0};
  if (!usb_reenum_inspect(&info)) {
    console_puts("Re-enumeration inspection failed.\n");
    return;
  }
  console_puts("USB re-enumeration eligibility:\n");
  console_printf("  Result      : %s\n", usb_reenum_result_name(info.result));
  console_printf("  Generation  : %u\n", (unsigned int)info.generation);
  console_printf("  Hub port    : %u\n", (unsigned int)info.port);
  console_printf("  Device state: %u\n", (unsigned int)info.device_state);
  console_printf("  Attempted   : %s\n",
      info.enumeration_attempted ? "Yes" : "No");
  console_printf("  Port status : 0x%04x\n", (unsigned int)info.port_status);
  console_printf("  Port change : 0x%04x\n", (unsigned int)info.port_change);
}

static const char *usb_lifecycle_action_name(usb_lifecycle_action_t action) {
  switch (action) {
    case USB_LIFECYCLE_IDLE: return "Idle";
    case USB_LIFECYCLE_RECONCILE_IN: return "Reconcile IN";
    case USB_LIFECYCLE_STOP_IN: return "Stop Bulk IN";
    case USB_LIFECYCLE_DISABLE: return "Disable Slot";
    case USB_LIFECYCLE_RETIRE_IN: return "Retire Bulk IN";
    case USB_LIFECYCLE_CLEANUP: return "Release DMA";
    case USB_LIFECYCLE_DETACH: return "Detach device";
    case USB_LIFECYCLE_RECYCLE: return "Recycle xHCI";
    case USB_LIFECYCLE_REARM: return "Rearm enumeration";
    case USB_LIFECYCLE_BLOCKED: return "Blocked";
    default: return "Unknown";
  }
}

static const char *usb_enum_admission_name(usb_enum_admission_result_t result) {
  switch (result) {
    case USB_ENUM_ADMIT_INITIAL:
      return "Ready for initial enumeration";
    case USB_ENUM_ADMIT_RECONNECT:
      return "Ready for re-enumeration";
    case USB_ENUM_ADMIT_SERVICE_FAILED:
      return "USB service unavailable";
    case USB_ENUM_ADMIT_HUB_FAILED:
      return "Hub monitoring failed";
    case USB_ENUM_ADMIT_HUB_NOT_READY:
      return "Hub not initialised";
    case USB_ENUM_ADMIT_ALREADY_ATTEMPTED:
      return "Enumeration already attempted";
    case USB_ENUM_ADMIT_XHCI_BUSY:
      return "xHCI downstream state busy";
    case USB_ENUM_ADMIT_NO_DEVICE:
      return "No connected device";
    case USB_ENUM_ADMIT_MULTIPLE_DEVICES:
      return "Multiple connected devices";
    case USB_ENUM_ADMIT_PORT_FAILED:
      return "Port inspection failed";
    case USB_ENUM_ADMIT_CHANGE_PENDING:
      return "Hub change pending";
    case USB_ENUM_ADMIT_RECONNECT_BLOCKED:
      return "Re-enumeration blocked";
    default:
      return "Unknown";
  }
}

static void cmd_usb_lifecycle_admission(void) {
  usb_enum_admission_info_t info = {0};
  if (!usb_enum_admission_inspect(&info)) {
    console_puts("Enumeration admission unavailable.\n");
    return;
  }
  console_puts("USB enumeration admission:\n");
  console_printf("  Decision   : %s\n", usb_enum_admission_name(info.result));
  console_printf("  Generation : %u\n", (unsigned int)info.generation);
  console_printf("  Hub port   : %u\n", (unsigned int)info.port);
  console_printf("  Connected  : %u\n", (unsigned int)info.connected_count);
  if (info.result == USB_ENUM_ADMIT_RECONNECT_BLOCKED) {
    console_printf("  Reason     : %s\n",
        usb_reenum_result_name(info.reconnect_reason));
  }
}

static void cmd_usb_lifecycle(void) {
  usb_lifecycle_info_t info = {0};
  if (!usb_lifecycle_inspect(&info)) {
    console_puts("USB lifecycle unavailable.\n");
    return;
  }
  console_puts("USB lifecycle:\n");
  console_printf("  Generation   : %u\n", (unsigned int)info.generation);
  console_printf("  Slot         : %u\n", (unsigned int)info.slot_id);
  console_printf("  Quiesced     : %s\n", info.quiesced ? "Yes" : "No");
  console_printf("  Disabled     : %s\n", info.slot_disabled ? "Yes" : "No");
  console_printf("  Cleaned      : %s\n", info.cleaned ? "Yes" : "No");
  console_printf("  Waiters      : %u\n", (unsigned int)info.waiters);
  console_printf("  IN started   : %s\n", info.in_started ? "Yes" : "No");
  console_printf("  IN pending   : %s\n", info.in_pending ? "Yes" : "No");
  console_printf("  IN completed : %s\n", info.in_completed ? "Yes" : "No");
  console_printf("  Next action  : %s\n",
      usb_lifecycle_action_name(info.next));
}

static const char *usb_lifecycle_run_name(
    usb_lifecycle_run_state_t state) {
  switch (state) {
    case USB_LIFECYCLE_RUN_DISARMED: return "Disarmed";
    case USB_LIFECYCLE_RUN_ARMED: return "Armed";
    case USB_LIFECYCLE_RUN_COMPLETE: return "Complete";
    case USB_LIFECYCLE_RUN_FAILED: return "Failed";
    default: return "Unknown";
  }
}

static const char *usb_lifecycle_fault_name(usb_lifecycle_fault_t fault) {
  switch (fault) {
    case USB_LIFECYCLE_FAULT_NONE:
      return "None";
    case USB_LIFECYCLE_FAULT_INSPECTION:
      return "Inspection failed";
    case USB_LIFECYCLE_FAULT_GENERATION:
      return "Generation mismatch";
    case USB_LIFECYCLE_FAULT_BLOCKED:
      return "Blocked action";
    case USB_LIFECYCLE_FAULT_OPERATION:
      return "Operation failed";
    case USB_LIFECYCLE_FAULT_XHCI_SERVICE:
      return "xHCI service failure";
    case USB_LIFECYCLE_FAULT_ROOT_PORT:
      return "Root port failure";
    case USB_LIFECYCLE_FAULT_HUB:
      return "Hub service failure";
    default:
      return "Unknown";
  }
}

static void cmd_usb_lifecycle_arm(void) {
  bool armed = usb_lifecycle_arm();
  console_printf("USB lifecycle arm: %s\n", armed ? "OK" : "NOT READY");
}

static void cmd_usb_lifecycle_status(void) {
  usb_lifecycle_run_info_t info = {0};
  if (!usb_lifecycle_get_run_info(&info)) {
    console_puts("USB lifecycle executor unavailable.\n");
    return;
  }
  console_puts("USB lifecycle executor:\n");
  console_printf("  State       : %s\n", usb_lifecycle_run_name(info.state));
  console_printf("  Generation  : %u\n", (unsigned int)info.generation);
  console_printf("  Steps       : %u\n", (unsigned int)info.steps);
  console_printf("  Last action : %s\n",
      usb_lifecycle_action_name(info.last_action));
  console_printf("  Last result : %u\n", (unsigned int)info.last_result);
  console_printf("  Succeeded   : %s\n", info.last_succeeded ? "Yes" : "No");
  console_printf("  Fault       : %s\n",
      usb_lifecycle_fault_name(info.fault));
  console_printf("  Slot        : %u\n", (unsigned int)info.slot_id);
  console_printf("  Observed gen: %u\n",
      (unsigned int)info.observed_generation);
  console_printf("  Command     : %u\n", (unsigned int)info.command_result);
  console_printf("  Completion  : %u\n", (unsigned int)info.completion_code);
}

static void cmd_usb_lifecycle_port(void) {
  usb_lifecycle_port_info_t info = {0};
  if (!usb_lifecycle_port_inspect(&info)) {
    console_puts("USB lifecycle port unavailable.\n");
    return;
  }
  console_puts("USB lifecycle physical port:\n");
  console_printf("  Generation  : %u\n", (unsigned int)info.generation);
  console_printf("  Hub port    : %u\n", (unsigned int)info.port);
  console_printf("  Connected   : %s\n", info.connected ? "Yes" : "No");
  console_printf("  Powered     : %s\n", info.powered ? "Yes" : "No");
  console_printf("  Status      : 0x%04x\n",
      (unsigned int)info.current_status);
  console_printf("  Change      : 0x%04x\n",
      (unsigned int)info.current_change);
  console_printf("  Last notify : %s\n",
      info.notification_seen ? "Yes" : "No");
  console_printf("  Sequence    : %u\n", (unsigned int)info.last_sequence);
  console_printf("  Last status : 0x%04x\n", (unsigned int)info.last_status);
  console_printf("  Last change : 0x%04x\n", (unsigned int)info.last_change);
}

static void cmd_usb_test_midiout(void) {
  usb_enum_info_t enumeration = {0};
  usb_device_t device = {0};

  if (!usb_get_enumeration(&enumeration) ||
      enumeration.result != USB_ENUM_OK ||
      !usb_get_device(&device) ||
      device.state != USB_DEVICE_ACTIVE) {
    console_puts("No active USB-MIDI device.\n");
    return;
  }
  const uint8_t note_on[4] = {0x09U, 0x90U, 0x3cU, 0x64U};
  const uint8_t note_off[4] = {0x08U, 0x80U, 0x3cU, 0x00U};
  xhci_bulk_out_info_t info = {0};
  console_puts("USB-MIDI Note On...\n");
  xhci_bulk_out_result_t result =
      xhci_downstream_bulk_out(note_on, sizeof(note_on), &info);
  console_printf("  Result          : %u\n", (unsigned int)result);
  console_printf("  Completion code : %u\n",
      (unsigned int)info.completion_code);
  console_printf("  TRB             : %016llx\n",
      (unsigned long long)info.transfer_trb);
  if (result != XHCI_BULK_OUT_OK || info.completion_code != 1U) {
    console_puts("USB-MIDI Note On: FAILED\n");
    return;
  }
  usb_test_delay_ms(250U);
  console_puts("USB-MIDI Note Off...\n");
  info = (xhci_bulk_out_info_t){0};
  result = xhci_downstream_bulk_out( note_off, sizeof(note_off), &info);
  console_printf("  Result          : %u\n", (unsigned int)result);
  console_printf("  Completion code : %u\n",
      (unsigned int)info.completion_code);
  console_printf("  TRB             : %016llx\n",
      (unsigned long long)info.transfer_trb);
  console_printf("USB-MIDI Note On/Off: %s\n",
      result == XHCI_BULK_OUT_OK &&
      info.completion_code == 1U ? "PASS" : "FAILED");
}

static void cmd_usb_test_midiin_stress(const char *args) {
  if (next_word_is(&args, "reset") && end_of_args(args)) {
    bool reset = usb_midi_in_stress_reset();
    console_printf("USB-MIDI IN stress reset: %s\n",
        reset ? "OK" : "NOT READY");
    return;
  }
  if (next_word_is(&args, "start") && end_of_args(args)) {
    bool started = usb_midi_in_stress_start();
    console_printf("USB-MIDI IN stress: %s\n",
        started ? "STARTED" : "NOT READY");
    if (started)
      console_puts("Send 600 test messages from Windows.\n");
    return;
  }
  if (next_word_is(&args, "status") && end_of_args(args)) {
    usb_midi_in_stress_info_t info = {0};
    if (!usb_midi_in_stress_get_info(&info)) {
      console_puts("USB-MIDI IN stress status unavailable.\n");
      return;
    }
    const char *state = "IDLE";
    switch (info.state) {
      case USB_MIDI_IN_STRESS_RUNNING:
        state = "RUNNING";
        break;
      case USB_MIDI_IN_STRESS_PASS:
        state = "PASS";
        break;
      case USB_MIDI_IN_STRESS_INCONCLUSIVE:
        state = "INCONCLUSIVE";
        break;
      case USB_MIDI_IN_STRESS_FAILED:
        state = "FAILED";
        break;
      default:
        break;
    }
    console_printf("USB-MIDI IN stress: %s\n", state);
    console_printf("  Transfers : %u\n",
        (unsigned int)info.transfers);
    console_printf("  Packets   : %u / %u\n",
        (unsigned int)info.packets,
        USB_MIDI_IN_STRESS_PACKETS);
    console_printf("  Bytes     : %u\n",
        (unsigned int)info.bytes);
    console_printf("  Wraps     : %u\n",
        (unsigned int)info.wraps);
    console_printf("  Elapsed   : %llu ms\n",
        (unsigned long long)(info.elapsed_us / 1000ULL));
    console_printf("  Discarded : %llu transfers\n",
        (unsigned long long)info.discarded_transfers);
    if (info.state == USB_MIDI_IN_STRESS_FAILED) {
      console_printf("  Error     : %u\n",
          (unsigned int)info.error);
      console_printf("  Transfer  : %u\n",
          (unsigned int)info.fault_transfer);
      console_printf("  Packet    : %u\n",
          (unsigned int)info.fault_packet);
      console_printf("  Expected TRB: %016llx\n",
          (unsigned long long)info.expected_trb);
      console_printf("  Actual TRB  : %016llx\n",
          (unsigned long long)info.actual_trb);
    }
    if (info.state == USB_MIDI_IN_STRESS_INCONCLUSIVE)
      console_puts(
          "Too few separate USB transfers; retry with slower MIDI.\n");
    return;
  }
  console_puts("Usage: usb test midiin stress [start | status | reset]\n");
}

static void cmd_usb_test_midiin(const char *args) {
  const char *stress_args = args;
  if (next_word_is(&stress_args, "stress")) {
    cmd_usb_test_midiin_stress(stress_args);
    return;
  }
  if (next_word_is(&args, "stream") && end_of_args(args)) {
    usb_enum_info_t enumeration = {0};
    if (!usb_get_enumeration(&enumeration) ||
        enumeration.result != USB_ENUM_OK) {
      console_puts("USB-MIDI device not enumerated.\n");
      return;
    }
    xhci_bulk_in_arm_result_t result = xhci_bulk_in_stream_start();
    console_printf("USB-MIDI IN stream start: %u\n", (unsigned int)result);
    return;
  }
  if (next_word_is(&args, "read") && end_of_args(args)) {
    usb_midi_in_stress_info_t stress = {0};
    if (usb_midi_in_stress_get_info(&stress) &&
        stress.state != USB_MIDI_IN_STRESS_IDLE) {
      console_puts("USB-MIDI IN stream owned by stress test.\n");
      return;
    }
    xhci_bulk_in_info_t info = {0};
    if (!xhci_bulk_in_stream_read(&info)) {
      console_puts("USB-MIDI IN queue empty.\n");
      return;
    }
    console_printf("USB-MIDI IN transfer:\n");
    console_printf("  Completion : %u\n", (unsigned int)info.completion_code);
    console_printf("  TRB        : %016llx\n",
        (unsigned long long)info.transfer_trb);
    console_printf("  Received   : %u bytes\n", (unsigned int)info.received);
    if (info.received % 4U != 0U)
      console_puts("  Warning: incomplete USB-MIDI packet.\n");
    for (uint32_t i = 0U; i + 3U < info.received; i += 4U) {
      console_printf("  Packet %u: %02x %02x %02x %02x\n",
          (unsigned int)(i / 4U), (unsigned int)info.data[i],
          (unsigned int)info.data[i + 1U], (unsigned int)info.data[i + 2U],
          (unsigned int)info.data[i + 3U]);
    }
    return;
  }
  if (next_word_is(&args, "status") && end_of_args(args)) {
    xhci_bulk_in_stream_stats_t stats = {0};
    if (!xhci_bulk_in_stream_get_stats(&stats) || !stats.started) {
      console_puts("USB-MIDI IN stream: NOT STARTED\n");
      return;
    }
    console_printf("USB-MIDI IN stream: %s\n",
        stats.failed ? "FAILED" : stats.paused ? "PAUSED" : "RUNNING");
    console_printf("  Queue depth : %u / %u\n",
        (unsigned int)stats.queue_depth, XHCI_BULK_IN_QUEUE_CAPACITY);
    console_printf("  Queue peak  : %u\n", (unsigned int)stats.queue_peak);
    console_printf("  Transfers   : %llu\n",
        (unsigned long long)stats.transfers);
    console_printf("  Bytes       : %llu\n", (unsigned long long)stats.bytes);
    console_printf("  Ring wraps  : %u\n", (unsigned int)stats.ring_wraps);
    console_printf("  Full pauses : %u\n", (unsigned int)stats.pauses);
    console_printf("  Last code   : %u\n",
        (unsigned int)stats.last_completion_code);
    console_printf("  Last arm    : %u\n",
        (unsigned int)stats.last_arm_result);
    return;
  }
  console_puts("Usage: usb test midiin [status | stream | read | stress]\n");
}

static void cmd_usb_test_midiwrap(void) {
  usb_enum_info_t enumeration = {0};
  if (!usb_get_enumeration(&enumeration) ||
      enumeration.result != USB_ENUM_OK) {
    console_puts("USB-MIDI device not enumerated.\n");
    return;
  }
  const unsigned int transfers = 520U;
  const unsigned int ring_entries = 255U;
  const uint64_t trb_size = 16U;
  uint64_t ring_base = 0U;
  unsigned int first_index = 0U;
  unsigned int completed = 0U;
  unsigned int wraps = 0U;
  uint64_t started = timer_now_us();
  console_puts("USB-MIDI ring-wrap stress test\n");
  for (unsigned int i = 0U; i < transfers; i++) {
    // Cable 0, Control Change, channel 16, controller 102.
    uint8_t packet[4] = { 0x0bU, 0xbfU, 0x66U, (uint8_t)(i & 0x7fU) };
    xhci_bulk_out_info_t info = {0};
    xhci_bulk_out_result_t result =
        xhci_downstream_bulk_out(packet, sizeof(packet), &info);
    if (result != XHCI_BULK_OUT_OK) {
      console_printf("Transfer %u failed: %u, completion %u\n",
          i + 1U, (unsigned int)result, info.completion_code);
      break;
    }
    if (info.completion_code != 1U ||
        info.event_parameter != info.transfer_trb) {
      console_printf("Transfer %u: unexpected completion\n", i + 1U);
      break;
    }
    if (i == 0U) {
      // The DMA ring occupies one aligned 4 KiB page.
      // Earlier MIDI transfers may have advanced its index.
      ring_base = info.transfer_trb & ~((uint64_t)PAGE_SIZE - 1U);
      first_index = (unsigned int)
          ((info.transfer_trb - ring_base) / trb_size);
      if (first_index >= ring_entries) {
        console_puts("Invalid initial transfer TRB index\n");
        break;
      }
    }
    unsigned int absolute_index = first_index + i;
    uint64_t expected = ring_base +
        (uint64_t)(absolute_index % ring_entries) * trb_size;
    if (info.transfer_trb != expected) {
      console_printf("Transfer %u: incorrect TRB address\n", i + 1U);
      console_printf("  Expected: %016llx\n", (unsigned long long)expected);
      console_printf("  Actual  : %016llx\n",
          (unsigned long long)info.transfer_trb);
      break;
    }
    completed++;
    if (i != 0U && absolute_index % ring_entries == 0U) {
      wraps++;
      console_printf("  Wrap %u: transfer %u, TRB %016llx\n", wraps, i + 1U,
          (unsigned long long)info.transfer_trb);
    }
    // Allow the adapter's physical DIN output to keep up.
    usb_test_delay_ms(4U);
  }
  uint64_t elapsed = timer_now_us() - started;
  console_printf("  Completed : %u / %u\n", completed, transfers);
  console_printf("  Wraps     : %u\n", wraps);
  console_printf("  Elapsed   : %llu ms\n",
      (unsigned long long)(elapsed / 1000ULL));
  console_printf("USB-MIDI ring-wrap: %s\n",
      completed == transfers && wraps >= 2U ? "PASS" : "FAILED");
}

static void cmd_usb_hubirq_start(void) {
  xhci_hub_notify_result_t result = xhci_hub_notify_start();
  if (result == XHCI_HUB_NOTIFY_OK)
    console_puts("Hub notification receiver armed.\n");
  else
    console_printf("Hub notification start failed (%u).\n",
        (unsigned int)result);
}

static void cmd_usb_hubirq_resume(void) {
  xhci_hub_notify_result_t result = xhci_hub_notify_resume();
  if (result == XHCI_HUB_NOTIFY_OK)
    console_puts("Hub notification receiver rearmed.\n");
  else
    console_printf("Hub notification resume failed (%u).\n",
        (unsigned int)result);
}

static void cmd_usb_changes(void) {
  usb_change_stats_t stats = {0};
  usb_change_get_stats(&stats);
  console_puts("USB downstream change events:\n");
  console_printf("  Recorded : %u\n", stats.recorded);
  console_printf("  Consumed : %u\n", stats.consumed);
  console_printf("  Queued   : %u\n", (unsigned int)stats.queued);
  console_printf("  Acked    : %u\n", stats.acknowledgements);
  console_printf("  Errors   : %u\n", stats.errors);
  console_printf("  Failed   : %s\n", stats.failed ? "Yes" : "No");
  if (stats.failed)
    console_printf("  Error port: %u\n", (unsigned int)stats.last_error_port);
  usb_change_event_t event = {0};
  while (usb_change_read(&event)) {
    console_printf( "  #%u port %u: status 0x%04x, change 0x%04x\n",
        event.sequence, (unsigned int)event.port,
        (unsigned int)event.status,
        (unsigned int)event.change);
  }
}

static void cmd_usb_hubirq(void) {
  xhci_hub_notify_info_t info = {0};
  if (!xhci_hub_notify_get_info(&info)) {
    console_puts("Hub notification receiver not started.\n");
    return;
  }
  console_printf("Hub interrupt receiver:\n");
  console_printf("  State       : %u\n", (unsigned int)info.state);
  console_printf("  Slot        : %u\n", (unsigned int)info.slot_id);
  console_printf("  Endpoint DCI: %u\n", (unsigned int)info.endpoint_id);
  console_printf("  Completion  : %u\n", (unsigned int)info.completion_code);
  console_printf("  Length      : %u\n", (unsigned int)info.length);
  console_printf("  Changes     : 0x%04x\n", (unsigned int)info.changes);
  console_printf("  Notifications: %u\n", (unsigned int)info.notifications);
  console_printf("  Ring wraps   : %u\n", (unsigned int)info.ring_wraps);
}

static void cmd_usb(const char *args) {
  // The VL805 is configured at boot.
  // This command merely inspects the controller.
  const char *other_args = args;
  if (next_word_is(&args, "recover")) {
    if (end_of_args(args)) {
      cmd_usb_recover();
      return;
    }
    if (next_word_is(&args, "status") && end_of_args(args)) {
      cmd_usb_recover_status();
      return;
    }
    console_puts("Usage: usb recover [status]\n");
    return;
  }
  if (next_word_is(&args, "lifecycle")) {
    if (end_of_args(args)) {
      cmd_usb_lifecycle();
      return;
    }
    if (next_word_is(&args, "admission") && end_of_args(args)) {
      cmd_usb_lifecycle_admission();
      return;
    }
    if (next_word_is(&args, "ready") && end_of_args(args)) {
      cmd_usb_lifecycle_ready();
      return;
    }
    if (next_word_is(&args, "port") && end_of_args(args)) {
      cmd_usb_lifecycle_port();
      return;
    }
    if (next_word_is(&args, "arm") && end_of_args(args)) {
      cmd_usb_lifecycle_arm();
      return;
    }
    if (next_word_is(&args, "status") && end_of_args(args)) {
      cmd_usb_lifecycle_status();
      return;
    }
    console_puts("Usage: usb lifecycle [arm | status | port | ready | admission]\n");
    return;
  }
  if (next_word_is(&args, "retirein") && end_of_args(args)) {
    cmd_usb_retirein();
    return;
  }
  if (next_word_is(&args, "reconcilein") && end_of_args(args)) {
    cmd_usb_reconcilein();
    return;
  }
  if (next_word_is(&args, "stopout") && end_of_args(args)) {
    cmd_usb_stopout();
    return;
  }
  if (next_word_is(&args, "stopin") && end_of_args(args)) {
    cmd_usb_stopin();
    return;
  }
  if (next_word_is(&args, "transfers") && end_of_args(args)) {
    cmd_usb_transfers();
    return;
  }
  if (next_word_is(&args, "rearm") && end_of_args(args)) {
    cmd_usb_rearm();
    return;
  }
  if (next_word_is(&args, "recycle") && end_of_args(args)) {
    cmd_usb_recycle();
    return;
  }
  if (next_word_is(&args, "detach") && end_of_args(args)) {
    cmd_usb_detach();
    return;
  }
  if (next_word_is(&args, "cleanup") && end_of_args(args)) {
    cmd_usb_cleanup();
    return;
  }
  if (next_word_is(&args, "disable") && end_of_args(args)) {
    cmd_usb_disable();
    return;
  }
  if (next_word_is(&args, "changes") && end_of_args(args)) {
    cmd_usb_changes();
    return;
  }
  if (next_word_is(&other_args, "device") &&
      next_word_is(&other_args, "check") &&
      end_of_args(other_args)) {
    cmd_usb_device_check();
    return;
  }
  if (next_word_is(&args, "hubirq")) {
    if (end_of_args(args)) {
      cmd_usb_hubirq();
      return;
    }
    if (next_word_is(&args, "start") && end_of_args(args)) {
      cmd_usb_hubirq_start();
      return;
    }
    if (next_word_is(&args, "resume") && end_of_args(args)) {
      cmd_usb_hubirq_resume();
      return;
    }
  }
  if (next_word_is(&args, "devices") && end_of_args(args)) {
    cmd_usb_devices();
    return;
  }
  if (next_word_is(&args, "scan") && end_of_args(args)) {
    cmd_usb_scan();
    return;
  }
  if (next_word_is(&args, "enumerate") && end_of_args(args)) {
    cmd_usb_enumerate();
    return;
  }
  if (next_word_is(&args, "enumstate") && end_of_args(args)) {
    cmd_usb_enumstate();
    return;
  }
  if (next_word_is(&args, "hubports") && end_of_args(args)) {
    cmd_usb_hubports();
    return;
  }
  if (next_word_is(&args, "events") && end_of_args(args)) {
    cmd_usb_events();
    return;
  }
  if (next_word_is(&args, "ports") && end_of_args(args)) {
    cmd_usb_ports();
    return;
  }
  if (next_word_is(&other_args, "test")) {
    if (next_word_is(&other_args, "ep0match") &&
        end_of_args(other_args)) {
      cmd_usb_test_ep0match();
      return;
    }
    if (next_word_is(&other_args, "enumabort") &&
        end_of_args(other_args)) {
      cmd_usb_test_enumabort();
      return;
    }
    if (next_word_is(&other_args, "enumconfigabort") &&
        end_of_args(other_args)) {
      cmd_usb_test_enumconfigabort();
      return;
    }
    if (next_word_is(&other_args, "noops") &&
        end_of_args(other_args)) {
      cmd_usb_test_noops();
      return;
    }
    if (next_word_is(&other_args, "quiesce") &&
        end_of_args(other_args)) {
      cmd_usb_test_quiesce();
      return;
    }
    if (next_word_is(&other_args, "midiout") &&
        end_of_args(other_args)) {
      cmd_usb_test_midiout();
      return;
    }
    if (next_word_is(&other_args, "midiwrap") &&
        end_of_args(other_args)) {
      cmd_usb_test_midiwrap();
      return;
    }
    if (next_word_is(&other_args, "midiin")) {
      cmd_usb_test_midiin(other_args);
      return;
    }
    console_puts("Usage: usb test [noops | quiesce | midiout | midiwrap | midiin | "
                 "enumabort | enumconfigabort ]\n");
    return;
  }
  if (next_word_is(&args, "state") && end_of_args(args)) {
    cmd_usb_state();
    return;
  }

  if (!end_of_args(args)) {
    console_puts(
        "Usage: usb [scan | state | ports | events | device | devices | test | recover"
        "stopout | stopin | disable | cleanup | detach | recycle | rearm | transfers | "
	"retirein | reconcilein | lifecycle | hubports | hubirq | enumerate | enumstate]\n");
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

static char console_line[CONSOLE_LINE_SIZE];
static size_t console_length;
static bool console_last_was_cr;

void console_start(void) {
  console_puts("Console ready.\n");
  console_prompt();
}

bool console_poll(void) {
  // Do not wait for input.
  if (!uart_has_data())
    return false;

  unsigned char c = (unsigned char)uart_getc();
  // Treat CRLF as one newline.
  if (c == '\n' && console_last_was_cr) {
    console_last_was_cr = false;
    return true;
  }

  console_last_was_cr = (c == '\r');
  if (c == '\r' || c == '\n') {
    console_puts("\n");
    console_line[console_length] = '\0';
    console_dispatch(console_line);
    console_length = 0U;
    console_prompt();
    return true;
  }

  if (c == '\b' || c == 0x7f) {
    if (console_length > 0U) {
      console_length--;
      console_puts("\b \b");
    }
    return true;
  }

  if (c == '\t')
    c = ' ';

  if (c < 0x20 || c > 0x7e)
    return true;

  if (console_length >= sizeof(console_line) - 1U) {
    console_putc('\a');
    return true;
  }

  console_line[console_length++] = (char)c;
  console_putc((char)c);

  return true;
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

