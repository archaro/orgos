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
#include "xhci.h"
#include "usb.h"

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
  usb_msi_notification_info_t notification = {0};
  if (usb_msi_notification_get_info(&notification)) {
    console_puts("\nForeground USB notification tracking:\n");
    console_printf("  IRQ sequence   : %u\n", notification.irq_sequence);
    console_printf("  Last observed  : %u\n", notification.last_observed);
    console_printf("  Notifications  : %u\n", notification.notifications);
    console_printf("  Service passes : %u\n", notification.service_passes);
  }
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

static void cmd_pci_msi_hardware_test(void) {
  uint32_t irq_before = irq_pcie_msi_count();
  uint32_t hw_before = irq_pcie_msi_hardware_count();
  bool armed = false;
  console_puts("Testing VL805 hardware MSI delivery...\n");
  // Start from known-disabled GIC and receiver states.
  if (pcie_msi_host_prepare() != PCIE_MSI_PREPARE_OK ||
      !gic_prepare_pcie_msi() || !pcie_xhci_msi_program()) {
    console_puts("MSI preparation failed.\n");
    return;
  }
  // Enable the endpoint while the BCM2711 vector
  // and GIC route remain disabled.
  if (!pcie_xhci_msi_set_enabled(true)) {
    console_puts("Unable to enable VL805 MSI.\n");
    goto cleanup;
  }
  if (!xhci_msi_test_set_enabled(true)) {
    console_puts("Unable to enable xHCI interrupts.\n");
    goto cleanup;
  }
  if (!pcie_msi_host_unmask_vector0()) {
    console_puts("Unable to unmask MSI vector zero.\n");
    goto cleanup;
  }
  // Enable GIC delivery last. The handler will
  // disable it again after the first notification.
  gic_enable_irq(GIC_IRQ_PCIE_MSI);
  armed = true;
  xhci_noop_info_t noop = {0};
  xhci_noop_result_t result = xhci_noop(&noop);
  console_printf("  No Op result    : %u\n", (unsigned int)result);
  console_printf("  Completion code : %u\n",
      (unsigned int)noop.completion_code);
  // Allow for xHCI interrupt moderation. A command
  // completion may be observed before its MSI arrives.
  uint64_t started = timer_now_us();
  while (irq_pcie_msi_hardware_count() == hw_before &&
      timer_now_us() - started < 100000ULL) {
    timer_delay_ms(1U);
  }
cleanup:
  // Disable the device's interrupt generation
  // before dismantling the routing configuration.
  bool xhci_off = xhci_msi_test_set_enabled(false);
  bool pci_off = pcie_xhci_msi_set_enabled(false);
  // The IRQ handler may already have disabled the
  // GIC route and masked the receiver.
  gic_pcie_msi_test_finish();
  pcie_msi_host_irq_quiesce();
  uint32_t irq_after = irq_pcie_msi_count();
  uint32_t hw_after = irq_pcie_msi_hardware_count();
  pcie_msi_host_info_t host = {0};
  gic_pcie_msi_info_t gic = {0};
  pcie_xhci_interrupt_info_t endpoint = {0};
  bool inspected = pcie_msi_host_probe(&host) &&
      gic_get_pcie_msi_info(&gic) && pcie_xhci_get_interrupt_info(&endpoint);
  console_printf("  IRQ count       : %u -> %u\n", irq_before, irq_after);
  console_printf("  Hardware IRQs   : %u -> %u\n", hw_before, hw_after);
  console_printf("  Last MSI status : 0x%08x\n", irq_pcie_msi_last_status());
  if (inspected) {
    console_printf("  Host pending    : 0x%08x\n", host.interrupt_status);
    console_printf("  GIC enabled     : %s\n", gic.enabled ? "Yes" : "No");
    console_printf("  VL805 MSI       : %s\n",
        endpoint.msi_enabled ? "Enabled" : "Disabled");
  }
  bool delivered = armed && irq_after == irq_before + 1U &&
      hw_after == hw_before + 1U && irq_pcie_msi_last_status() == 1U;
  bool disarmed = xhci_off && pci_off && inspected && !gic.enabled &&
      !endpoint.msi_enabled && host.interrupt_status == 0U;
  console_printf("  MSI delivery    : %s\n", delivered ? "PASS" : "FAILED");
  console_printf("  Disarmed        : %s\n", disarmed ? "Yes" : "NO");
  if (!disarmed)
    console_puts("WARNING: Interrupt disarming failed.\n");
}

static void cmd_pci_msi_repeat_test(void) {
  const unsigned int cycles = 2U;
  uint32_t start = irq_pcie_msi_hardware_count();
  unsigned int completed = 0U;
  bool passed = true;
  console_puts("Testing repeated VL805 MSI delivery...\n");
  // Start with both the MSI receiver and GIC route
  // configured but disabled.
  if (pcie_msi_host_prepare() != PCIE_MSI_PREPARE_OK ||
      !gic_prepare_pcie_msi() || !pcie_xhci_msi_program()) {
    console_puts("MSI preparation failed.\n");
    return;
  }
  // These two enables remain set across both No Ops.
  if (!pcie_xhci_msi_set_enabled(true)) {
    console_puts("VL805 MSI enable failed.\n");
    passed = false;
    goto cleanup;
  }
  if (!xhci_msi_test_set_enabled(true)) {
    console_puts("xHCI interrupt enable failed.\n");
    passed = false;
    goto cleanup;
  }
  for (unsigned int i = 0U; i < cycles; i++) {
    uint32_t before = irq_pcie_msi_hardware_count();
    // The previous IRQ must have been fully handled
    // before another MSI can be admitted.
    gic_pcie_msi_info_t gic = {0};
    if (!gic_get_pcie_msi_info(&gic) || gic.enabled || gic.pending) {
      console_puts("GIC not quiescent before rearm.\n");
      passed = false;
      break;
    }
    // The ISR has already masked all receiver vectors.
    // Reopen only vector zero, then GIC delivery.
    if (!pcie_msi_host_unmask_vector0()) {
      console_puts("MSI vector rearm failed.\n");
      passed = false;
      break;
    }
    gic_enable_irq(GIC_IRQ_PCIE_MSI);
    // Detect an unexpected notification that arrives
    // before we submit the next command.
    if (irq_pcie_msi_hardware_count() != before) {
      console_puts("Unexpected MSI during rearm.\n");
      passed = false;
      break;
    }
    xhci_noop_info_t noop = {0};
    xhci_noop_result_t result = xhci_noop(&noop);
    // A completion can precede its moderated MSI.
    uint64_t started = timer_now_us();
    while (irq_pcie_msi_hardware_count() == before &&
        timer_now_us() - started < 100000ULL) {
      timer_delay_ms(1U);
    }
    uint32_t after = irq_pcie_msi_hardware_count();
    bool correct = result == XHCI_NOOP_OK &&
        noop.completion_code == XHCI_COMPLETION_SUCCESS &&
        after == before + 1U && irq_pcie_msi_last_status() == 1U;
    console_printf("  Cycle %u:\n", i + 1U);
    console_printf("    No Op result : %u\n", (unsigned int)result);
    console_printf("    Completion   : %u\n",
        (unsigned int)noop.completion_code);
    console_printf("    Hardware IRQ : %u -> %u\n", before, after);
    console_printf("    MSI status   : 0x%08x\n", irq_pcie_msi_last_status());
    console_printf("    Result       : %s\n", correct ? "PASS" : "FAILED");
    if (!correct) {
      passed = false;
      break;
    }
    completed++;
  }
cleanup:
  // Stop interrupt generation before closing the
  // receiver and GIC routes.
  bool xhci_off = xhci_msi_test_set_enabled(false);
  bool pci_off = pcie_xhci_msi_set_enabled(false);
  gic_pcie_msi_test_finish();
  pcie_msi_host_irq_quiesce();
  pcie_msi_host_info_t host = {0};
  gic_pcie_msi_info_t gic = {0};
  pcie_xhci_interrupt_info_t endpoint = {0};
  bool inspected = pcie_msi_host_probe(&host) &&
      gic_get_pcie_msi_info(&gic) && pcie_xhci_get_interrupt_info(&endpoint);
  bool disarmed = xhci_off && pci_off && inspected && !gic.enabled &&
      !gic.pending && !endpoint.msi_enabled && host.interrupt_status == 0U;
  uint32_t finish = irq_pcie_msi_hardware_count();
  console_printf("\n  Completed      : %u of %u\n", completed, cycles);
  console_printf("  Total hardware : %u -> %u\n", start, finish);
  console_printf("  Disarmed       : %s\n", disarmed ? "Yes" : "NO");
  bool success = passed && completed == cycles &&
      finish == start + cycles && disarmed;
  console_printf("  Overall result : %s\n", success ? "PASS" : "FAILED");
  if (!disarmed)
    console_puts("WARNING: Interrupt disarming failed.\n");
}

static void cmd_pci_msi_mode_status(void) {
  usb_msi_mode_info_t info = {0};
  if (!usb_msi_mode_get_info(&info)) {
    console_puts("MSI mode information unavailable.\n");
    return;
  }
  console_puts("Experimental continuous MSI mode:\n");
  console_printf("  Enabled       : %s\n", info.enabled ? "Yes" : "No");
  console_printf("  Faulted       : %s\n", info.faulted ? "Yes" : "No");
  console_printf("  Disarm failed: %s\n", info.disarm_failed ? "Yes" : "No");
  console_printf("  USB available: %s\n",
      usb_service_available() ? "Yes" : "No");
  console_printf("  Last rearmed  : %u\n", info.last_rearmed);
  console_printf("  Rearms        : %u\n", info.rearms);
  console_printf("  Failures      : %u\n", info.failures);
  console_printf("  Hardware IRQs : %u\n", irq_pcie_msi_hardware_count());
  console_printf("  Fault stage   : %u\n", info.fault_stage);
  console_printf("  Fault pending : 0x%08x\n", info.fault_host_pending);
  console_printf("  Pending defers : %u\n", info.pending_deferrals);
  console_printf("  Other vectors  : %u\n", info.unexpected_pending);
  console_puts("\nEvent dispatch scheduling:\n");
  console_printf("  IRQ dispatches     : %u\n", info.irq_dispatches);
  console_printf("  Catchup dispatches : %u\n", info.catchup_dispatches);
  console_printf("  Fallback dispatches: %u\n", info.fallback_dispatches);
  console_printf("  Skipped dispatches : %u\n", info.skipped_dispatches);
  usb_event_dispatch_stats_t dispatch = {0};
  if (usb_event_dispatch_get_stats(&dispatch)) {
    console_puts("\nxHCI event dispatch timing:\n");
    console_printf("  Calls        : %u\n", dispatch.calls);
    console_printf("  Idle         : %u\n", dispatch.idle);
    console_printf("  Progress     : %u\n", dispatch.progress);
    console_printf("  Errors       : %u\n", dispatch.errors);
    console_printf("  Zero-us calls: %u\n", dispatch.zero_us);
    console_puts("  Total time   : "); console_putu64(dispatch.total_us);
    console_puts(" us\n");
    console_puts("  Maximum      : ");
    console_putu64(dispatch.max_us);
    console_puts(" us\n");
    if (dispatch.calls != 0U) {
      uint64_t average = dispatch.total_us / dispatch.calls;
      console_puts("  Mean         : ");
      console_putu64(average);
      console_puts(" us\n");
    }
  }
  console_puts("\nMSI response timing:\n");
  console_printf("  Samples          : %u\n", info.response_samples);
  console_puts("  Total latency    : ");
  console_putu64(info.response_total_us);
  console_puts(" us\n");
  console_puts("  Maximum latency  : ");
  console_putu64(info.response_max_us);
  console_puts(" us\n");
  if (info.response_samples != 0U) {
    console_puts("  Mean latency     : ");
    console_putu64(info.response_total_us / info.response_samples);
    console_puts(" us\n");
  }
  console_puts("\nFallback effectiveness:\n");
  console_printf("  Found work       : %u\n", info.fallback_progress);
  console_printf("  Ring idle        : %u\n", info.fallback_idle);
  xhci_event_latency_stats_t events = {0};
  if (xhci_event_latency_get_stats(&events)) {
    console_puts("\nxHCI event consumption:\n");
    console_printf("  Foreground events : %u\n", events.foreground_events);
    console_printf("  Synchronous events: %u\n", events.synchronous_events);
    console_printf("  Port-wait events  : %u\n", events.port_wait_events);
    console_puts("\nEvent/MSI correlation:\n");
    console_printf("  Matched samples   : %u\n", events.correlated_events);
    console_printf("  Other events      : %u\n", events.uncorrelated_events);
    console_puts("  Maximum delay     : ");
    console_putu64(events.latency_max_us);
    console_puts(" us\n");
    if (events.correlated_events != 0U) {
      console_puts("  Mean delay        : ");
      console_putu64(events.latency_total_us / events.correlated_events);
      console_puts(" us\n");
    }
    console_printf("  Foreground samples: %u\n", events.foreground_samples);
    console_printf("  Synchronous samples: %u\n", events.synchronous_samples);
    console_printf("  Port-wait samples : %u\n", events.port_wait_samples);
    if (events.foreground_samples != 0U) {
      console_puts("  Foreground mean   : ");
      console_putu64(events.foreground_latency_us /
          events.foreground_samples);
      console_puts(" us\n");
    }
    if (events.synchronous_samples != 0U) {
      console_puts("  Synchronous mean  : ");
      console_putu64(events.synchronous_latency_us / events.synchronous_samples);
      console_puts(" us\n");
    }
  }
  console_puts("\nxHCI state at MSI rearm:\n");
  console_printf("  IMAN pending : %u\n", info.rearm_iman_pending);
  console_printf("  EHB busy     : %u\n", info.rearm_ehb_busy);
  console_printf("  Both clear   : %u\n", info.rearm_both_clear);
  console_printf("  Read failures: %u\n", info.rearm_state_failures);
  console_printf("  Last IMAN    : 0x%08x\n", info.last_rearm_iman);
  console_puts("  Last ERDP    : 0x");
  console_puthex64(info.last_rearm_erdp);
  console_putc('\n');
  console_puts("\nPost-rearm event inspection:\n");
  console_printf("  Inspections : %u\n", info.post_rearm_checks);
  console_printf("  Ring idle   : %u\n", info.post_rearm_idle);
  console_printf("  Found work  : %u\n", info.post_rearm_progress);
  console_printf("  Failures    : %u\n", info.post_rearm_failures);
  console_printf("  IRQ changes : %u\n", info.post_rearm_irq_changes);
}

void cmd_pci(const char *args) {
  if (console_arg_word(&args, "msi")) {
    if (console_arg_word(&args, "mode")) {
      if (console_arg_end(args)) {
        cmd_pci_msi_mode_status();
        return;
      }
      if (console_arg_word(&args, "on") && console_arg_end(args)) {
        bool ok = usb_msi_mode_set(true);
        console_puts(ok ? "Continuous MSI enabled.\n" :
            "Unable to enable continuous MSI.\n");
        return;
      }
      if (console_arg_word(&args, "off") && console_arg_end(args)) {
        bool ok = usb_msi_mode_set(false);
        console_puts(ok ? "Continuous MSI disabled.\n" :
            "MSI disable verification failed.\n");
        return;
      }
      console_puts("Usage: pci msi mode [on | off]\n");
      return;
    }
    // Diagnostics which alter interrupt configuration
    // must not run during continuous MSI operation.
    usb_msi_mode_info_t mode = {0};
    usb_msi_mode_get_info(&mode);
    if (mode.enabled && !console_arg_end(args) &&
        !console_arg_word(&args, "irq")) {
      console_puts("Disable continuous MSI mode first.\n");
      return;
    }
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
    if (console_arg_word(&args, "hardware-test") &&
        console_arg_end(args)) {
      cmd_pci_msi_hardware_test();
      return;
    }
    if (console_arg_word(&args, "repeat-test") &&
        console_arg_end(args)) {
      cmd_pci_msi_repeat_test();
      return;
    }
    console_puts("Usage: pci msi [prepare | route | irq | program | test | "
        "hardware-test | repeat-test]\n");
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

