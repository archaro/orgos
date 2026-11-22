#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "console.h"
#include "console_args.h"
#include "console_commands.h"
#include "console_usb_internal.h"
#include "pcie.h"
#include "xhci.h"
#include "xhci_transfer.h"
#include "usb.h"
#include "usb_hub.h"
#include "usb_enumerate.h"
#include "usb_midi_quirks.h"
#include "usb_midi.h"
#include "usb_midi_rx.h"
#include "midi_output.h"
#include "usb_midi_test.h"

void cmd_usb_state(void) {
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

void cmd_usb_test_ep0match(void) {
  bool passed = xhci_ep0_match_selftest();
  console_printf("EP0 TD matching: %s\n", passed ? "PASS" : "FAILED");
}

void cmd_usb_test_noops(void) {
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

void cmd_usb_events(void) {
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

void cmd_usb_hubports(void) {
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

void cmd_usb_devices(void) {
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

void cmd_usb_enumerate(const char *args) {
  unsigned int requested = 0U;
  if (!console_arg_word(&args, "port") || !console_arg_uint(&args, &requested) ||
      !console_arg_end(args) || requested == 0U || requested > 4U) {
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

void cmd_usb_enumstate(const char *args) {
  unsigned int index = 0U;
  if (!console_arg_word(&args, "record") || !console_arg_uint(&args, &index) ||
      !console_arg_end(args) || index >= USB_MANAGED_DEVICE_CAPACITY) {
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
    case USB_RECOVERY_READY_EP0_LATE_SUCCESS:
      return "Ready (reconciled late EP0 success)";
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

void cmd_usb_recover_status(const char *args) {
  uint32_t generation = 0U;
  if (!console_arg_generation(args, &generation)) {
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
  console_printf("  Late events: %u\n",
      (unsigned int)info.ep0_late_completions);
  if (info.ep0_state_valid) {
    console_printf("  EP0 state  : %s (%u)\n",
        xhci_ep_state_name(info.ep0_state), (unsigned int)info.ep0_state);
  } else {
    console_puts("  EP0 state  : Unavailable\n");
  }
}

void cmd_usb_recover(const char *args) {
  uint32_t generation = 0U;
  if (!console_arg_generation(args, &generation)) {
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
       recovery.result != USB_RECOVERY_READY_EP0_ERROR &&
       recovery.result != USB_RECOVERY_READY_EP0_LATE_SUCCESS)) {
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

void cmd_usb_transfers(const char *args) {
  uint32_t generation = 0U;
  if (!console_arg_generation(args, &generation)) {
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
  console_printf("  EP0 uncertain : %s\n",
      state.ep0_uncertain ? "Yes" : "No");
  console_printf("  EP0 late events: %u\n",
      (unsigned int)state.ep0_late_completions);
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
    case USB_LIFECYCLE_STOP_OUT: return "Stop Bulk OUT";
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

void cmd_usb_lifecycle_status(const char *args) {
  uint32_t generation = 0U;
  if (!console_arg_generation(args, &generation)) {
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

void cmd_usb_lifecycle_port(const char *args) {
  uint32_t generation = 0U;
  if (!console_arg_generation(args, &generation)) {
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

void cmd_usb_topology(bool detail) {
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

void cmd_usb_changes(void) {
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

void cmd_usb_changes_dump(void) {
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

void cmd_usb_hubirq(void) {
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

void cmd_usb_auto_status(void) {
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

void cmd_usb_auto_ports(void) {
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

void cmd_usb_resources(void) {
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

void cmd_usb_auto(const char *args) {
  if (console_arg_end(args)) {
    cmd_usb_auto_status();
    return;
  }
  const char *word = args;
  if (console_arg_word(&word, "on") && console_arg_end(word)) {
    usb_auto_enum_set_enabled(true);
    console_puts("USB auto enumeration: ON\n");
    return;
  }
  word = args;
  if (console_arg_word(&word, "off") && console_arg_end(word)) {
    usb_auto_enum_set_enabled(false);
    console_puts("USB auto enumeration: OFF\n");
    return;
  }
  word = args;
  if (console_arg_word(&word, "status") && console_arg_end(word)) {
    cmd_usb_auto_status();
    return;
  }
  word = args;
  if (console_arg_word(&word, "ports") && console_arg_end(word)) {
    cmd_usb_auto_ports();
    return;
  }
  console_puts("Usage: usb auto [on|off|status|ports]\n");
}

