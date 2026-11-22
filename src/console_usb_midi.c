#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "console.h"
#include "console_args.h"
#include "console_commands.h"
#include "console_usb_internal.h"
#include "timer.h"
#include "page.h"
#include "xhci.h"
#include "xhci_transfer.h"
#include "usb.h"
#include "usb_enumerate.h"
#include "usb_midi.h"
#include "usb_midi_rx.h"
#include "midi_output.h"
#include "usb_midi_test.h"

static void usb_test_delay_ms(uint32_t milliseconds) {
  uint64_t started = timer_now_us();
  uint64_t duration = (uint64_t)milliseconds * 1000ULL;
  while (timer_now_us() - started < duration) {
    usb_service();
    timer_delay_ms(1U);
  }
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
  if (!console_arg_generation(args, &generation))
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
  if (!console_arg_uint(&p, &requested) || requested == 0U)
    return false;

  uint32_t generation = (uint32_t)requested;

  if (midi_output_owns_generation(generation)) {
    console_puts("Bulk OUT owned by the MIDI output service.\n");
    return false;
  }

  // The virtual cable is optional.
  // Cable zero remains the default.
  if (!console_arg_end(p)) {
    if (!console_arg_word(&p, "cable") || !console_arg_uint(&p, cable) ||
        !console_arg_end(p))
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

void cmd_usb_test_midiout(const char *args) {
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

void cmd_usb_test_midiasync(const char *args) {
  const char *p = args;
  if (console_arg_word(&p, "start")) {
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
  if (console_arg_word(&args, "status")) {
    action = ASYNC_STATUS;
  } else if (console_arg_word(&args, "reap")) {
    action = ASYNC_REAP;
  } else {
    console_puts("Usage: usb test midiasync "
        "[start | status | reap] <generation>\n");
    return;
  }
  uint32_t generation = 0U;
  if (!console_arg_generation(args, &generation)) {
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
  if (console_arg_word(&args, "reset") && console_arg_end(args)) {
    bool reset = usb_midi_in_stress_reset();
    console_printf("USB-MIDI IN stress reset: %s\n",
        reset ? "OK" : "NOT READY");
    return;
  }
  if (console_arg_word(&args, "start")) {
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
  if (console_arg_word(&args, "status") && console_arg_end(args)) {
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

void cmd_usb_test_midiin(const char *args) {
  const char *sub_args = args;
  if (console_arg_word(&sub_args, "stress")) {
    cmd_usb_test_midiin_stress(sub_args);
    return;
  }
  // Identify the requested operation before
  // resolving its mandatory device generation.
  enum { MIDI_IN_STREAM, MIDI_IN_READ, MIDI_IN_STATUS } action;
  sub_args = args;
  if (console_arg_word(&sub_args, "stream")) {
    action = MIDI_IN_STREAM;
  } else if (console_arg_word(&sub_args, "read")) {
    action = MIDI_IN_READ;
  } else if (console_arg_word(&sub_args, "status")) {
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

void cmd_usb_test_midiwrap(const char *args) {
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

