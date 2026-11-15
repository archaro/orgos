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

#include "console_usb_internal.h"

void cmd_usb(const char *args) {
  // The VL805 is configured at boot. Subcommands inspect
  // controller state and exercise explicit diagnostics.
  const char *other_args = args;
  if (console_arg_word(&args, "topology")) {
    if (console_arg_end(args)) {
      cmd_usb_topology(false);
      return;
    }
    if (console_arg_word(&args, "detail") && console_arg_end(args)) {
      cmd_usb_topology(true);
      return;
    }
    console_puts("Usage: usb topology [detail]\n");
    return;
  }
  if (console_arg_word(&args, "hubirq")) {
    if (console_arg_end(args)) {
      cmd_usb_hubirq();
      return;
    }
    console_puts("Usage: usb hubirq\n");
    return;
  }
  if (console_arg_word(&args, "auto")) {
    cmd_usb_auto(args);
    return;
  }
  if (console_arg_word(&args, "recover")) {
    if (console_arg_word(&args, "status")) {
      cmd_usb_recover_status(args);
      return;
    }
    cmd_usb_recover(args);
    return;
  }
  if (console_arg_word(&args, "lifecycle")) {
    if (console_arg_word(&args, "status")) {
      cmd_usb_lifecycle_status(args);
      return;
    }
    if (console_arg_word(&args, "port")) {
      cmd_usb_lifecycle_port(args);
      return;
    }
    console_puts("Usage: usb lifecycle "
        "[status <generation> | port <generation>]\n");
    return;
  }
  if (console_arg_word(&args, "resources") && console_arg_end(args)) {
    cmd_usb_resources();
    return;
  }
  if (console_arg_word(&args, "transfers")) {
    cmd_usb_transfers(args);
    return;
  }
  if (console_arg_word(&args, "changes")) {
    if (console_arg_end(args)) {
      cmd_usb_changes();
      return;
    }
    if (console_arg_word(&args, "status") && console_arg_end(args)) {
      cmd_usb_changes();
      return;
    }
    if (console_arg_word(&args, "dump") && console_arg_end(args)) {
      cmd_usb_changes_dump();
      return;
    }
    console_puts("Usage: usb changes [status | dump]\n");
    return;
  }
  if (console_arg_word(&args, "devices") && console_arg_end(args)) {
    cmd_usb_devices();
    return;
  }
  if (console_arg_word(&args, "enumerate")) {
    cmd_usb_enumerate(args);
    return;
  }
  if (console_arg_word(&args, "enumstate")) {
    cmd_usb_enumstate(args);
    return;
  }
  if (console_arg_word(&args, "hubports") && console_arg_end(args)) {
    cmd_usb_hubports();
    return;
  }
  if (console_arg_word(&args, "events") && console_arg_end(args)) {
    cmd_usb_events();
    return;
  }
  if (console_arg_word(&other_args, "test")) {
    if (console_arg_word(&other_args, "ep0match") &&
        console_arg_end(other_args)) {
      cmd_usb_test_ep0match();
      return;
    }
    if (console_arg_word(&other_args, "ep0late") &&
        console_arg_end(other_args)) {
      console_printf("EP0 late completion: %s\n",
          xhci_ep0_late_selftest() ? "PASS" : "FAILED");
      return;
    }
    if (console_arg_word(&other_args, "ep0recovery") &&
        console_arg_end(other_args)) {
      console_printf("EP0 recovery classification: %s\n",
          xhci_ep0_recovery_selftest() ? "PASS" : "FAILED");
      return;
    }
    if (console_arg_word(&other_args, "stopout") &&
        console_arg_end(other_args)) {
      console_printf("Bulk OUT stop reconciliation: %s\n",
          xhci_bulk_out_stop_selftest() ?  "PASS" : "FAILED");
      return;
    }
    if (console_arg_word(&other_args, "faultpaths") &&
        console_arg_end(other_args)) {
      console_printf("xHCI cancellation fault paths: %s\n",
          xhci_cancellation_fault_selftest() ? "PASS" : "FAILED");
      console_printf("USB lifecycle fault paths: %s\n",
          usb_lifecycle_fault_selftest() ? "PASS" : "FAILED");
      return;
    }
    if (console_arg_word(&other_args, "noops") &&
        console_arg_end(other_args)) {
      cmd_usb_test_noops();
      return;
    }
    if (console_arg_word(&other_args, "midiout")) {
      cmd_usb_test_midiout(other_args);
      return;
    }
    if (console_arg_word(&other_args, "midiwrap")) {
      cmd_usb_test_midiwrap(other_args);
      return;
    }
    if (console_arg_word(&other_args, "midiin")) {
      cmd_usb_test_midiin(other_args);
      return;
    }
    if (console_arg_word(&other_args, "midiasync")) {
      cmd_usb_test_midiasync(other_args);
      return;
    }
    if (console_arg_word(&other_args, "routefault") &&
        console_arg_end(other_args)) {
      console_printf("xHCI completion routing: %s\n",
          xhci_route_fault_selftest() ?  "PASS" : "FAILED");
      return;
    }
    if (console_arg_word(&other_args, "disablefault") &&
        console_arg_end(other_args)) {
      console_printf("Disable Slot ownership: %s\n",
          xhci_disable_fault_selftest() ? "PASS" : "FAILED");
      console_printf("Lifecycle failure isolation: %s\n",
          usb_lifecycle_disable_fault_selftest() ? "PASS" : "FAILED");
      return;
    }
    console_puts(
        "Usage: usb test [ep0match | ep0late | ep0recovery | stopout | faultpaths | routefault | disablefault | "
	"noops | midiout | midiwrap | midiin | midiasync]\n");
    return;
  }
  if (console_arg_word(&args, "state") && console_arg_end(args)) {
    cmd_usb_state();
    return;
  }
  if (!console_arg_end(args)) {
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

