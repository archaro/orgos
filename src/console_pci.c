#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "console.h"
#include "console_args.h"
#include "console_commands.h"
#include "timer.h"
#include "gic.h"
#include "irq.h"
#include "pcie.h"

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

static void cmd_pci_interrupts(void) {
  pcie_xhci_interrupt_info_t info = {0};
  if (!pcie_xhci_get_interrupt_info(&info)) {
    console_puts("PCIe interrupt inspection failed.\n");
    return;
  }
  console_puts("VL805 PCI interrupt capabilities:\n");
  console_printf("  Interrupt line : 0x%02x\n",
      (unsigned int)info.interrupt_line);
  console_printf("  Interrupt pin  : %u\n", (unsigned int)info.interrupt_pin);
  console_printf("  Capability list: %s\n",
      info.capabilities_present ? "Present" : "Absent");
  console_printf("  Capabilities   : %u\n", info.capabilities_scanned);
  console_puts("\nMSI:\n");
  console_printf("  Supported      : %s\n", info.msi_present ? "Yes" : "No");
  if (info.msi_present) {
    console_printf("  Offset         : 0x%02x\n",
        (unsigned int)info.msi_offset);
    console_printf("  Control        : 0x%04x\n",
        (unsigned int)info.msi_control);
    console_printf("  Enabled        : %s\n",
        info.msi_enabled ? "Yes" : "No");
    console_printf("  64-bit address : %s\n", info.msi_64bit ? "Yes" : "No");
  }
  console_puts("\nMSI-X:\n");
  console_printf("  Supported      : %s\n",
      info.msix_present ? "Yes" : "No");
  if (info.msix_present) {
    console_printf("  Offset         : 0x%02x\n",
        (unsigned int)info.msix_offset);
    console_printf("  Control        : 0x%04x\n",
        (unsigned int)info.msix_control);
    console_printf("  Enabled        : %s\n",
        info.msix_enabled ? "Yes" : "No");
    console_printf("  Function masked: %s\n",
        info.msix_masked ? "Yes" : "No");
  }
  console_puts("\nNo interrupt configuration was changed.\n");
}

static void cmd_pci_msi_host(void) {
  pcie_msi_host_info_t info = {0};
  if (!pcie_msi_host_probe(&info)) {
    console_puts("PCIe MSI host registers unavailable.\n");
    return;
  }
  console_puts("BCM2711 PCIe MSI receiver:\n");
  console_printf("  Revision       : 0x%08x\n", info.revision);
  console_printf("  Target low     : 0x%08x\n", info.target_lo);
  console_printf("  Target high    : 0x%08x\n", info.target_hi);
  console_printf("  Data config    : 0x%08x\n", info.data_config);
  console_printf("  Pending status : 0x%08x\n", info.interrupt_status);
  console_printf("  Receiver enable: %s\n",
      (info.target_lo & 1U) ? "Yes" : "No");
}

static void cmd_pci_msi_prepare(void) {
  pcie_msi_prepare_result_t result = pcie_msi_host_prepare();
  switch (result) {
    case PCIE_MSI_PREPARE_OK:
      console_puts("PCIe MSI receiver prepared.\n");
      break;
    case PCIE_MSI_PREPARE_NOT_READY:
      console_puts("PCIe or DMA subsystem not ready.\n");
      return;
    case PCIE_MSI_PREPARE_UNSUPPORTED_REVISION:
      console_puts("Unsupported PCIe controller revision.\n");
      return;
    case PCIE_MSI_PREPARE_ENDPOINT_STATE:
      console_puts("VL805 MSI capability not in a safe state.\n");
      return;
    case PCIE_MSI_PREPARE_VERIFY_FAILED:
      console_puts("MSI receiver verification failed.\n");
      return;
  }
  console_puts("All MSI vectors requested masked.\n");
  console_puts("VL805 MSI remains disabled.\n\n");
  cmd_pci_msi_host();
}

static void cmd_pci_msi_route(void) {
  uint32_t capacity = gic_interrupt_capacity();
  console_puts("BCM2711 PCIe MSI GIC route:\n");
  console_printf("  Interrupt ID : %u\n", GIC_IRQ_PCIE_MSI);
  console_printf("  GIC capacity : %u\n", capacity);
  if (GIC_IRQ_PCIE_MSI >= capacity) {
    console_puts("  Result       : Interrupt ID unavailable.\n");
    return;
  }
  bool prepared = gic_prepare_pcie_msi();
  gic_pcie_msi_info_t info = {0};
  if (!gic_get_pcie_msi_info(&info)) {
    console_puts("  Result       : GIC inspection failed.\n");
    return;
  }
  console_printf("  Enabled      : %s\n", info.enabled ? "Yes" : "No");
  console_printf("  Pending      : %s\n", info.pending ? "Yes" : "No");
  console_printf("  Trigger      : %s\n",
      info.edge_triggered ? "Edge" : "Level");
  console_printf("  Priority     : 0x%02x\n", (unsigned int)info.priority);
  console_printf("  CPU targets  : 0x%02x\n", (unsigned int)info.targets);
  console_printf("  Verification : %s\n", prepared ? "PASS" : "FAILED");
  console_puts("\nGIC interrupt delivery remains disabled.\n");
}

static void cmd_pci_msi_test(void) {
  pcie_xhci_interrupt_info_t endpoint = {0};
  if (!pcie_xhci_get_interrupt_info(&endpoint) ||
      endpoint.msi_enabled || endpoint.msix_enabled) {
    console_puts("VL805 interrupt state is unsafe for test.\n");
    return;
  }
  pcie_msi_host_info_t host = {0};
  if (!pcie_msi_host_probe(&host) || !(host.target_lo & 1U) ||
      host.interrupt_status != 0U) {
    console_puts("PCIe MSI receiver is not ready for test.\n");
    return;
  }
  // Preparation leaves interrupt delivery disabled.
  if (!gic_prepare_pcie_msi()) {
    console_puts("GIC MSI route preparation failed.\n");
    return;
  }
  uint32_t before = irq_pcie_msi_count();
  console_puts("Testing software delivery of GIC IRQ 180...\n");
  if (!gic_pcie_msi_test_trigger()) {
    console_puts("Unable to trigger test interrupt.\n");
    return;
  }
  uint64_t started = timer_now_us();
  while (irq_pcie_msi_count() == before &&
      timer_now_us() - started < 100000ULL) {
    timer_delay_ms(1U);
  }
  // Ensure the test leaves interrupt 180 disabled,
  // whether or not the processor received it.
  gic_pcie_msi_test_finish();
  uint32_t after = irq_pcie_msi_count();
  gic_pcie_msi_info_t info = {0};
  bool inspected = gic_get_pcie_msi_info(&info);
  console_printf("  IRQ count before : %u\n", before);
  console_printf("  IRQ count after  : %u\n", after);
  if (inspected) {
    console_printf("  GIC enabled      : %s\n", info.enabled ? "Yes" : "No");
    console_printf("  GIC pending      : %s\n", info.pending ? "Yes" : "No");
  }
  bool passed = after == before + 1U &&
      inspected && !info.enabled && !info.pending;
  console_printf("  Result           : %s\n", passed ? "PASS" : "FAILED");
}

static void cmd_pci_msi_irq(void) {
  console_puts("PCIe MSI interrupt statistics:\n");
  console_printf("  Total IRQs     : %u\n", irq_pcie_msi_count());
  console_printf("  Hardware IRQs  : %u\n", irq_pcie_msi_hardware_count());
  console_printf("  Last MSI status: 0x%08x\n", irq_pcie_msi_last_status());
  gic_pcie_msi_info_t gic = {0};
  if (gic_get_pcie_msi_info(&gic)) {
    console_printf("  GIC enabled    : %s\n", gic.enabled ? "Yes" : "No");
    console_printf("  GIC pending    : %s\n", gic.pending ? "Yes" : "No");
  }
  pcie_msi_host_info_t host = {0};
  if (pcie_msi_host_probe(&host)) {
    console_printf("  Host pending   : 0x%08x\n", host.interrupt_status);
  }
}

static void cmd_pci_msi_program(void) {
  if (!pcie_xhci_msi_program()) {
    console_puts("VL805 MSI message configuration failed.\n");
    return;
  }
  pcie_xhci_msi_message_t message = {0};
  if (!pcie_xhci_msi_get_message(&message)) {
    console_puts("VL805 MSI message readback failed.\n");
    return;
  }
  console_puts("VL805 MSI message configured:\n");
  console_printf("  Address low : 0x%08x\n", message.address_lo);
  console_printf("  Address high: 0x%08x\n", message.address_hi);
  console_printf("  Data        : 0x%04x\n", (unsigned int)message.data);
  console_printf("  Control     : 0x%04x\n", (unsigned int)message.control);
  console_printf("  MSI enabled : %s\n",
      (message.control & 1U) ? "Yes" : "No");
  console_puts("\nMSI message verified; delivery remains disabled.\n");
}

void cmd_pci(const char *args) {
  if (console_arg_word(&args, "msi")) {
    if (console_arg_end(args)) {
      cmd_pci_msi_host();
      return;
    }
    if (console_arg_word(&args, "prepare") &&
        console_arg_end(args)) {
      cmd_pci_msi_prepare();
      return;
    }
    if (console_arg_word(&args, "route") &&
        console_arg_end(args)) {
      cmd_pci_msi_route();
      return;
    }
    if (console_arg_word(&args, "irq") &&
        console_arg_end(args)) {
      cmd_pci_msi_irq();
      return;
    }
    if (console_arg_word(&args, "program") &&
        console_arg_end(args)) {
      cmd_pci_msi_program();
      return;
    }
    if (console_arg_word(&args, "test") &&
        console_arg_end(args)) {
      cmd_pci_msi_test();
      return;
    }
    console_puts("Usage: pci msi [prepare | route | irq | program | test]\n");
    return;
  }
  if (console_arg_word(&args, "interrupts")) {
    if (console_arg_end(args)) {
      cmd_pci_interrupts();
      return;
    }
    console_puts("Usage: pci interrupts\n");
    return;
  }
  if (console_arg_word(&args, "dma")) {
    if (console_arg_word(&args, "test") && console_arg_end(args)) {
      cmd_pci_dma_test();
      return;
    }
    if (console_arg_end(args)) {
      cmd_pci_dma();
      return;
    }
    console_puts("Usage: pci [dma [test] | interrupts | msi]\n");
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

