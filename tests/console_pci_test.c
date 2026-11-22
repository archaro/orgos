#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Exercise the actual dispatcher and diagnostic handlers, with no PCIe/GIC
// hardware. Unexpected operations fail instead of silently simulating writes.
#include "../src/console_pci.c"

static char output[8192];
static size_t output_length;
static bool mode_enabled;
static unsigned int host_reads, notification_reads, gic_reads, prepare_calls, mode_writes;

void console_puts(const char *text) {
  size_t length = strlen(text);
  assert(output_length + length < sizeof(output));
  memcpy(output + output_length, text, length + 1U);
  output_length += length;
}

void console_printf(const char *format, ...) {
  va_list args;
  va_start(args, format);
  int length = vsnprintf(output + output_length,
      sizeof(output) - output_length, format, args);
  va_end(args);
  assert(length >= 0 && (size_t)length < sizeof(output) - output_length);
  output_length += (size_t)length;
}

void console_putc(char c) { char text[] = {c, '\0'}; console_puts(text); }
void console_putu32(uint32_t value) { console_printf("%u", value); }
void console_putu64(uint64_t value) { console_printf("%llu", (unsigned long long)value); }
void console_puthex(uint32_t value, unsigned int digits) {
  console_printf("%0*X", (int)digits, value);
}
void console_puthex64(uint64_t value) {
  console_printf("%016llX", (unsigned long long)value);
}

bool usb_msi_mode_get_info(usb_msi_mode_info_t *info) {
  *info = (usb_msi_mode_info_t){.enabled = mode_enabled};
  return true;
}
bool usb_msi_mode_set(bool enabled) { mode_writes++; mode_enabled = enabled; return true; }
bool usb_service_available(void) { return true; }
bool usb_msi_notification_get_info(usb_msi_notification_info_t *info) {
  notification_reads++;
  *info = (usb_msi_notification_info_t){.irq_sequence = 9U,
    .last_observed = 9U, .notifications = 9U, .service_passes = 100U};
  return true;
}
bool pcie_msi_host_probe(pcie_msi_host_info_t *info) {
  host_reads++;
  *info = (pcie_msi_host_info_t){.interrupt_status = 1U};
  return true;
}
bool gic_get_pcie_msi_info(gic_pcie_msi_info_t *info) {
  gic_reads++;
  *info = (gic_pcie_msi_info_t){.enabled = mode_enabled};
  return true;
}
uint32_t irq_pcie_msi_count(void) { return 11U; }
uint32_t irq_pcie_msi_hardware_count(void) { return 9U; }
uint32_t irq_pcie_msi_last_status(void) { return 1U; }
pcie_msi_prepare_result_t pcie_msi_host_prepare(void) {
  assert(!mode_enabled);
  prepare_calls++;
  return PCIE_MSI_PREPARE_NOT_READY;
}

#define UNUSED __attribute__((unused))
#define UNEXPECTED(type, name, args, value) \
  type name args { assert(!"Unexpected hardware operation"); return value; }
UNEXPECTED(void, pcie_probe, (pcie_info_t *info UNUSED), )
UNEXPECTED(bool, pcie_dma_probe, (pcie_dma_info_t *info UNUSED), false)
UNEXPECTED(bool, pcie_phys_to_dma, (uint64_t physical UNUSED, uint64_t length UNUSED,
    uint64_t *address UNUSED), false)
UNEXPECTED(uint32_t, gic_interrupt_capacity, (void), 0U)
UNEXPECTED(bool, gic_prepare_pcie_msi, (void), false)
UNEXPECTED(void, gic_enable_irq, (uint32_t id UNUSED), )
UNEXPECTED(bool, gic_pcie_msi_test_trigger, (void), false)
UNEXPECTED(void, gic_pcie_msi_test_finish, (void), )
UNEXPECTED(bool, pcie_xhci_get_interrupt_info, (pcie_xhci_interrupt_info_t *info UNUSED), false)
UNEXPECTED(bool, pcie_xhci_msi_program, (void), false)
UNEXPECTED(bool, pcie_xhci_msi_set_enabled, (bool enabled UNUSED), false)
UNEXPECTED(bool, pcie_xhci_msi_get_message, (pcie_xhci_msi_message_t *info UNUSED), false)
UNEXPECTED(bool, pcie_msi_host_unmask_vector0, (void), false)
UNEXPECTED(uint32_t, pcie_msi_host_irq_quiesce, (void), 0U)
UNEXPECTED(bool, xhci_msi_test_set_enabled, (bool enabled UNUSED), false)
UNEXPECTED(xhci_noop_result_t, xhci_noop, (xhci_noop_info_t *info UNUSED), XHCI_NOOP_NOT_RUNNING)
UNEXPECTED(uint64_t, timer_now_us, (void), 0U)
UNEXPECTED(void, timer_delay_ms, (uint32_t milliseconds UNUSED), )
#undef UNEXPECTED

bool usb_event_dispatch_get_stats(usb_event_dispatch_stats_t *info UNUSED) { return false; }
bool xhci_event_latency_get_stats(xhci_event_latency_stats_t *info UNUSED) { return false; }
#undef UNUSED

static void fixture(bool enabled) {
  mode_enabled = enabled;
  host_reads = notification_reads = gic_reads = prepare_calls = mode_writes = 0U;
  output_length = 0U;
  output[0] = '\0';
}

static void test_read_only_commands(void) {
  const char *commands[] = {"msi irq", "  msi\tirq  "};
  for (unsigned int enabled = 0U; enabled < 2U; enabled++) {
    for (unsigned int i = 0U; i < sizeof(commands) / sizeof(commands[0]); i++) {
      fixture(enabled != 0U);
      cmd_pci(commands[i]);
      assert(strstr(output, "PCIe MSI interrupt statistics:\n"));
      assert(strstr(output, "  Total IRQs     : 11\n  Hardware IRQs  : 9\n"));
      assert(!strstr(output, "BCM2711 PCIe MSI receiver:"));
      assert(notification_reads == 1U && gic_reads == 1U && host_reads == 1U);
      assert(prepare_calls == 0U && mode_writes == 0U && mode_enabled == (enabled != 0U));
    }
    fixture(enabled != 0U);
    cmd_pci("msi");
    assert(strstr(output, "BCM2711 PCIe MSI receiver:\n"));
    assert(host_reads == 1U && notification_reads == 0U && gic_reads == 0U);
    assert(prepare_calls == 0U && mode_writes == 0U);
  }
}

static void test_guard_and_syntax(void) {
  const char *blocked[] = {"msi prepare", "msi route", "msi program", "msi test",
    "msi hardware-test", "msi repeat-test", "msi unknown", "msi irqx",
    "msi irq extra", "msi irq prepare", "msi irq route", "msi irq program",
    "msi irq test", "msi irq hardware-test", "msi irq repeat-test"};
  for (unsigned int i = 0U; i < sizeof(blocked) / sizeof(blocked[0]); i++) {
    fixture(true);
    cmd_pci(blocked[i]);
    assert(strcmp(output, "Disable continuous MSI mode first.\n") == 0);
    assert(host_reads == 0U && notification_reads == 0U && gic_reads == 0U);
    assert(prepare_calls == 0U && mode_writes == 0U && mode_enabled);
  }
  fixture(false);
  cmd_pci("msi irq extra");
  assert(strncmp(output, "Usage: pci msi ", 15U) == 0);
  assert(host_reads == 0U && notification_reads == 0U && gic_reads == 0U);
  assert(prepare_calls == 0U && mode_writes == 0U);
  fixture(false);
  cmd_pci("msi prepare");
  assert(prepare_calls == 1U && strcmp(output, "PCIe or DMA subsystem not ready.\n") == 0);
  fixture(true);
  cmd_pci("msi mode off");
  assert(mode_writes == 1U && !mode_enabled);
  assert(strcmp(output, "Continuous MSI disabled.\n") == 0);
}

int main(void) {
  test_read_only_commands();
  test_guard_and_syntax();
  puts("Console PCI MSI dispatch: PASS");
  return 0;
}
