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

void cmd_pci(const char *args) {
  if (console_arg_word(&args, "dma")) {
    if (console_arg_word(&args, "test") && console_arg_end(args)) {
      cmd_pci_dma_test();
      return;
    }
    if (console_arg_end(args)) {
      cmd_pci_dma();
      return;
    }
    console_puts("Usage: pci [dma [test]]\n");
    return;
  }
  if (!console_arg_end(args)) {
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

