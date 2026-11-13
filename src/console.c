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
#include "usb_enumerate.h"
#include "usb_midi.h"
#include "usb_midi_rx.h"
#include "midi_monitor.h"
#include "midi_auto.h"
#include "midi_input.h"
#include "midi_output.h"
#include "midi_stream.h"
#include "usb_midi_test.h"

#define CONSOLE_LINE_SIZE 256

static char console_line[CONSOLE_LINE_SIZE];
static size_t console_length;
static bool console_last_was_cr;
static bool console_line_overflowed;

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

static void cmd_uptime(const char *args) {
  if (!end_of_args(args)) {
    console_puts("Usage: uptime\n");
    return;
  }

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
  if (!end_of_args(args)) {
    console_puts("Usage: irq\n");
    return;
  }

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
    console_puts("Usage: pci [dma [test]]\n");
    return;
  }
  if (!end_of_args(args)) {
    console_puts("Usage: pci [dma [test]]\n");
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
  console_puts("Usage: dma [owner | cache | edge]\n");
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

static void cmd_usb_test_ep0match(void) {
  bool passed = xhci_ep0_match_selftest();
  console_printf("EP0 TD matching: %s\n", passed ? "PASS" : "FAILED");
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

static void cmd_usb_hubports(void) {
  usb_hub_init_info_t hub = {0};
  if (!usb_hub_get_init_info(&hub) ||
      hub.result != USB_HUB_INIT_OK || hub.ports == 0U) {
    console_puts("Internal USB hub not initialised.\n");
    return;
  }
  console_printf("Internal USB hub downstream ports (%u):\n",
      (unsigned int)hub.ports);
  for (uint8_t port = 1U; port <= hub.ports; port++) {
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

static void cmd_usb_devices(void) {
  unsigned int records = 0U;
  unsigned int active = 0U;
  unsigned int failed = 0U;
  unsigned int detached = 0U;
  console_puts("USB managed device inventory:\n");
  for (unsigned int i = 0U; i < USB_MANAGED_DEVICE_CAPACITY; i++) {
    usb_device_t device = {0};
    if (!usb_get_device_at(i, &device))
      continue;
    records++;
    switch (device.state) {
      case USB_DEVICE_ACTIVE:
        active++;
        break;
      case USB_DEVICE_FAILED:
        failed++;
        break;
      case USB_DEVICE_DETACHED:
        detached++;
        break;
      default:
        break;
    }
    console_printf("\n  Record %u:\n", i);
    console_printf("    State          : %s\n",
        usb_device_state_name(device.state));
    if (device.generation != 0U)
      console_printf("    Generation     : %u\n",
          (unsigned int)device.generation);
    else
      console_puts("    Generation     : Not assigned\n");
    console_printf("    Root port      : %u\n",
        (unsigned int)device.root_port);
    console_printf("    Parent slot    : %u\n",
        (unsigned int)device.parent_slot);
    console_printf("    Parent port    : %u\n",
        (unsigned int)device.parent_port);
    console_printf("    Speed          : %s\n",
        usb_speed_name((uint8_t)device.speed));
    if (device.slot_id != 0U) {
      console_printf("    Slot ID        : %u\n",
          (unsigned int)device.slot_id);
      console_printf("    xHCI quiesced  : %s\n",
          xhci_downstream_is_quiesced(device.slot_id) ? "Yes" : "No");
    } else {
      console_puts("    Slot ID        : Not assigned\n");
      console_puts("    xHCI quiesced  : N/A\n");
    }
    if (device.address != 0U)
      console_printf("    USB address    : %u\n",
          (unsigned int)device.address);
    else
      console_puts("    USB address    : Not assigned\n");
    console_printf("    Vendor ID      : 0x%04x\n",
        (unsigned int)device.vendor_id);
    console_printf("    Product ID     : 0x%04x\n",
        (unsigned int)device.product_id);
    usb_enum_info_t enumeration = {0};
    if (usb_get_enumeration_at(i, &enumeration)) {
      console_printf("    Enumeration    : %s (result %u)\n",
          enumeration.result == USB_ENUM_OK ? "PASS" : "FAILED",
          (unsigned int)enumeration.result);
      console_printf("    Enum stage     : %s\n",
          usb_enum_stage_name(enumeration.stage));
      if (enumeration.result != USB_ENUM_OK)
        console_printf("    Enum detail    : 0x%08x\n",
            (unsigned int)enumeration.detail);
    } else {
      console_puts("    Enumeration    : No recorded attempt\n");
    }
  }
  if (records == 0U)
    console_puts("  No managed device records.\n");
  console_printf("\n  Records  : %u / %u\n",
      records, USB_MANAGED_DEVICE_CAPACITY);
  console_printf("  Active   : %u\n", active);
  console_printf("  Failed   : %u\n", failed);
  console_printf("  Detached : %u\n", detached);
}

static void cmd_usb_enumerate(const char *args) {
  unsigned int requested = 0U;
  if (!next_word_is(&args, "port") || !next_uint(&args, &requested) ||
      !end_of_args(args) || requested == 0U || requested > 4U) {
    console_puts("Usage: usb enumerate port <1-4>\n");
    return;
  }
  uint8_t port = (uint8_t)requested;
  usb_enum_info_t info = {0};
  usb_enum_result_t outcome = usb_enumerate_port(port, &info);
  bool recorded = outcome == USB_ENUM_ALREADY_ATTEMPTED;
  usb_enum_result_t result = recorded ? info.result : outcome;
  console_puts("USB enumeration:\n");
  console_printf("  Requested port  : %u\n", (unsigned int)port);
  console_printf("  Source          : %s\n",
      recorded ? "Recorded result (no new attempt)" : "Manual request");
  console_printf("  Result          : %u\n", (unsigned int)result);
  console_printf("  Detail          : %u\n", (unsigned int)info.detail);
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
  usb_device_t device = {0};
  if (usb_get_device_by_slot(info.slot_id, &device))
    console_printf("  Generation      : %u\n",
        (unsigned int)device.generation);
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
      (unsigned int)info.midi.bulk_out, (unsigned int)info.midi.bulk_out_mps);
  console_printf("  Bulk IN         : 0x%02x, MPS %u\n",
      (unsigned int)info.midi.bulk_in, (unsigned int)info.midi.bulk_in_mps);
  console_puts("USB enumeration: PASS\n");
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

static void cmd_usb_enumstate(const char *args) {
  unsigned int index = 0U;
  if (!next_word_is(&args, "record") || !next_uint(&args, &index) ||
      !end_of_args(args) || index >= USB_MANAGED_DEVICE_CAPACITY) {
    console_puts("Usage: usb enumstate record <0-3>\n");
    return;
  }
  usb_enum_info_t info = {0};
  if (!usb_get_enumeration_at(index, &info)) {
    console_printf("No enumeration attempt recorded for record %u.\n", index);
    return;
  }
  usb_device_t device = {0};
  bool have_device = usb_get_device_at(index, &device);
  console_printf("USB enumeration state (record %u):\n", index);
  if (have_device && device.generation != 0U) {
    console_printf("  Generation   : %u\n", (unsigned int)device.generation);
  } else {
    console_puts("  Generation   : Not assigned\n");
  }
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
  // Inspect hardware endpoint state only when
  // the slot still belongs to this exact record.
  usb_device_t owner = {0};
  xhci_downstream_transfer_state_t transfers = {0};
  if (info.slot_id == 0U || !have_device || device.slot_id != info.slot_id ||
      !usb_get_device_by_slot(info.slot_id, &owner) ||
      owner.generation != device.generation ||
      owner.root_port != device.root_port ||
      owner.parent_slot != device.parent_slot ||
      owner.parent_port != device.parent_port ||
      !xhci_downstream_transfer_state(info.slot_id, &transfers) ||
      transfers.slot_id != info.slot_id)
    return;
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

static const char *usb_recovery_result_name(
    usb_recovery_result_t result) {
  switch (result) {
    case USB_RECOVERY_READY:
      return "Ready";
    case USB_RECOVERY_READY_EP0_ERROR:
      return "Ready (confirmed EP0 error)";
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

static bool usb_parse_generation(const char *args, uint32_t *generation) {
  if (!args || !generation)
    return false;
  unsigned int value = 0U;
  if (!next_uint(&args, &value) || value == 0U || !end_of_args(args))
    return false;
  *generation = (uint32_t)value;
  return true;
}

static void cmd_usb_recover_status(const char *args) {
  uint32_t generation = 0U;
  if (!usb_parse_generation(args, &generation)) {
    console_puts("Usage: usb recover status <generation>\n");
    return;
  }
  usb_recovery_info_t info = {0};
  if (!usb_enumeration_recovery_inspect_generation(generation, &info)) {
    console_puts("USB recovery generation unavailable.\n");
    return;
  }
  console_printf("USB enumeration recovery (generation %u):\n",
      (unsigned int)generation);
  console_printf("  Decision   : %s\n",
      usb_recovery_result_name(info.result));
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

static void cmd_usb_recover(const char *args) {
  uint32_t generation = 0U;
  if (!usb_parse_generation(args, &generation)) {
    console_puts("Usage: usb recover <generation>\n");
    return;
  }
  if (!usb_service_available()) {
    console_puts("USB recovery: SERVICE UNAVAILABLE\n");
    return;
  }
  usb_recovery_info_t recovery = {0};
  if (!usb_enumeration_recovery_inspect_generation(generation, &recovery) ||
      (recovery.result != USB_RECOVERY_READY &&
       recovery.result != USB_RECOVERY_READY_EP0_ERROR)) {
    console_puts("USB recovery: NOT READY\n");
    return;
  }
  if (!usb_enumeration_recovery_begin_generation(generation)) {
    console_puts("USB recovery: FAILED\n");
    return;
  }
  if (!usb_lifecycle_arm_generation(generation)) {
    console_puts("USB recovery: QUIESCED, NOT ARMED\n");
    return;
  }
  console_printf("USB recovery: STARTED (generation %u)\n",
      (unsigned int)generation);
}

static void cmd_usb_transfers(const char *args) {
  uint32_t generation = 0U;
  if (!usb_parse_generation(args, &generation)) {
    console_puts("Usage: usb transfers <generation>\n");
    return;
  }
  usb_device_t device = {0};
  usb_device_t slot_owner = {0};
  xhci_downstream_transfer_state_t state = {0};
  if (!usb_get_device_by_generation(generation, &device) ||
      device.slot_id == 0U ||
      !usb_get_device_by_slot(device.slot_id, &slot_owner) ||
      slot_owner.generation != generation ||
      slot_owner.root_port != device.root_port ||
      slot_owner.parent_slot != device.parent_slot ||
      slot_owner.parent_port != device.parent_port ||
      !xhci_downstream_transfer_state(device.slot_id, &state) ||
      state.slot_id != device.slot_id) {
    console_puts("Transfer state unavailable.\n");
    return;
  }
  console_printf("Downstream xHCI transfers (generation %u):\n",
      (unsigned int)generation);
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

static void cmd_usb_lifecycle_status(const char *args) {
  uint32_t generation = 0U;
  if (!usb_parse_generation(args, &generation)) {
    console_puts("Usage: usb lifecycle status <generation>\n");
    return;
  }
  usb_lifecycle_info_t state = {0};
  if (!usb_lifecycle_inspect_generation(generation, &state)) {
    console_puts("USB lifecycle generation unavailable.\n");
    return;
  }
  console_printf("USB lifecycle (generation %u):\n",
      (unsigned int)generation);
  console_printf("  Slot         : %u\n", (unsigned int)state.slot_id);
  console_printf("  Quiesced     : %s\n", state.quiesced ? "Yes" : "No");
  console_printf("  Disabled     : %s\n", state.slot_disabled ? "Yes" : "No");
  console_printf("  Cleaned      : %s\n", state.cleaned ? "Yes" : "No");
  console_printf("  Waiters      : %u\n", (unsigned int)state.waiters);
  console_printf("  IN started   : %s\n", state.in_started ? "Yes" : "No");
  console_printf("  IN pending   : %s\n", state.in_pending ? "Yes" : "No");
  console_printf("  IN completed : %s\n", state.in_completed ? "Yes" : "No");
  console_printf("  Next action  : %s\n",
      usb_lifecycle_action_name(state.next));
  usb_lifecycle_run_info_t run = {0};
  if (!usb_lifecycle_get_run_info_generation(generation, &run)) {
    console_puts("\n  Executor     : None recorded\n");
    return;
  }
  console_puts("\nLifecycle executor:\n");
  console_printf("  State       : %s\n", usb_lifecycle_run_name(run.state));
  console_printf("  Generation  : %u\n", (unsigned int)run.generation);
  console_printf("  Steps       : %u\n", (unsigned int)run.steps);
  console_printf("  Last action : %s\n",
      usb_lifecycle_action_name(run.last_action));
  console_printf("  Last result : %u\n", (unsigned int)run.last_result);
  console_printf("  Succeeded   : %s\n", run.last_succeeded ? "Yes" : "No");
  console_printf("  Fault       : %s\n", usb_lifecycle_fault_name(run.fault));
  console_printf("  Slot        : %u\n", (unsigned int)run.slot_id);
  console_printf("  Observed gen: %u\n",
      (unsigned int)run.observed_generation);
  console_printf("  Command     : %u\n", (unsigned int)run.command_result);
  console_printf("  Completion  : %u\n", (unsigned int)run.completion_code);
}

static void cmd_usb_lifecycle_port(const char *args) {
  uint32_t generation = 0U;
  if (!usb_parse_generation(args, &generation)) {
    console_puts("Usage: usb lifecycle port <generation>\n");
    return;
  }
  usb_lifecycle_port_info_t info = {0};
  if (!usb_lifecycle_port_inspect_generation(generation, &info)) {
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
  usb_init_info_t root = {0};
  xhci_connection_ownership_t ownership = XHCI_CONNECTION_UNCERTAIN;
  if (usb_get_init_info(&root) && root.result == USB_INIT_OK) {
    (void)xhci_downstream_connection_ownership(
        root.root_port, root.slot_id, info.port, &ownership);
  }
  const char *owner_name = "Uncertain";
  switch (ownership) {
    case XHCI_CONNECTION_UNOWNED:
      owner_name = "Unowned";
      break;
    case XHCI_CONNECTION_OWNED:
      owner_name = "Owned";
      break;
    default:
      break;
  }
  console_printf("  xHCI owner  : %s\n", owner_name);
}

static void usb_topology_port_detail(const xhci_port_t *port) {
  console_printf("    PORTSC       : 0x%08x\n", (unsigned int)port->portsc);
  console_printf("    Overcurrent  : %s\n", port->overcurrent ? "Yes" : "No");
  console_printf("    Reset active : %s\n", port->resetting ? "Yes" : "No");
  const struct { bool set; const char *name; } flags[] = {
    {port->connect_change, "CSC"},
    {port->enable_change, "PEC"},
    {port->warm_reset_change, "WRC"},
    {port->overcurrent_change, "OCC"},
    {port->reset_change, "PRC"},
    {port->link_change, "PLC"},
    {port->config_error_change, "CEC"}
  };
  bool changed = false;
  console_puts("    Change flags :");
  for (unsigned int i = 0U; i < sizeof(flags) / sizeof(flags[0]); i++) {
    if (!flags[i].set)
      continue;
    console_printf(" %s", flags[i].name);
    changed = true;
  }
  if (!changed)
    console_puts(" None");
  console_putc('\n');
}

static void cmd_usb_topology(bool detail) {
  uint8_t total_ports = 0U;
  if (!xhci_port_count(&total_ports)) {
    console_puts("USB root-port topology unavailable.\n");
    return;
  }
  // Track which root ports are described by
  // Supported Protocol capabilities. This is
  // temporary diagnostic data, not controller state.
  bool covered[256] = {false};
  usb_init_info_t root = {0};
  bool hub_known = usb_get_init_info(&root) && root.result == USB_INIT_OK &&
      root.slot_id != 0U && root.address != 0U;
  console_printf("xHCI root-port topology (%u ports):\n",
      (unsigned int)total_ports);
  unsigned int index = 0U;
  for (;;) {
    xhci_protocol_t protocol = {0};
    bool found = false;
    if (!xhci_protocol_get(index, &protocol, &found)) {
      console_printf( "Supported Protocol inspection failed at index %u.\n",
          index);
      return;
    }
    if (!found)
      break;
    console_printf("\nProtocol %u: ", index);
    // The USB protocol name is "USB ", stored
    // as a little-endian 32-bit register value.
    if (protocol.name == 0x20425355U) {
      // Revision fields use BCD. For example,
      // minor 0x10 denotes USB 3.1.
      console_printf("USB %u.%u", (unsigned int)protocol.major,
          (unsigned int)(protocol.minor >> 4));
      if ((protocol.minor & 0x0fU) != 0U)
        console_printf(".%u", (unsigned int)(protocol.minor & 0x0fU));
    } else {
      console_printf("Unknown protocol (name 0x%08x)", protocol.name);
    }
    console_printf("\n  Capability offset : 0x%04x\n", protocol.offset);
    console_printf("  Port offset       : %u\n",
        (unsigned int)protocol.first_port);
    console_printf("  Port count        : %u\n",
        (unsigned int)protocol.port_count);
    console_printf("  Slot type         : %u\n",
        (unsigned int)protocol.slot_type);
    console_printf("  Speed ID count    : %u\n",
        (unsigned int)protocol.speed_id_count);
    unsigned int first = protocol.first_port;
    unsigned int count = protocol.port_count;
    // Root ports are numbered from 1 to MaxPorts.
    // Never inspect a range outside that boundary.
    if (first == 0U || count == 0U || first + count - 1U > total_ports) {
      console_puts("  INVALID root-port range.\n");
      index++;
      continue;
    }
    for (unsigned int number = first; number < first + count; number++) {
      if (covered[number])
        console_printf( "  WARNING: root port %u appears in multiple "
            "protocol ranges.\n", number);
      covered[number] = true;
      xhci_port_t port = {0};
      if (!xhci_port_read((uint8_t)number, &port)) {
        console_printf("  Root port %u: unavailable.\n", number);
        continue;
      }
      console_printf( "  Root port %u: %s, %s, %s", number,
          port.connected ? "Connected" : "Disconnected",
          port.enabled ? "Enabled" : "Disabled",
          port.powered ? "Powered" : "Unpowered");
      if (port.connected)
        console_printf(", %s", usb_speed_name(port.speed));
      console_printf(", link %s", usb_link_name(port.link_state));
      if (hub_known && number == root.root_port)
        console_printf( " [internal hub, slot %u, address %u]",
            (unsigned int)root.slot_id, (unsigned int)root.address);
      console_putc('\n');
      if (detail)
        usb_topology_port_detail(&port);
    }
    index++;
  }
  if (index == 0U)
    console_puts("\nNo Supported Protocol capabilities found.\n");
  // Detect root ports which have no protocol
  // capability describing them. Do not read
  // their PORTSC registers.
  for (unsigned int number = 1U; number <= total_ports; number++) {
    if (!covered[number])
      console_printf( "WARNING: root port %u has no protocol mapping.\n",
          number);
  }
  if (hub_known) {
    usb_hub_init_info_t hub = {0};
    if (usb_hub_get_init_info(&hub) && hub.result == USB_HUB_INIT_OK)
      console_printf( "\nInternal USB hub: %u downstream ports "
          "(inspect with usb hubports).\n", (unsigned int)hub.ports);
  }
  console_puts( "\nNote: protocol ranges do not identify physical "
      "USB socket pairings.\n");
  console_puts( "SuperSpeed root-port enumeration is not yet implemented.\n");
}

static bool usb_test_midi_active_generation(
    uint32_t generation, uint8_t expected_slot, usb_device_t *owner) {
  if (!owner)
    return false;
  *owner = (usb_device_t){0};
  if (!usb_service_available() || generation == 0U)
    return false;
  usb_device_t device = {0};
  usb_device_t by_slot = {0};
  usb_device_t by_connection = {0};
  usb_enum_info_t enumeration = {0};
  if (!usb_get_device_by_generation(generation, &device) ||
      device.state != USB_DEVICE_ACTIVE || device.slot_id == 0U ||
      device.root_port == 0U || device.parent_slot == 0U ||
      device.parent_port == 0U)
    return false;
  if (expected_slot != 0U && device.slot_id != expected_slot)
    return false;
  // The hardware slot must belong to exactly
  // the same device incarnation.
  if (!usb_get_device_by_slot(device.slot_id, &by_slot) ||
      by_slot.generation != generation ||
      by_slot.root_port != device.root_port ||
      by_slot.parent_slot != device.parent_slot ||
      by_slot.parent_port != device.parent_port)
    return false;
  // Physical-connection ownership must also
  // resolve uniquely to this generation and slot.
  if (!usb_get_device_by_connection( device.root_port, device.parent_slot,
          device.parent_port, &by_connection) ||
      by_connection.generation != generation ||
      by_connection.slot_id != device.slot_id)
    return false;
  // The completed enumeration must belong to
  // this same device and its MIDI endpoints.
  if (!usb_get_enumeration_generation( generation, &enumeration) ||
      enumeration.result != USB_ENUM_OK ||
      enumeration.slot_id != device.slot_id ||
      enumeration.hub_port != device.parent_port ||
      enumeration.vendor_id != device.vendor_id ||
      enumeration.product_id != device.product_id)
    return false;
  *owner = device;
  return true;
}

static bool usb_test_midi_select(const char *args, usb_device_t *device) {

  if (!device)
    return false;

  *device = (usb_device_t){0};

  uint32_t generation = 0U;
  if (!usb_parse_generation(args, &generation))
    return false;

  return usb_test_midi_active_generation(generation, 0U, device);
}

static bool usb_test_midi_out_select(const char *args, usb_device_t *device,
    usb_midi_config_t *midi, unsigned int *cable) {

  if (!args || !device || !midi || !cable)
    return false;

  *device = (usb_device_t){0};
  *midi = (usb_midi_config_t){0};
  *cable = 0U;

  const char *p = args;
  unsigned int requested = 0U;

  // Every USB-MIDI OUT test must identify
  // a specific device incarnation.
  if (!next_uint(&p, &requested) || requested == 0U)
    return false;

  uint32_t generation = (uint32_t)requested;

  if (midi_output_owns_generation(generation)) {
    console_puts("Bulk OUT owned by the MIDI output service.\n");
    return false;
  }

  // The virtual cable is optional.
  // Cable zero remains the default.
  if (!end_of_args(p)) {
    if (!next_word_is(&p, "cable") || !next_uint(&p, cable) ||
        !end_of_args(p))
      return false;
  }

  if (!usb_test_midi_active_generation(generation, 0U, device))
    return false;

  // Read the MIDI configuration associated
  // with the selected device incarnation.
  usb_enum_info_t enumeration = {0};
  if (!usb_get_enumeration_generation(generation, &enumeration) ||
      enumeration.result != USB_ENUM_OK ||
      enumeration.slot_id != device->slot_id ||
      enumeration.hub_port != device->parent_port)
    return false;

  *midi = enumeration.midi;
  return true;
}

static void cmd_usb_test_midiout(const char *args) {
  usb_device_t device = {0};
  usb_midi_config_t midi = {0};
  unsigned int cable = 0U;
  if (!usb_test_midi_out_select(args, &device, &midi, &cable)) {
    console_puts("No active USB-MIDI device or invalid arguments.\n");
    console_puts("Usage: usb test midiout <generation> [cable N]\n");
    return;
  }
  const uint8_t on_message[3] = {
    0x90U, 0x3cU, 0x64U
  };
  const uint8_t off_message[3] = {
    0x80U, 0x3cU, 0x00U
  };
  uint8_t note_on[4] = {0};
  uint8_t note_off[4] = {0};
  if (!usb_midi_out_packet(&midi, cable, 0x09U, on_message, note_on) ||
      !usb_midi_out_packet(&midi, cable, 0x08U, off_message, note_off)) {
    console_printf("USB-MIDI cable %u unavailable "
        "(Bulk OUT jacks: %u, maximum supported: %u).\n",
        cable, (unsigned int)midi.bulk_out_jacks, USB_MIDI_MAX_CABLES);
    return;
  }
  console_printf("USB-MIDI target: generation %u, slot %u, cable %u\n",
      (unsigned int)device.generation, (unsigned int)device.slot_id, cable);
  console_printf("  Available cables: %u\n",
      (unsigned int)midi.bulk_out_jacks);
  console_printf("  Note On packet : %02x %02x %02x %02x\n",
      (unsigned int)note_on[0], (unsigned int)note_on[1],
      (unsigned int)note_on[2], (unsigned int)note_on[3]);
  xhci_bulk_out_info_t info = {0};
  console_puts("USB-MIDI Note On...\n");
  xhci_bulk_out_result_t result = xhci_downstream_bulk_out(
      device.slot_id, note_on, sizeof(note_on), &info);
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
  // Never send Note Off to a different device
  // incarnation which has reused the old slot.
  usb_device_t current = {0};
  if (!usb_test_midi_active_generation(
          device.generation, device.slot_id, &current)) {
    console_puts("USB-MIDI Note On/Off: FAILED (device ownership changed)\n");
    return;
  }
  console_printf("  Note Off packet: %02x %02x %02x %02x\n",
      (unsigned int)note_off[0], (unsigned int)note_off[1],
      (unsigned int)note_off[2], (unsigned int)note_off[3]);
  console_puts("USB-MIDI Note Off...\n");
  info = (xhci_bulk_out_info_t){0};
  result = xhci_downstream_bulk_out(
      device.slot_id, note_off, sizeof(note_off), &info);
  console_printf("  Result          : %u\n", (unsigned int)result);
  console_printf("  Completion code : %u\n",
      (unsigned int)info.completion_code);
  console_printf("  TRB             : %016llx\n",
      (unsigned long long)info.transfer_trb);
  console_printf("USB-MIDI Note On/Off: %s\n", result == XHCI_BULK_OUT_OK &&
      info.completion_code == 1U ? "PASS" : "FAILED");
}

static const char *xhci_async_out_state_name(
    xhci_bulk_out_async_state_t state) {

  switch (state) {
    case XHCI_BULK_OUT_ASYNC_IDLE:
      return "Idle";
    case XHCI_BULK_OUT_ASYNC_PENDING:
      return "Pending";
    case XHCI_BULK_OUT_ASYNC_COMPLETE:
      return "Complete";
    case XHCI_BULK_OUT_ASYNC_FAILED:
      return "Failed";
    case XHCI_BULK_OUT_ASYNC_UNCERTAIN:
      return "Uncertain";
    default:
      return "Unknown";
  }
}

static void cmd_usb_test_midiasync(const char *args) {
  const char *p = args;
  if (next_word_is(&p, "start")) {
    usb_device_t device = {0};
    usb_midi_config_t midi = {0};
    unsigned int cable = 0U;
    if (!usb_test_midi_out_select(p, &device, &midi, &cable)) {
      console_puts("Usage: usb test midiasync "
          "start <generation> [cable N]\n");
      return;
    }
    const uint8_t message[3] = { 0xbfU, 0x66U, 0x20U };
    uint8_t packet[4] = {0};
    if (!usb_midi_out_packet(&midi, cable, 0x0bU, message, packet)) {
      console_puts("USB-MIDI OUT cable unavailable.\n");
      return;
    }
    xhci_bulk_out_result_t result =
        xhci_bulk_out_async_start( device.slot_id, packet, sizeof(packet));
    console_printf( "Async Bulk OUT (generation %u, slot %u): %s\n",
        (unsigned int)device.generation, (unsigned int)device.slot_id,
        result == XHCI_BULK_OUT_OK ?  "SUBMITTED" : "NOT SUBMITTED");
    console_printf("  Result : %u\n", (unsigned int)result);
    return;
  }
  enum { ASYNC_STATUS, ASYNC_REAP } action;
  if (next_word_is(&args, "status")) {
    action = ASYNC_STATUS;
  } else if (next_word_is(&args, "reap")) {
    action = ASYNC_REAP;
  } else {
    console_puts("Usage: usb test midiasync "
        "[start | status | reap] <generation>\n");
    return;
  }
  uint32_t generation = 0U;
  if (!usb_parse_generation(args, &generation)) {
    console_puts("Usage: usb test midiasync "
        "[start | status | reap] <generation>\n");
    return;
  }
  if (midi_output_owns_generation(generation)) {
    console_puts("Bulk OUT owned by the MIDI output service.\n");
    return;
  }
  usb_device_t device = {0};
  if (!usb_get_device_by_generation(generation, &device) ||
      device.slot_id == 0U) {
    console_puts("Device generation unavailable.\n");
    return;
  }
  xhci_bulk_out_async_info_t info = {0};
  bool available = action == ASYNC_STATUS ?
      xhci_bulk_out_async_get(device.slot_id, &info) :
      xhci_bulk_out_async_reap(device.slot_id, &info);
  if (!available) {
    console_puts("Async Bulk OUT result unavailable.\n");
    return;
  }
  console_printf("Async Bulk OUT (generation %u):\n",
      (unsigned int)generation);
  console_printf("  State      : %s\n",
      xhci_async_out_state_name(info.state));
  console_printf("  Result     : %u\n", (unsigned int)info.result);
  console_printf("  Completion : %u\n",
      (unsigned int)info.transfer.completion_code);
  console_printf("  TRB        : %016llx\n",
      (unsigned long long)info.transfer.transfer_trb);
  console_printf("  Elapsed    : %llu us\n",
      (unsigned long long)info.transfer.elapsed_us);
  if (action == ASYNC_REAP)
    console_puts("  Acknowledged\n");
}

static void cmd_usb_test_midiin_stress(const char *args) {
  if (next_word_is(&args, "reset") && end_of_args(args)) {
    bool reset = usb_midi_in_stress_reset();
    console_printf("USB-MIDI IN stress reset: %s\n",
        reset ? "OK" : "NOT READY");
    return;
  }
  if (next_word_is(&args, "start")) {
    usb_device_t device = {0};
    if (!usb_test_midi_select(args, &device)) {
      console_puts("Usage: usb test midiin stress start <generation>\n");
      return;
    }
    bool started = usb_midi_in_stress_start_generation(device.generation);
    console_printf("USB-MIDI IN stress: %s\n",
        started ? "STARTED" : "NOT READY");
    if (started) {
      console_printf("  Generation: %u\n", (unsigned int)device.generation);
      console_printf("  Slot      : %u\n", (unsigned int)device.slot_id);
      console_puts("Send 600 test messages from Windows.\n");
    }
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
    console_printf("  Transfers : %u\n", (unsigned int)info.transfers);
    console_printf("  Generation: %u\n", (unsigned int)info.generation);
    console_printf("  Slot      : %u\n", (unsigned int)info.slot_id);
    console_printf("  Packets   : %u / %u\n",
        (unsigned int)info.packets, USB_MIDI_IN_STRESS_PACKETS);
    console_printf("  Bytes     : %u\n", (unsigned int)info.bytes);
    console_printf("  Wraps     : %u\n", (unsigned int)info.wraps);
    console_printf("  Elapsed   : %llu ms\n",
        (unsigned long long)(info.elapsed_us / 1000ULL));
    console_printf("  Discarded : %llu transfers\n",
        (unsigned long long)info.discarded_transfers);
    if (info.state == USB_MIDI_IN_STRESS_FAILED) {
      console_printf("  Error     : %u\n", (unsigned int)info.error);
      console_printf("  Transfer  : %u\n", (unsigned int)info.fault_transfer);
      console_printf("  Packet    : %u\n", (unsigned int)info.fault_packet);
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
    console_puts("Usage: usb test midiin stress "
      "[start <generation> | status | reset]\n");
}

static const char *usb_bulk_in_arm_result_name(
    xhci_bulk_in_arm_result_t result) {
  switch (result) {
    case XHCI_BULK_IN_ARM_OK:
      return "STARTED";
    case XHCI_BULK_IN_ARM_NOT_READY:
      return "NOT READY";
    case XHCI_BULK_IN_ARM_ALREADY_STARTED:
      return "ALREADY STARTED";
    case XHCI_BULK_IN_ARM_ALLOCATION_FAILED:
      return "ALLOCATION FAILED";
    case XHCI_BULK_IN_ARM_DMA_FAILED:
      return "DMA FAILED";
    case XHCI_BULK_IN_ARM_RING_FAILED:
      return "RING FAILED";
    case XHCI_BULK_IN_ARM_WAITER_FAILED:
      return "WAITER FAILED";
    default:
      return "UNKNOWN";
  }
}

static void cmd_usb_test_midiin(const char *args) {
  const char *sub_args = args;
  if (next_word_is(&sub_args, "stress")) {
    cmd_usb_test_midiin_stress(sub_args);
    return;
  }
  // Identify the requested operation before
  // resolving its mandatory device generation.
  enum { MIDI_IN_STREAM, MIDI_IN_READ, MIDI_IN_STATUS } action;
  sub_args = args;
  if (next_word_is(&sub_args, "stream")) {
    action = MIDI_IN_STREAM;
  } else if (next_word_is(&sub_args, "read")) {
    action = MIDI_IN_READ;
  } else if (next_word_is(&sub_args, "status")) {
    action = MIDI_IN_STATUS;
  } else {
    console_puts("Usage: usb test midiin "
        "[stream | read | status] <generation>\n");
    return;
  }
  usb_device_t device = {0};
  if (!usb_test_midi_select(sub_args, &device)) {
    console_puts("No active USB-MIDI device or invalid generation.\n");
    console_puts("Usage: usb test midiin "
        "[stream | read | status] <generation>\n");
    return;
  }
  console_printf("USB-MIDI IN target: generation %u, slot %u\n",
      (unsigned int)device.generation, (unsigned int)device.slot_id);
  // The stress diagnostic owns the receive queue
  // only for its recorded device incarnation.
  // Other generations may use their own receivers.
  usb_midi_in_stress_info_t stress = {0};
  bool stress_owns_receiver = usb_midi_in_stress_get_info(&stress) &&
      stress.state != USB_MIDI_IN_STRESS_IDLE &&
      stress.generation == device.generation;
  // Persistent receivers own their xHCI queues.
  // Diagnostic commands may inspect stream
  // statistics, but must not start or drain them.
  if (action != MIDI_IN_STATUS &&
      usb_midi_rx_owns_generation(device.generation)) {
    console_puts("USB-MIDI IN stream owned by persistent receiver.\n");
    return;
  }
  if (action == MIDI_IN_STREAM) {
    if (stress_owns_receiver) {
      console_puts("USB-MIDI IN stream owned by stress test.\n");
      return;
    }
    xhci_bulk_in_arm_result_t result =
        xhci_bulk_in_stream_start(device.slot_id);
    console_printf("USB-MIDI IN stream start: %s (%u)\n",
        usb_bulk_in_arm_result_name(result), (unsigned int)result);
    return;
  }
  if (action == MIDI_IN_READ) {
    // Never remove a transfer from the stress
    // diagnostic's own receive queue.
    if (stress_owns_receiver) {
      console_puts("USB-MIDI IN stream owned by stress test.\n");
      return;
    }
    xhci_bulk_in_info_t info = {0};
    if (!xhci_bulk_in_stream_read(device.slot_id, &info)) {
      console_puts("USB-MIDI IN queue empty.\n");
      return;
    }
    // An xHCI receive completion must identify
    // the slot from which it was consumed.
    if (info.slot_id != device.slot_id) {
      console_puts("USB-MIDI IN transfer: SLOT MISMATCH\n");
      return;
    }
    console_puts("USB-MIDI IN transfer:\n");
    console_printf("  Completion : %u\n", (unsigned int)info.completion_code);
    console_printf("  TRB        : %016llx\n",
        (unsigned long long)info.transfer_trb);
    console_printf("  Received   : %u bytes\n", (unsigned int)info.received);
    if (info.received % 4U != 0U)
      console_puts("  Warning: incomplete USB-MIDI packet.\n");
    for (uint32_t i = 0U;
        i + 3U < info.received; i += 4U) {
      console_printf("  Packet %u: %02x %02x %02x %02x\n",
          (unsigned int)(i / 4U), (unsigned int)info.data[i],
          (unsigned int)info.data[i + 1U],
          (unsigned int)info.data[i + 2U],
          (unsigned int)info.data[i + 3U]);
    }
    return;
  }
  // MIDI_IN_STATUS: the selected active generation
  // owns the stream whose statistics we inspect.
  xhci_bulk_in_stream_stats_t stats = {0};
  if (!xhci_bulk_in_stream_get_stats(device.slot_id, &stats) ||
      !stats.started) {
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
  console_printf("  Last arm    : %u\n", (unsigned int)stats.last_arm_result);
}

static void cmd_usb_test_midiwrap(const char *args) {
  usb_device_t device = {0};
  usb_midi_config_t midi = {0};
  unsigned int cable = 0U;
  if (!usb_test_midi_out_select( args, &device, &midi, &cable)) {
    console_puts("No active USB-MIDI device or invalid arguments.\n");
    console_puts("Usage: usb test midiwrap <generation> [cable N]\n");
    return;
  }
  const uint8_t message[3] = {0xbfU, 0x66U, 0x00U};
  uint8_t packet[4] = {0};
  if (!usb_midi_out_packet( &midi, cable, 0x0bU, message, packet)) {
    console_printf("USB-MIDI cable %u unavailable "
        "(Bulk OUT jacks: %u, maximum supported: %u).\n",
        cable, (unsigned int)midi.bulk_out_jacks, USB_MIDI_MAX_CABLES);
    return;
  }
  console_printf("USB-MIDI target: generation %u, slot %u, cable %u\n",
      (unsigned int)device.generation, (unsigned int)device.slot_id, cable);
  const unsigned int transfers = 520U;
  const unsigned int ring_entries = 255U;
  const uint64_t trb_size = 16U;
  uint64_t ring_base = 0U;
  unsigned int first_index = 0U;
  unsigned int completed = 0U;
  unsigned int wraps = 0U;
  uint64_t started = timer_now_us();
  console_printf("USB-MIDI ring-wrap stress test (%u transfers)\n", transfers);
  console_puts("  WARNING: Sends real MIDI OUT messages: "
      "channel 16, CC 102.\n");
  for (unsigned int i = 0U; i < transfers; i++) {
    // A long-running diagnostic must not continue
    // against a recycled or reassigned slot.
    usb_device_t current = {0};
    if (!usb_test_midi_active_generation(
            device.generation, device.slot_id, &current)) {
      console_printf("Transfer %u: device ownership changed\n", i + 1U);
      break;
    }
    // The header retains its selected virtual
    // cable and Control Change CIN. Only the
    // controller value changes per transfer.
    packet[3] = (uint8_t)(i & 0x7fU);
    xhci_bulk_out_info_t info = {0};
    xhci_bulk_out_result_t result = xhci_downstream_bulk_out(
        device.slot_id, packet, sizeof(packet), &info);
    if (result != XHCI_BULK_OUT_OK) {
      console_printf("Transfer %u failed: %u, completion %u\n",
          i + 1U, (unsigned int)result, (unsigned int)info.completion_code);
      break;
    }
    if (info.completion_code != 1U ||
        info.event_parameter != info.transfer_trb) {
      console_printf("Transfer %u: unexpected completion\n", i + 1U);
      break;
    }
    if (i == 0U) {
      // The DMA ring occupies one aligned page.
      // Earlier transfers may have advanced it.
      ring_base = info.transfer_trb & ~((uint64_t)PAGE_SIZE - 1ULL);
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
      console_printf( "Transfer %u: incorrect TRB address\n", i + 1U);
      console_printf("  Expected: %016llx\n", (unsigned long long)expected);
      console_printf("  Actual  : %016llx\n",
          (unsigned long long)info.transfer_trb);
      break;
    }
    completed++;
    if (i != 0U && absolute_index % ring_entries == 0U) {
      wraps++;
      console_printf("  Wrap %u: transfer %u, TRB %016llx\n",
          wraps, i + 1U, (unsigned long long)info.transfer_trb);
    }
    // Allow the adapter's physical DIN output
    // to keep up with the test traffic.
    usb_test_delay_ms(4U);
  }
  uint64_t elapsed = timer_now_us() - started;
  console_printf("  Completed : %u / %u\n", completed, transfers);
  console_printf("  Wraps     : %u\n", wraps);
  console_printf("  Elapsed   : %llu ms\n",
      (unsigned long long)(elapsed / 1000ULL));
  console_printf("USB-MIDI ring-wrap: %s\n",
      completed == transfers && wraps >= 2U ?  "PASS" : "FAILED");
}

static void cmd_usb_changes(void) {
  usb_change_stats_t stats = {0};
  if (!usb_change_get_stats(&stats)) {
    console_puts("USB change statistics unavailable.\n");
    return;
  }
  console_puts("USB downstream change events:\n");
  console_printf("  Recorded  : %u\n", stats.recorded);
  console_printf("  Consumed  : %u\n", stats.consumed);
  console_printf("  Dropped   : %u\n", stats.dropped);
  console_printf("  Queued    : %u\n", (unsigned int)stats.queued);
  console_printf("  Acked     : %u\n", stats.acknowledgements);
  console_printf("  Errors    : %u\n", stats.errors);
  console_printf("  Failed    : %s\n", stats.failed ? "Yes" : "No");
  if (stats.failed)
    console_printf("  Error port: %u\n", (unsigned int)stats.last_error_port);
}

static void cmd_usb_changes_dump(void) {
  usb_change_event_t event = {0};
  unsigned int count = 0U;
  console_puts("USB retained downstream changes:\n");
  while (usb_change_read(&event)) {
    console_printf( "  #%u port %u: status 0x%04x, change 0x%04x\n",
        event.sequence, (unsigned int)event.port,
        (unsigned int)event.status, (unsigned int)event.change);
    count++;
  }
  if (count == 0U)
    console_puts("  No queued events.\n");
  usb_change_stats_t stats = {0};
  console_printf("  Records read: %u\n", count);
  if (usb_change_get_stats(&stats))
    console_printf("  Queued now  : %u\n", (unsigned int)stats.queued);
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

static void cmd_usb_auto_status(void) {
  usb_auto_enum_info_t info = {0};
  if (!usb_auto_enum_get_info(&info)) {
    console_puts("USB auto enumeration unavailable.\n");
    return;
  }
  console_puts("USB automatic enumeration:\n");
  console_printf("  Enabled    : %s\n", info.enabled ? "Yes" : "No");
  console_printf("  Pending    : %s\n", info.pending ? "Yes" : "No");
  console_printf("  Attempts   : %u\n", (unsigned int)info.attempts);
  console_printf("  Successful : %u\n", (unsigned int)info.successes);
  console_printf("  Failed     : %u\n", (unsigned int)info.failures);
  if (info.attempts != 0U)
    console_printf("  Last result: %u\n", (unsigned int)info.last_result);
}

static void cmd_usb_auto_ports(void) {
  usb_auto_enum_select_info_t info = {0};
  if (!usb_auto_enum_select_inspect(&info)) {
    console_puts("USB candidate selection unavailable.\n");
    return;
  }
  console_puts("USB per-port enumeration scheduler:\n");
  console_printf("  Pending mask  : 0x%04x\n",
      (unsigned int)info.pending_ports);
  console_printf("  Eligible mask : 0x%04x\n",
      (unsigned int)info.eligible_ports);
  console_printf("  Record mask   : 0x%04x\n",
      (unsigned int)info.record_ready_ports);
  console_printf("  Next port     : %u\n", (unsigned int)info.next_port);
  console_printf("  Free contexts : %u\n", info.free_contexts);
  console_printf("  Waiter room   : %u\n", info.waiter_headroom);
  console_printf("  Resources     : %s\n",
      info.resources_ready ? "Ready" : "Blocked");
  if (info.selected_port != 0U)
    console_printf("  Candidate     : Port %u\n",
        (unsigned int)info.selected_port);
  else
    console_puts("  Candidate     : None\n");
  console_printf("  Would admit   : %s\n", info.would_admit ? "Yes" : "No");
  for (unsigned int port = 1U; port <= 4U; port++) {
    uint16_t bit = (uint16_t)(1U << port);
    uint32_t epoch = usb_auto_enum_port_request_epoch((uint8_t)port);
    const char *state = "None";
    if (info.pending_ports & bit)
      state = (info.eligible_ports & bit) ? "Eligible" : "Blocked";
    console_printf("  Port %u        : %s", port, state);
    if (epoch != 0U)
      console_printf(" (request %u)", (unsigned int)epoch);
    console_putc('\n');
  }
  console_puts("  Dispatch      : Independent per-port\n");
}

static void cmd_usb_resources(void) {
  xhci_resource_info_t info = {0};
  if (!xhci_resource_get_info(&info)) {
    console_puts("xHCI resource census unavailable.\n");
    return;
  }
  console_puts("xHCI resource census:\n");
  console_printf("  Downstream contexts : %u / %u owned\n",
      info.downstream_owned, info.downstream_capacity);
  console_printf("  Free contexts       : %u\n", info.downstream_free);
  console_printf("  Assigned slots      : %u\n", info.assigned_slots);
  console_printf("  Pending slots       : %u\n", info.pending_slots);
  console_puts("  DMA allocations (pages):\n");
  console_printf("    Controller        : %u\n",
      (unsigned int)info.controller_dma_pages);
  console_printf("    Internal hub      : %u\n",
      (unsigned int)info.hub_dma_pages);
  console_printf("    Downstream        : %u\n",
      (unsigned int)info.downstream_dma_pages);
  console_printf("  Active waiters      : %u / %u\n",
      info.active_waiters, info.waiter_capacity);
  console_printf("    Peak active       : %u\n", info.waiter_high_water);
  console_printf("    Command reserve   : %u\n", info.command_waiter_reserve);
  console_printf("    Capacity rejected : %u\n",
      (unsigned int)info.waiter_capacity_rejections);
  console_printf("    Duplicate rejected: %u\n",
      (unsigned int)info.waiter_duplicate_rejections);
  console_printf("    Invalid rejected  : %u\n",
      (unsigned int)info.waiter_invalid_rejections);
  console_printf("    Completed         : %u\n", info.completed_waiters);
  console_printf("    Command           : %u\n", info.command_waiters);
  console_printf("    Internal hub      : %u\n", info.hub_waiters);
  console_printf("    Downstream        : %u\n", info.downstream_waiters);
  console_printf("    Orphaned          : %u\n", info.orphan_waiters);
  console_printf("  Controller running  : %s\n",
      info.controller_running ? "Yes" : "No");
  console_printf("  Command outstanding : %s\n",
      info.command_outstanding ? "Yes" : "No");
  console_printf("  Command ring failed : %s\n",
      info.command_ring_failed ? "Yes" : "No");
  console_printf("  Event dispatch fault: %s\n",
      info.dispatch_failed ? "Yes" : "No");
}

static void cmd_usb_auto(const char *args) {
  if (end_of_args(args)) {
    cmd_usb_auto_status();
    return;
  }
  const char *word = args;
  if (next_word_is(&word, "on") && end_of_args(word)) {
    usb_auto_enum_set_enabled(true);
    console_puts("USB auto enumeration: ON\n");
    return;
  }
  word = args;
  if (next_word_is(&word, "off") && end_of_args(word)) {
    usb_auto_enum_set_enabled(false);
    console_puts("USB auto enumeration: OFF\n");
    return;
  }
  word = args;
  if (next_word_is(&word, "status") && end_of_args(word)) {
    cmd_usb_auto_status();
    return;
  }
  word = args;
  if (next_word_is(&word, "ports") && end_of_args(word)) {
    cmd_usb_auto_ports();
    return;
  }
  console_puts("Usage: usb auto [on|off|status|ports]\n");
}

static void cmd_usb(const char *args) {
  // The VL805 is configured at boot. Subcommands inspect
  // controller state and exercise explicit diagnostics.
  const char *other_args = args;
  if (next_word_is(&args, "topology")) {
    if (end_of_args(args)) {
      cmd_usb_topology(false);
      return;
    }
    if (next_word_is(&args, "detail") && end_of_args(args)) {
      cmd_usb_topology(true);
      return;
    }
    console_puts("Usage: usb topology [detail]\n");
    return;
  }
  if (next_word_is(&args, "hubirq")) {
    if (end_of_args(args)) {
      cmd_usb_hubirq();
      return;
    }
    console_puts("Usage: usb hubirq\n");
    return;
  }
  if (next_word_is(&args, "auto")) {
    cmd_usb_auto(args);
    return;
  }
  if (next_word_is(&args, "recover")) {
    if (next_word_is(&args, "status")) {
      cmd_usb_recover_status(args);
      return;
    }
    cmd_usb_recover(args);
    return;
  }
  if (next_word_is(&args, "lifecycle")) {
    if (next_word_is(&args, "status")) {
      cmd_usb_lifecycle_status(args);
      return;
    }
    if (next_word_is(&args, "port")) {
      cmd_usb_lifecycle_port(args);
      return;
    }
    console_puts("Usage: usb lifecycle "
        "[status <generation> | port <generation>]\n");
    return;
  }
  if (next_word_is(&args, "resources") && end_of_args(args)) {
    cmd_usb_resources();
    return;
  }
  if (next_word_is(&args, "transfers")) {
    cmd_usb_transfers(args);
    return;
  }
  if (next_word_is(&args, "changes")) {
    if (end_of_args(args)) {
      cmd_usb_changes();
      return;
    }
    if (next_word_is(&args, "status") && end_of_args(args)) {
      cmd_usb_changes();
      return;
    }
    if (next_word_is(&args, "dump") && end_of_args(args)) {
      cmd_usb_changes_dump();
      return;
    }
    console_puts("Usage: usb changes [status | dump]\n");
    return;
  }
  if (next_word_is(&args, "devices") && end_of_args(args)) {
    cmd_usb_devices();
    return;
  }
  if (next_word_is(&args, "enumerate")) {
    cmd_usb_enumerate(args);
    return;
  }
  if (next_word_is(&args, "enumstate")) {
    cmd_usb_enumstate(args);
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
  if (next_word_is(&other_args, "test")) {
    if (next_word_is(&other_args, "ep0match") &&
        end_of_args(other_args)) {
      cmd_usb_test_ep0match();
      return;
    }
    if (next_word_is(&other_args, "noops") &&
        end_of_args(other_args)) {
      cmd_usb_test_noops();
      return;
    }
    if (next_word_is(&other_args, "midiout")) {
      cmd_usb_test_midiout(other_args);
      return;
    }
    if (next_word_is(&other_args, "midiwrap")) {
      cmd_usb_test_midiwrap(other_args);
      return;
    }
    if (next_word_is(&other_args, "midiin")) {
      cmd_usb_test_midiin(other_args);
      return;
    }
    if (next_word_is(&other_args, "midiasync")) {
      cmd_usb_test_midiasync(other_args);
      return;
    }
    console_puts(
        "Usage: usb test [ep0match | "
        "noops | midiout | midiwrap | midiin]\n");
    return;
  }
  if (next_word_is(&args, "state") && end_of_args(args)) {
    cmd_usb_state();
    return;
  }
  if (!end_of_args(args)) {
    console_puts(
        "Usage: usb [state | events | changes | devices | topology | "
        "test | recover | transfers | resources | auto | hubirq | "
        "lifecycle | hubports | enumerate | enumstate]\n");
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
  if (!end_of_args(args)) {
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

typedef struct {
  const char *name;
  console_command_fn handler;
  const char *description;
} console_command_t;

static void cmd_memory(const char *args) {
  (void)args;
  uint32_t base;
  uint32_t size;
  uint32_t revision;
  uint32_t clock_hz;

  console_puts("Firmware mailbox:\n");
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
  console_puts("  UART clock     : ");
  if (mailbox_get_clock_rate(MAILBOX_CLOCK_UART, &clock_hz))
    console_printf("%u Hz\n", (unsigned int)clock_hz);
  else
    console_puts("FAILED\n");
  console_puts("\nARM memory allocation:\n");
  if (!mailbox_get_arm_memory(&base, &size)) {
    console_puts("  Firmware memory query failed.\n");
    return;
  }
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

static int midi_hex_digit(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';

  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;

  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;

  return -1;
}

static bool midi_next_hex_byte(const char **input,
    uint8_t *value) {

  if (!input || !*input || !value)
    return false;

  const char *p = *input;

  while (*p == ' ' || *p == '\t')
    p++;

  int high = midi_hex_digit(p[0]);

  if (high < 0)
    return false;

  int low = midi_hex_digit(p[1]);

  if (low < 0)
    return false;

  if (p[2] != '\0' && p[2] != ' ' && p[2] != '\t')
    return false;

  *value = (uint8_t)((high << 4) | low);
  *input = p + 2;

  return true;
}

static void cmd_midi_decode(const char *args) {
  uint8_t packet[4] = {0};
  if (!midi_next_hex_byte(&args, &packet[0]) ||
      !midi_next_hex_byte(&args, &packet[1]) ||
      !midi_next_hex_byte(&args, &packet[2]) ||
      !midi_next_hex_byte(&args, &packet[3]) ||
      !end_of_args(args)) {
    console_puts("Usage: midi decode <hh> <hh> <hh> <hh>\n");
    return;
  }
  usb_midi_event_t event = {0};
  if (!usb_midi_decode_packet(packet, &event)) {
    console_puts("USB-MIDI: reserved or invalid CIN.\n");
    return;
  }
  console_puts("USB-MIDI decoded event:\n");
  console_printf("  Cable  : %u\n", (unsigned int)event.cable);
  console_printf("  CIN    : 0x%01x\n", (unsigned int)event.cin);
  console_printf("  Length : %u\n", (unsigned int)event.length);
  console_puts("  Data   :");
  for (unsigned int i = 0U; i < event.length; i++) {
    console_putc(' ');
    console_puthex(event.data[i], 2U);
  }
  console_putc('\n');
}

static const char *midi_rx_state_name(usb_midi_rx_state_t state) {
  switch (state) {
    case USB_MIDI_RX_IDLE: return "Idle";
    case USB_MIDI_RX_RUNNING: return "Running";
    case USB_MIDI_RX_DISCONNECTED: return "Disconnected";
    case USB_MIDI_RX_FAILED: return "Failed";
    default: return "Unknown";
  }
}

static void cmd_midi_rx(const char *args) {
  enum { RX_START, RX_STATUS, RX_READ } action;
  if (next_word_is(&args, "start")) {
    action = RX_START;
  } else if (next_word_is(&args, "status")) {
    action = RX_STATUS;
  } else if (next_word_is(&args, "read")) {
    action = RX_READ;
  } else {
    console_puts("Usage: midi rx [start | status | read] <generation>\n");
    return;
  }
  uint32_t generation = 0U;
  if (!usb_parse_generation(args, &generation)) {
    console_puts("Usage: midi rx [start | status | read] <generation>\n");
    return;
  }
  if (action == RX_START) {
    bool started = usb_midi_rx_start_generation(generation);
    console_printf("MIDI receiver: %s\n", started ? "STARTED" : "NOT READY");
    return;
  }
  if (action == RX_STATUS) {
    usb_midi_rx_info_t info = {0};
    if (!usb_midi_rx_get_info(generation, &info)) {
      console_puts("MIDI receiver unavailable.\n");
      return;
    }
    console_printf("MIDI receiver (generation %u):\n",
        (unsigned int)generation);
    console_printf("  State      : %s\n", midi_rx_state_name(info.state));
    console_printf("  Slot       : %u\n", (unsigned int)info.slot_id);
    console_printf("  IN cables  : %u\n", (unsigned int)info.cables);
    console_printf("  Transfers  : %llu\n",
        (unsigned long long)info.transfers);
    console_printf("  Events     : %llu\n", (unsigned long long)info.events);
    console_printf("  Rejected   : %llu\n",
        (unsigned long long)info.rejected);
    console_printf("  Staged     : %u\n", info.staged);
    console_printf("  Queue      : %u / %u\n",
        info.queue_depth, USB_MIDI_RX_QUEUE_CAPACITY);
    console_printf("  Queue peak : %u\n", info.queue_peak);
    return;
  }
  usb_midi_event_t event = {0};
  if (!usb_midi_rx_read(generation, &event)) {
    console_puts("No decoded MIDI event available.\n");
    return;
  }
  console_printf("MIDI RX event (generation %u):\n",
      (unsigned int)generation);
  console_printf("  Cable  : %u\n", (unsigned int)event.cable);
  console_printf("  CIN    : 0x%01x\n", (unsigned int)event.cin);
  console_printf("  Length : %u\n", (unsigned int)event.length);
  console_puts("  Data   :");
  for (unsigned int i = 0U; i < event.length; i++) {
    console_putc(' ');
    console_puthex(event.data[i], 2U);
  }
  console_putc('\n');
}

static void cmd_midi_monitor(const char *args) {
  enum { MONITOR_START, MONITOR_STOP, MONITOR_STATUS } action;
  if (next_word_is(&args, "start")) {
    action = MONITOR_START;
  } else if (next_word_is(&args, "stop")) {
    action = MONITOR_STOP;
  } else if (next_word_is(&args, "status")) {
    action = MONITOR_STATUS;
  } else {
    console_puts("Usage: midi monitor "
        "[start | stop | status] <generation>\n");
    return;
  }
  uint32_t generation = 0U;
  if (!usb_parse_generation(args, &generation)) {
    console_puts("Usage: midi monitor "
        "[start | stop | status] <generation>\n");
    return;
  }
  if (action == MONITOR_START) {
    bool started = midi_monitor_start(generation);
    console_printf("MIDI monitor: %s\n", started ? "ATTACHED" : "NOT READY");
    return;
  }
  if (action == MONITOR_STOP) {
    bool stopped = midi_monitor_stop(generation);
    console_printf("MIDI monitor: %s\n", stopped ? "DETACHED" : "NOT READY");
    return;
  }
  midi_monitor_info_t info = {0};
  if (!midi_monitor_get_info(generation, &info)) {
    console_puts("MIDI monitor unavailable.\n");
    return;
  }
  console_printf("MIDI monitor (generation %u):\n", (unsigned int)generation);
  console_printf("  Attached : %s\n", info.attached ? "Yes" : "No");
  console_printf("  Events   : %llu\n", (unsigned long long)info.events);
  for (unsigned int cable = 0U; cable < USB_MIDI_MAX_CABLES; cable++) {
    if (info.by_cable[cable] == 0U)
      continue;
    console_printf("  Cable %u  : %llu events\n",
        cable, (unsigned long long)info.by_cable[cable]);
  }
  if (!info.have_last)
    return;
  console_printf("  Last     : cable %u, CIN 0x%x\n",
      (unsigned int)info.last.cable, (unsigned int)info.last.cin);
  console_puts("  Data     :");
  for (unsigned int i = 0U; i < info.last.length; i++) {
    console_putc(' ');
    console_puthex(info.last.data[i], 2U);
  }
  console_putc('\n');
}

static void cmd_midi_auto(const char *args) {
  const char *p = args;
  if (next_word_is(&p, "on") && end_of_args(p)) {
    midi_auto_set_enabled(true);
    console_puts("Automatic MIDI reception: ON\n");
    return;
  }
  p = args;
  if (next_word_is(&p, "off") && end_of_args(p)) {
    midi_auto_set_enabled(false);
    console_puts("Automatic MIDI reception: OFF\n");
    console_puts("Existing receivers retain their ownership.\n");
    return;
  }
  p = args;
  if (end_of_args(args) || (next_word_is(&p, "status") && end_of_args(p))) {

    midi_auto_info_t info = {0};
    if (!midi_auto_get_info(&info)) {
      console_puts("Automatic MIDI reception unavailable.\n");
      return;
    }
    console_puts("Automatic MIDI reception:\n");
    console_printf("  Enabled    : %s\n", info.enabled ? "Yes" : "No");
    console_printf("  Attempts   : %u\n", (unsigned int)info.attempts);
    console_printf("  Started    : %u\n", (unsigned int)info.started);
    console_printf("  Skipped    : %u\n", (unsigned int)info.skipped);
    console_printf("  Failed     : %u\n", (unsigned int)info.failed);
    if (info.attempts != 0U)
      console_printf("  Last gen   : %u\n",
          (unsigned int)info.last_generation);
    return;
  }
  console_puts("Usage: midi auto [on | off | status]\n");
}

static midi_stream_parser_t console_stream_parser;

static bool midi_valid_byte_list(const char *args) {
  uint8_t byte = 0U;
  bool found = false;
  while (!end_of_args(args)) {
    if (!midi_next_hex_byte(&args, &byte))
      return false;
    found = true;
  }
  return found;
}

static unsigned int midi_console_emit(
    const midi_stream_output_t *output, bool show_sysex) {
  unsigned int short_count = 0U;
  for (unsigned int i = 0U; i < output->count; i++) {
    const midi_stream_event_t *event = &output->events[i];
    if (event->type == MIDI_STREAM_SHORT) {
      const midi_short_message_t *message = &event->message;
      console_puts("  ");
      console_puthex(message->status, 2U);
      for (unsigned int j = 1U; j < message->length; j++) {
        console_putc(' ');
        console_puthex(message->data[j - 1U], 2U);
      }
      console_putc('\n');
      short_count++;
      continue;
    }
    if (!show_sysex)
      continue;
    switch (event->type) {
      case MIDI_STREAM_SYSEX_BEGIN:
        console_puts("  SysEx BEGIN\n");
        break;
      case MIDI_STREAM_SYSEX_DATA:
        console_puts("  SysEx DATA ");
        console_puthex(event->byte, 2U);
        console_putc('\n');
        break;
      case MIDI_STREAM_SYSEX_END:
        console_puts("  SysEx END\n");
        break;
      case MIDI_STREAM_SYSEX_ABORT:
        console_puts("  SysEx ABORT");
        if (event->byte != 0U) {
          console_puts(" (status ");
          console_puthex(event->byte, 2U);
          console_putc(')');
        }
        console_putc('\n');
        break;
      default:
        break;
    }
  }
  return short_count;
}

static unsigned int midi_console_feed(midi_stream_parser_t *parser,
    const char *args, bool show_sysex) {
  unsigned int short_count = 0U;
  while (!end_of_args(args)) {
    uint8_t byte = 0U;
    // The complete argument list was validated
    // before this function was called.
    if (!midi_next_hex_byte(&args, &byte))
      break;
    midi_stream_output_t output = {0};
    if (!midi_stream_parser_feed(parser, byte, &output))
      break;
    short_count += midi_console_emit(&output, show_sysex);
  }
  return short_count;
}

static void cmd_midi_stream(const char *args) {
  const char *p = args;
  if (next_word_is(&p, "reset") && end_of_args(p)) {
    midi_stream_output_t output = {0};
    midi_stream_parser_cancel(&console_stream_parser, &output);
    midi_console_emit(&output, true);
    console_puts("MIDI stream parser reset.\n");
    return;
  }
  p = args;
  if (next_word_is(&p, "status") && end_of_args(p)) {
    console_puts("MIDI stream parser state:\n");
    console_printf("  SysEx   : %s\n",
        console_stream_parser.in_sysex ? "Yes" : "No");
    console_printf("  Running : 0x%02x\n",
        (unsigned int)console_stream_parser.running_status);
    console_printf("  Active  : 0x%02x\n",
        (unsigned int)console_stream_parser.active_status);
    return;
  }
  if (!midi_valid_byte_list(args)) {
    console_puts("Usage: midi stream [reset | status | <hh> [hh ...]]\n");
    return;
  }
  console_puts("MIDI stream events:\n");
  midi_console_feed(&console_stream_parser, args, true);
}

static void cmd_midi_input(const char *args) {
  enum { INPUT_START, INPUT_STOP, INPUT_STATUS, INPUT_READ } action;

  if (next_word_is(&args, "start")) {
    action = INPUT_START;
  } else if (next_word_is(&args, "stop")) {
    action = INPUT_STOP;
  } else if (next_word_is(&args, "status")) {
    action = INPUT_STATUS;
  } else if (next_word_is(&args, "read")) {
    action = INPUT_READ;
  } else {
    console_puts("Usage: midi input [start | stop | status | read] <generation>\n");
    return;
  }

  uint32_t generation = 0U;

  if (!usb_parse_generation(args, &generation)) {
    console_puts("Usage: midi input [start | stop | status] <generation>\n");
    return;
  }

  if (action == INPUT_START) {
    bool started = midi_input_start_generation(generation);
    console_printf("MIDI input: %s\n", started ? "ATTACHED" : "NOT READY");
    return;
  }

  if (action == INPUT_STOP) {
    bool stopped = midi_input_stop_generation(generation);
    console_printf("MIDI input: %s\n", stopped ? "DETACHED" : "NOT READY");
    return;
  }

  if (action == INPUT_READ) {
    midi_input_event_t received = {0};

    if (!midi_input_read(generation, &received)) {
      console_puts("MIDI input queue empty.\n");
      return;
    }

    console_printf( "MIDI input event (generation %u, cable %u):\n",
        (unsigned int)received.generation, (unsigned int)received.cable);

    // Reuse the existing stream-event formatter.
    midi_stream_output_t output = {
      .count = 1U
    };

    output.events[0] = received.event;
    midi_console_emit(&output, true);
    return;
  }

  midi_input_info_t info = {0};

  if (!midi_input_get_info(generation, &info)) {
    console_puts("MIDI input unavailable.\n");
    return;
  }

  console_printf("MIDI input (generation %u):\n", (unsigned int)generation);
  console_printf("  Attached    : %s\n", info.attached ? "Yes" : "No");
  console_printf("  Lost        : %s\n", info.lost ? "Yes" : "No");
  console_printf("  Cables      : %u\n", (unsigned int)info.cables);
  console_printf("  Packets     : %llu\n", (unsigned long long)info.packets);
  console_printf("  Malformed   : %llu\n",
      (unsigned long long)info.malformed);
  console_printf("  Short       : %llu\n",
      (unsigned long long)info.short_messages);
  console_printf("  SysEx begin : %llu\n",
      (unsigned long long)info.sysex_begins);
  console_printf("  SysEx bytes : %llu\n",
      (unsigned long long)info.sysex_bytes);
  console_printf("  SysEx end   : %llu\n",
      (unsigned long long)info.sysex_ends);
  console_printf("  SysEx abort : %llu\n",
      (unsigned long long)info.sysex_aborts);
  console_printf("  Queue       : %u / %u\n",
      info.queue_depth, MIDI_INPUT_QUEUE_CAPACITY);
  console_printf("  Queue peak  : %u\n", info.queue_peak);
  console_printf("  Pending end : %u aborts\n", info.pending_aborts);
  console_printf("  Stalls      : %llu\n", (unsigned long long)info.stalls);

  if (!info.have_last)
    return;

  console_printf("  Last cable  : %u\n", (unsigned int)info.last_cable);
  console_puts("  Last event:\n");

  // Reuse the existing stream-event formatter.
  midi_stream_output_t output = {
    .count = 1U
  };

  output.events[0] = info.last;
  midi_console_emit(&output, true);
}

static const char *midi_output_state_name(midi_output_state_t state) {
  switch (state) {
    case MIDI_OUTPUT_IDLE: return "Idle";
    case MIDI_OUTPUT_RUNNING: return "Running";
    case MIDI_OUTPUT_STOPPED: return "Stopped";
    case MIDI_OUTPUT_DISCONNECTED: return "Disconnected";
    case MIDI_OUTPUT_FAILED: return "Failed";
    default: return "Unknown";
  }
}

static void cmd_midi_output_sysex(const char *args) {
  const char *p = args;
  if (next_word_is(&p, "begin")) {
    unsigned int generation = 0U;
    unsigned int cable = 0U;
    if (!next_uint(&p, &generation) || generation == 0U ||
        !next_uint(&p, &cable) || !end_of_args(p)) {
      console_puts("Usage: midi output sysex begin "
          "<generation> <cable>\n");
      return;
    }
    bool started = midi_output_sysex_begin(generation, cable);
    console_printf("MIDI SysEx: %s\n", started ? "STARTED" : "NOT READY");
    return;
  }
  p = args;
  if (next_word_is(&p, "write")) {
    unsigned int generation = 0U;
    if (!next_uint(&p, &generation) || generation == 0U || end_of_args(p)) {
      console_puts("Usage: midi output sysex write "
          "<generation> <hh> [hh ...]\n");
      return;
    }
    // Validate the whole argument list before
    // accepting any part of it.
    const char *probe = p;
    unsigned int total = 0U;
    uint8_t byte = 0U;
    while (!end_of_args(probe)) {
      if (!midi_next_hex_byte(&probe, &byte) || byte >= 0x80U) {
        console_puts("SysEx payload must contain "
            "seven-bit hexadecimal bytes.\n");
        return;
      }
      total++;
    }
    unsigned int accepted = 0U;
    while (!end_of_args(p)) {
      if (!midi_next_hex_byte(&p, &byte))
        break;
      if (!midi_output_sysex_write(generation, byte))
        break;
      accepted++;
    }
    console_printf("SysEx payload: %u / %u bytes accepted\n",
        accepted, total);
    if (accepted != total)
      console_puts("Retry starting with the first unaccepted byte.\n");
    return;
  }
  p = args;
  if (next_word_is(&p, "end")) {
    uint32_t generation = 0U;
    if (!usb_parse_generation(p, &generation)) {
      console_puts("Usage: midi output sysex end <generation>\n");
      return;
    }
    bool ended = midi_output_sysex_end(generation);
    console_printf("MIDI SysEx end: %s\n", ended ? "QUEUED" : "NOT READY");
    return;
  }
  console_puts("Usage: midi output sysex [begin | write | end]\n");
}

static void cmd_midi_output(const char *args) {
  const char *p = args;
  if (next_word_is(&p, "sysex")) {
    cmd_midi_output_sysex(p);
    return;
  }
  if (next_word_is(&p, "send")) {
    unsigned int generation = 0U;
    unsigned int cable = 0U;
    uint8_t bytes[3] = {0};
    unsigned int count = 0U;
    if (!next_uint(&p, &generation) || generation == 0U ||
        !next_uint(&p, &cable)) {
      console_puts("Usage: midi output send "
          "<generation> <cable> <hh> [hh ...]\n");
      return;
    }
    while (!end_of_args(p)) {
      if (count == 3U || !midi_next_hex_byte(&p, &bytes[count])) {
        console_puts("Usage: midi output send "
            "<generation> <cable> <hh> [hh ...]\n");
        return;
      }
      count++;
    }
    if (count == 0U) {
      console_puts("Missing MIDI message.\n");
      return;
    }
    midi_short_message_t message = {
      .status = bytes[0],
      .length = (uint8_t)count
    };
    if (count > 1U)
      message.data[0] = bytes[1];
    if (count > 2U)
      message.data[1] = bytes[2];
    bool queued = midi_output_enqueue_short(generation, cable, &message);
    console_printf("MIDI output: %s\n", queued ? "QUEUED" : "NOT QUEUED");
    return;
  }
  if (next_word_is(&p, "test")) {
    unsigned int generation = 0U;
    unsigned int count = 0U;
    if (!next_uint(&p, &generation) || generation == 0U ||
        !next_uint(&p, &count) || count == 0U ||
        count > 128U || !end_of_args(p)) {
      console_puts( "Usage: midi output test <generation> <count 1..128>\n");
      return;
    }
    console_puts("WARNING: Sends real MIDI OUT messages: "
        "channel 16, CC 102.\n");
    unsigned int accepted = 0U;
    for (unsigned int i = 0U; i < count; i++) {
      midi_short_message_t message = {
        .status = 0xbfU,
        .data = {0x66U, (uint8_t)(i & 0x7fU)},
        .length = 3U
      };
      if (midi_output_enqueue_short(generation, 0U, &message))
        accepted++;
    }
    console_printf("  Accepted : %u\n", accepted);
    console_printf("  Rejected : %u\n", count - accepted);
    return;
  }
  enum { OUTPUT_START, OUTPUT_STOP, OUTPUT_STATUS } action;
  if (next_word_is(&args, "start")) {
    action = OUTPUT_START;
  } else if (next_word_is(&args, "stop")) {
    action = OUTPUT_STOP;
  } else if (next_word_is(&args, "status")) {
    action = OUTPUT_STATUS;
  } else {
    console_puts("Usage: midi output "
        "[start | stop | status | send | test]\n");
    return;
  }
  uint32_t generation = 0U;
  if (!usb_parse_generation(args, &generation)) {
    console_puts("Usage: midi output "
        "[start | stop | status] <generation>\n");
    return;
  }
  if (action == OUTPUT_START) {
    bool started = midi_output_start_generation(generation);
    console_printf("MIDI output: %s\n", started ? "STARTED" : "NOT READY");
    return;
  }
  if (action == OUTPUT_STOP) {
    bool stopped = midi_output_stop_generation(generation);
    console_printf("MIDI output: %s\n", stopped ? "STOPPED" : "NOT READY");
    return;
  }
  midi_output_info_t info = {0};
  if (!midi_output_get_info(generation, &info)) {
    console_puts("MIDI output unavailable.\n");
    return;
  }
  console_printf("MIDI output (generation %u):\n", (unsigned int)generation);
  console_printf("  State       : %s\n", midi_output_state_name(info.state));
  console_printf("  Slot        : %u\n", (unsigned int)info.slot_id);
  console_printf("  OUT cables  : %u\n", (unsigned int)info.cables);
  console_printf("  Accepted    : %llu\n", (unsigned long long)info.accepted);
  console_printf("  Confirmed   : %llu\n",
      (unsigned long long)info.confirmed);
  console_printf("  Transfers   : %llu\n",
      (unsigned long long)info.transfers);
  console_printf("  Failures    : %llu\n", (unsigned long long)info.failures);
  console_printf("  Queue       : %u / %u\n",
      info.queue_depth, MIDI_OUTPUT_QUEUE_CAPACITY);
  console_printf("  Queue peak  : %u\n", info.queue_peak);
  console_printf("  In flight   : %u packets\n", info.in_flight);
  console_printf("  Full rejects: %llu\n",
      (unsigned long long)info.full_rejections);
  console_printf("  Last error  : %u\n", (unsigned int)info.last_error);
  console_printf("  SysEx active: %s\n", info.sysex_active ? "Yes" : "No");
  if (info.sysex_active) {
    console_printf("  SysEx cable : %u\n", (unsigned int)info.sysex_cable);
    console_printf("  SysEx staged: %u bytes\n",
        (unsigned int)info.sysex_staged);
  }
  console_printf("  SysEx starts: %llu\n",
      (unsigned long long)info.sysex_started);
  console_printf("  SysEx ends  : %llu\n",
      (unsigned long long)info.sysex_completed);
  console_printf("  SysEx data  : %llu bytes\n",
      (unsigned long long)info.sysex_payload_bytes);
}

static void cmd_midi(const char *args) {
  if (next_word_is(&args, "decode")) {
    cmd_midi_decode(args);
    return;
  }
  if (next_word_is(&args, "rx")) {
    cmd_midi_rx(args);
    return;
  }
  if (next_word_is(&args, "monitor")) {
    cmd_midi_monitor(args);
    return;
  }
  if (next_word_is(&args, "auto")) {
    cmd_midi_auto(args);
    return;
  }
  if (next_word_is(&args, "stream")) {
    cmd_midi_stream(args);
    return;
  }
  if (next_word_is(&args, "input")) {
    cmd_midi_input(args);
    return;
  }
  if (next_word_is(&args, "output")) {
    cmd_midi_output(args);
    return;
  }
  console_puts("Usage: midi [decode | rx | monitor | auto | stream | input | output]\n");
}

// Commands will be added here as they are implemented.
// The NULL entry terminates the table.
static const console_command_t commands[] = {
  { "uptime", cmd_uptime, "Show time since OrgOS started" },
  { "help",   cmd_help,   "List available commands" },
  { "memory", cmd_memory, "Inspect firmware mailbox and ARM memory" },
  { "irq", cmd_irq, "Display interrupt statistics" },
  { "fdt", cmd_fdt, "Inspect firmware Device Tree, memory and DMA" },
  { "physmem", cmd_physmem, "Display physical RAM and reservations" },
  { "cache", cmd_cache, "Display CPU cache configuration" },
  { "pci", cmd_pci, "Inspect PCIe controller and DMA configuration" },
  { "page", cmd_page, "Inspect physical page statistics and run tests" },
  { "dma", cmd_dma, "Run DMA ownership and cache tests" },
  { "usb", cmd_usb, "Inspect and test USB controller and MIDI transport" },
  { "midi", cmd_midi, "Decode, receive and monitor MIDI" },
  { NULL, NULL, NULL }
};

static void cmd_help(const char *args) {
  if (!end_of_args(args)) {
    console_puts("Usage: help\n");
    return;
  }

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
  return c == ' '  || c == '\t' || c == '\n' ||
         c == '\r' || c == '\v' || c == '\f';
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

    if (console_line_overflowed) {
      console_puts("Input line too long; command discarded.\n");
    } else {
      console_line[console_length] = '\0';
      console_dispatch(console_line);
    }

    console_length = 0U;
    console_line_overflowed = false;
    console_prompt();
    return true;
  }

  // Once overflow has occurred, discard everything
  // until the end of the line. In particular, do not
  // allow backspace to make a truncated line executable.
  if (console_line_overflowed)
    return true;

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
    console_line_overflowed = true;
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

