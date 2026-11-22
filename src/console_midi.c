#include <stdbool.h>
#include <stdint.h>

#include "console.h"
#include "console_args.h"
#include "console_midi.h"
#include "xhci.h"
#include "usb_midi.h"
#include "usb_midi_rx.h"
#include "midi_auto.h"
#include "midi_input.h"
#include "midi_output.h"
#include "midi_stream.h"
#include "midi_ports.h"
#include "midi_ports_usb.h"
#include "midi_port_io.h"

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

static bool midi_console_short_message(const char *args,
    midi_short_message_t *message) {

  if (!message)
    return false;

  uint8_t bytes[3] = {0};
  unsigned int count = 0U;

  while (!console_arg_end(args)) {
    if (count == 3U || !midi_next_hex_byte(&args, &bytes[count]))
      return false;

    count++;
  }

  if (count == 0U)
    return false;

  *message = (midi_short_message_t){
    .status = bytes[0],
    .length = (uint8_t)count
  };

  if (count > 1U)
    message->data[0] = bytes[1];

  if (count > 2U)
    message->data[1] = bytes[2];

  return true;
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
  if (console_arg_word(&args, "status")) {
    uint32_t generation = 0U;
    if (!console_arg_generation(args, &generation)) {
      console_puts("Usage: midi rx status <generation>\n");
      return;
    }
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
    console_printf("  Idle policy: %s\n",
        info.discard_unclaimed ? "Discard" : "Retain");
    console_printf("  Idle drops : %llu\n",
        (unsigned long long)info.unclaimed_dropped);
    console_printf("  Staged     : %u\n", info.staged);
    console_printf("  Queue      : %u / %u\n",
        info.queue_depth, USB_MIDI_RX_QUEUE_CAPACITY);
    console_printf("  Queue peak : %u\n", info.queue_peak);
    xhci_bulk_in_stream_stats_t hw = {0};
    if (xhci_bulk_in_stream_get_stats(info.slot_id, &hw)) {
      console_printf("  HW queue   : %u / %u\n",
          hw.queue_depth, XHCI_BULK_IN_QUEUE_CAPACITY);
      console_printf("  HW peak    : %u\n", hw.queue_peak);
      console_printf("  HW pauses  : %u\n", hw.pauses);
      console_printf("  HW paused  : %s\n", hw.paused ? "Yes" : "No");
      console_printf("  HW failed  : %s\n", hw.failed ? "Yes" : "No");
    }
  } else {
    console_puts("Usage: midi rx status <generation>\n");
  }
}

static void cmd_midi_auto(const char *args) {
  const char *p = args;
  if (console_arg_word(&p, "on") && console_arg_end(p)) {
    midi_auto_set_enabled(true);
    console_puts("Automatic MIDI reception: ON\n");
    return;
  }
  p = args;
  if (console_arg_word(&p, "off") && console_arg_end(p)) {
    midi_auto_set_enabled(false);
    console_puts("Automatic MIDI reception: OFF\n");
    console_puts("Existing MIDI services retain their ownership.\n");
    console_puts("Disconnect and reconnect to reserve "
        "a device for diagnostics.\n");
    return;
  }
  p = args;
  if (console_arg_end(args) || (console_arg_word(&p, "status") && console_arg_end(p))) {

    midi_auto_info_t info = {0};
    if (!midi_auto_get_info(&info)) {
      console_puts("Automatic MIDI reception unavailable.\n");
      return;
    }
    console_puts("Automatic MIDI attachment:\n");
    console_printf("  Enabled     : %s\n", info.enabled ? "Yes" : "No");
    console_printf("  Attempts    : %u\n", (unsigned int)info.attempts);
    console_printf("  IN started  : %u\n", (unsigned int)info.started);
    console_printf("  IN skipped  : %u\n", (unsigned int)info.skipped);
    console_printf("  IN failed   : %u\n", (unsigned int)info.failed);
    console_printf("  OUT started : %u\n", (unsigned int)info.output_started);
    console_printf("  OUT skipped : %u\n", (unsigned int)info.output_skipped);
    console_printf("  OUT failed  : %u\n", (unsigned int)info.output_failed);
    console_printf("  IN retries  : %u\n", (unsigned int)info.input_retries);
    console_printf("  OUT retries : %u\n", (unsigned int)info.output_retries);
    console_printf("  IN terminal : %u\n", (unsigned int)info.input_terminal);
    console_printf("  OUT terminal: %u\n",
        (unsigned int)info.output_terminal);
    console_printf("  IN pending  : %u\n", (unsigned int)info.input_pending);
    console_printf("  OUT pending : %u\n", (unsigned int)info.output_pending);
    if (info.attempts != 0U)
      console_printf("  Last gen   : %u\n",
          (unsigned int)info.last_generation);
    return;
  }
  console_puts("Usage: midi auto [on | off | status]\n");
}

static void midi_console_emit(const midi_stream_output_t *output) {
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
      continue;
    }
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
}

static void cmd_midi_input(const char *args) {
  const char *probe = args;
  if (console_arg_word(&probe, "selftest") && console_arg_end(probe)) {
    bool passed = midi_input_backpressure_selftest();
    console_printf("MIDI input backpressure: %s\n", passed ? "PASS" : "FAIL");
    return;
  } else if (console_arg_word(&args, "status")) {
    uint32_t generation = 0U;
    if (!console_arg_generation(args, &generation)) {
      console_puts("Usage: midi input status <generation>\n");
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
    console_printf("  Selection   : %s\n",
        info.port_filtered ? "Opened ports" : "All cables");
    if (info.port_filtered)
      console_printf("  Open mask   : 0x%04x\n",
        (unsigned int)info.open_cables);
    console_printf("  Unclaimed   : %llu packets\n",
        (unsigned long long)info.unclaimed_packets);
    console_printf("  Cables      : %u\n", (unsigned int)info.cables);
    console_printf("  Packets     : %llu\n",
        (unsigned long long)info.packets);
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
    console_printf("  Closed drop : %llu events\n",
        (unsigned long long)info.closed_events);
    if (!info.have_last)
      return;
    console_printf("  Last cable  : %u\n", (unsigned int)info.last_cable);
    console_puts("  Last event:\n");

    // Reuse the existing stream-event formatter.
    midi_stream_output_t output = {
      .count = 1U
    };

    output.events[0] = info.last;
    midi_console_emit(&output);
  } else {
    console_puts("Usage: midi input [status <generation> | selftest]\n");
    return;
  }
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

static void cmd_midi_output(const char *args) {
  const char *p = args;
  if (console_arg_word(&p, "losses") && console_arg_end(p)) {
    midi_output_loss_info_t info = {0};
    if (!midi_output_loss_get_info(&info)) {
      console_puts("MIDI output loss statistics unavailable.\n");
      return;
    }

    console_puts("MIDI output loss accounting:\n");
    console_printf("  Loss events       : %u\n", info.loss_events);
    console_printf("  Not submitted     : %llu\n",
        (unsigned long long)info.not_submitted);
    console_printf("  Unconfirmed       : %llu\n",
        (unsigned long long)info.submitted_unconfirmed);
    console_printf("  Invariant faults  : %u\n", info.invariant_violations);
    if (info.loss_events != 0U) {
      console_printf("  Last generation   : %u\n", info.last_generation);
      console_printf("  Last state        : %s\n",
          midi_output_state_name(info.last_state));
      console_printf("  Last error        : %u\n", info.last_error);
      console_printf("  Last not submitted: %u\n", info.last_not_submitted);
      console_printf("  Last unconfirmed  : %u\n",
          info.last_submitted_unconfirmed);
    }
    return;
  }
  if (console_arg_word(&args, "status")) {
    uint32_t generation = 0U;
    if (!console_arg_generation(args, &generation)) {
      console_puts("Usage: midi output status <generation>\n");
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
    console_printf("  Confirmed   : %llu\n", (unsigned long long)info.confirmed);
    console_printf("  Transfers   : %llu\n", (unsigned long long)info.transfers);
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
    return;
  }
  console_puts("Usage: midi output [status | losses]\n");
  return;
}

static const char *midi_port_state_name(midi_port_state_t state) {
  switch (state) {
    case MIDI_PORT_UNBOUND: return "Unbound";
    case MIDI_PORT_BOUND: return "Bound";
    case MIDI_PORT_AMBIGUOUS: return "Ambiguous";
    default: return "Unknown";
  }
}

static const char *midi_port_direction_name(
    midi_port_direction_t direction) {
  return direction == MIDI_PORT_IN ? "IN" : "OUT";
}

static midi_port_handle_t console_handles[MIDI_PORT_CAPACITY];

static midi_port_handle_t *console_port_handle(unsigned int id) {
  return id >= 1U && id <= MIDI_PORT_CAPACITY ? &console_handles[id - 1U] : NULL;
}

static void midi_port_print(const midi_port_info_t *info) {
  console_printf("MIDI port %u:\n", info->id);
  console_printf("  Direction  : %s\n",
      midi_port_direction_name(info->key.direction));
  console_printf("  State      : %s\n", midi_port_state_name(info->state));
  console_printf("  Root port  : %u\n", (unsigned int)info->key.root_port);
  console_printf("  Hub port   : %u\n", (unsigned int)info->key.hub_port);
  console_printf("  VID:PID    : %04x:%04x\n",
      (unsigned int)info->key.vendor_id, (unsigned int)info->key.product_id);
  console_printf("  Cable      : %u\n", (unsigned int)info->key.cable);
  if (info->state == MIDI_PORT_BOUND) {
    console_printf("  Generation : %u\n", (unsigned int)info->generation);
    console_printf("  Slot       : %u\n", (unsigned int)info->slot_id);
  }
  midi_port_lifecycle_info_t lifecycle = {0};
  if (midi_port_lifecycle_get_info(info->id, &lifecycle) != MIDI_STATUS_OK)
    return;
  console_printf("  Opened gen : %u\n",
      (unsigned int)lifecycle.opened_generation);
  console_printf("  Retired    : %u events\n", lifecycle.retired_depth);
  console_printf("  Lost events: %llu\n",
      (unsigned long long)lifecycle.retired_dropped);
  console_printf("  Missed retirements: %llu\n",
      (unsigned long long)lifecycle.retirement_missed);
  console_printf("  Changes    : %u pending\n", lifecycle.pending_changes);
  console_printf("  Lost changes: %llu\n",
      (unsigned long long)lifecycle.changes_dropped);
  midi_port_handle_info_t handle = {0};
  if (midi_port_get_handle_info(console_port_handle(info->id), &handle) !=
      MIDI_STATUS_OK)
    return;
  console_printf("  Handle      : %s\n", midi_status_name(handle.status));
  console_printf("  Handle gen  : %u\n", handle.handle.generation);
  console_printf("  Handle retired: %u events\n", handle.retired_depth);
  if (handle.service_present) {
    console_printf("  Service queue: %u / %u\n",
        handle.service_queue_depth, handle.service_queue_capacity);
    console_printf("  Service pending aborts: %u\n", handle.service_pending_aborts);
    console_printf("  Service in flight: %u\n", handle.service_in_flight);
  }
}

static midi_port_sysex_t console_sysex[MIDI_PORT_CAPACITY];

static bool midi_sysex_port_id(const char **args, unsigned int *id) {
  return console_arg_uint(args, id) &&
      *id >= 1U && *id <= MIDI_PORT_CAPACITY;
}

static void cmd_midi_port_sysex(const char *args) {
  const char *p = args;
  unsigned int id = 0U;

  if (console_arg_word(&p, "begin")) {
    if (!midi_sysex_port_id(&p, &id) || !console_arg_end(p))
      goto usage;

    midi_port_sysex_t *session = &console_sysex[id - 1U];

    midi_status_t started = midi_port_sysex_begin(console_port_handle(id), session);

    console_printf("MIDI port %u SysEx: %s\n",
        id, midi_status_name(started));
    return;
  }

  p = args;
  if (console_arg_word(&p, "write")) {
    if (!midi_sysex_port_id(&p, &id) || console_arg_end(p))
      goto usage;

    const char *probe = p;
    uint8_t byte = 0U;
    unsigned int total = 0U;

    // Validate all bytes before accepting any.
    while (!console_arg_end(probe)) {
      if (!midi_next_hex_byte(&probe, &byte) || byte >= 0x80U) {
        console_puts("SysEx payload requires "
            "seven-bit hexadecimal bytes.\n");
        return;
      }
      total++;
    }

    midi_port_sysex_t *session = &console_sysex[id - 1U];
    unsigned int accepted = 0U;

    midi_status_t status = MIDI_STATUS_OK;
    while (!console_arg_end(p)) {
      if (!midi_next_hex_byte(&p, &byte)) {
        status = MIDI_STATUS_INVALID_ARGUMENT;
        break;
      }
      status = midi_port_sysex_write(session, byte);
      if (status != MIDI_STATUS_OK)
        break;
      accepted++;
    }

    console_printf("MIDI port %u SysEx: "
        "%u / %u bytes accepted (%s)\n", id, accepted, total,
        midi_status_name(status));

    if (accepted != total)
      console_puts("Retry from the first unaccepted byte, "
          "or cancel the session.\n");

    return;
  }

  p = args;
  if (console_arg_word(&p, "end")) {
    if (!midi_sysex_port_id(&p, &id) || !console_arg_end(p))
      goto usage;

    midi_status_t ended = midi_port_sysex_end(&console_sysex[id - 1U]);

    console_printf("MIDI port %u SysEx end: %s\n",
        id, midi_status_name(ended));
    return;
  }

  p = args;
  if (console_arg_word(&p, "cancel")) {
    if (!midi_sysex_port_id(&p, &id) || !console_arg_end(p))
      goto usage;

    midi_status_t cancelled = midi_port_sysex_cancel(&console_sysex[id - 1U]);
    console_printf("MIDI port %u SysEx: %s\n",
        id, midi_status_name(cancelled));
    return;
  }

  p = args;
  if (console_arg_word(&p, "status")) {
    if (!midi_sysex_port_id(&p, &id) || !console_arg_end(p))
      goto usage;

    const midi_port_sysex_t *session = &console_sysex[id - 1U];
    midi_port_sysex_info_t info = {0};
    midi_status_t status = midi_port_sysex_get_info(session, &info);
    if (status != MIDI_STATUS_OK) {
      console_printf("MIDI port %u SysEx status: %s\n", id,
          midi_status_name(status));
      return;
    }
    console_printf("MIDI port %u SysEx session:\n", id);
    console_printf("  Status     : %s\n", midi_status_name(info.status));
    console_printf("  Claim present: %s\n", info.claim_present ? "Yes" : "No");
    console_printf("  Staged     : %u bytes\n", info.staged);
    console_printf("  Fragments  : %u\n", info.fragments);
    console_printf("  Active     : %s\n", session->active ? "Yes" : "No");
    if (session->active) {
      console_printf("  Generation : %u\n",
          (unsigned int)session->generation);
      console_printf("  Cable      : %u\n", (unsigned int)session->cable);
      console_printf("  Claim      : %u\n", (unsigned int)session->claim);
    }
    return;
  }

usage:
  console_puts("Usage: midi port sysex "
      "[begin | end | cancel | status] <id>\n"
      "       midi port sysex write "
      "<id> <hh> [hh ...]\n");
}

typedef struct {
  bool active;
  unsigned int port;
  uint32_t generation;
  midi_port_handle_t handle;
  unsigned int expected_count;
  unsigned int observed;
  unsigned int mismatches;
} midi_port_verify_t;

static midi_port_verify_t port_verify;

static void cmd_midi_port_verify(const char *args) {
  const char *p = args;

  if (console_arg_word(&p, "start")) {
    unsigned int id = 0U;
    unsigned int count = 0U;

    if (!console_arg_uint(&p, &id) ||
        !console_arg_uint(&p, &count) ||
        !console_arg_end(p) ||
        id == 0U || id > MIDI_PORT_CAPACITY ||
        count == 0U)
      goto usage;

    midi_port_info_t port = {0};
    midi_port_lifecycle_info_t life = {0};

    if ((midi_port_get_info(id, &port) != MIDI_STATUS_OK) ||
        (midi_port_lifecycle_get_info(id, &life) != MIDI_STATUS_OK) ||
        port.state != MIDI_PORT_BOUND ||
        port.key.direction != MIDI_PORT_IN ||
        life.opened_generation != port.generation) {
      console_puts("Open a bound MIDI IN port first.\n");
      return;
    }

    midi_input_info_t input = {0};
    usb_midi_rx_info_t rx = {0};

    if (!midi_input_get_info(port.generation, &input) ||
        !usb_midi_rx_get_info(port.generation, &rx) ||
        !input.attached ||
        input.queue_depth != 0U ||
        input.pending_aborts != 0U ||
        life.retired_depth != 0U ||
        rx.queue_depth != 0U || rx.staged != 0U) {
      console_puts("Input queues must be empty first.\n");
      return;
    }

    port_verify = (midi_port_verify_t){
      .active = true,
      .port = id,
      .generation = port.generation,
      .handle = *console_port_handle(id),
      .expected_count = count
    };

    console_printf("MIDI receive verification: ARMED\n"
        "  Port       : %u\n"
        "  Generation : %u\n"
        "  Expected   : %u\n",
        id, port.generation, count);
    return;
  }

  p = args;
  if (console_arg_word(&p, "drain")) {
    unsigned int limit = 64U;

    if (!console_arg_end(p) && (!console_arg_uint(&p, &limit) ||
         !console_arg_end(p) || limit == 0U || limit > 64U))
      goto usage;

    if (!port_verify.active) {
      console_puts("No receive verification armed.\n");
      return;
    }

    unsigned int drained = 0U;

    while (drained < limit) {
      midi_port_event_t received = {0};

      if (midi_port_read(&port_verify.handle, &received) != MIDI_STATUS_OK)
        break;

      // Test pattern: CC 102 on channel 16,
      // with values cycling from 0 through 127.
      unsigned int expected = port_verify.observed & 0x7fU;

      if (received.generation != port_verify.generation ||
          received.event.type != MIDI_STREAM_SHORT ||
          received.event.message.length != 3U ||
          received.event.message.status != 0xbfU ||
          received.event.message.data[0] != 0x66U ||
          received.event.message.data[1] != expected)
        port_verify.mismatches++;

      port_verify.observed++;
      drained++;
    }

    console_printf("MIDI receive verification:\n"
        "  Drained    : %u\n Observed   : %u / %u\n Mismatches : %u\n",
        drained, port_verify.observed, port_verify.expected_count,
        port_verify.mismatches);
    return;
  }

  p = args;
  if (console_arg_word(&p, "status") &&
      console_arg_end(p)) {

    if (!port_verify.active) {
      console_puts("No receive verification armed.\n");
      return;
    }

    midi_port_lifecycle_info_t life = {0};
    bool current = (midi_port_lifecycle_get_info(port_verify.port, &life) == MIDI_STATUS_OK) &&
        life.opened_generation == port_verify.generation &&
        console_port_handle(port_verify.port)->open_token ==
            port_verify.handle.open_token;

    const char *result = "INCOMPLETE";

    if (!current)
      result = "DEVICE CHANGED";
    else if (port_verify.mismatches != 0U ||
        port_verify.observed > port_verify.expected_count)
      result = "FAIL";
    else if (port_verify.observed == port_verify.expected_count)
      result = "PASS";

    console_printf("MIDI receive verification:\n Result     : %s\n"
        " Port       : %u\n Generation : %u\n"
        " Observed   : %u / %u\n Mismatches : %u\n",
        result, port_verify.port,
        port_verify.generation, port_verify.observed,
        port_verify.expected_count, port_verify.mismatches);
    return;
  }

  p = args;
  if (console_arg_word(&p, "reset") && console_arg_end(p)) {
    port_verify = (midi_port_verify_t){0};
    console_puts("MIDI receive verification: RESET\n");
    return;
  }

usage:
  console_puts("Usage: midi port verify "
      "[start <id> <count> | drain [1..64] |\n status | reset]\n");
}

static void cmd_midi_port(const char *args) {
  const char *p = args;

  if (console_arg_word(&p, "verify")) {
    cmd_midi_port_verify(p);
    return;
  }
  if (console_arg_word(&p, "sysex")) {
    cmd_midi_port_sysex(p);
    return;
  }
  if (console_arg_word(&p, "list") && console_arg_end(p)) {
    unsigned int count = 0U;
    unsigned int cursor = 0U;
    for (;;) {
      midi_port_info_t info = {0};
      midi_status_t status = midi_port_next(&cursor, &info);
      if (status == MIDI_STATUS_EMPTY)
        break;
      if (status != MIDI_STATUS_OK) {
        console_printf("MIDI port discovery: %s\n", midi_status_name(status));
        return;
      }
      console_printf("  %u: %s %u/%u cable %u, %s (gen %u)\n",
          info.id, midi_port_direction_name(info.key.direction),
          (unsigned int)info.key.root_port,
          (unsigned int)info.key.hub_port,
          (unsigned int)info.key.cable,
          midi_port_state_name(info.state),
          (unsigned int)info.generation);
      count++;
    }
    if (count == 0U)
      console_puts("No logical MIDI ports defined.\n");
    return;
  }

  p = args;
  if (console_arg_word(&p, "bind")) {
    unsigned int id = 0U;
    unsigned int generation = 0U;
    unsigned int cable = 0U;
    midi_port_direction_t direction;

    if (!console_arg_uint(&p, &id))
      goto usage;

    if (console_arg_word(&p, "in"))
      direction = MIDI_PORT_IN;
    else if (console_arg_word(&p, "out"))
      direction = MIDI_PORT_OUT;
    else
      goto usage;

    if (!console_arg_uint(&p, &generation) || generation == 0U ||
        !console_arg_uint(&p, &cable) || !console_arg_end(p))
      goto usage;

    bool defined = midi_ports_usb_define(id, direction, generation, cable);
    console_printf("MIDI port %u: %s\n", id,
        defined ? "DEFINED" : "NOT DEFINED");
    return;
  }

  p = args;
  if (console_arg_word(&p, "status")) {
    unsigned int id = 0U;
    if (!console_arg_uint(&p, &id) || !console_arg_end(p))
      goto usage;

    midi_port_info_t info = {0};
    midi_status_t status = midi_port_get_info(id, &info);
    if (status != MIDI_STATUS_OK) {
      console_printf("MIDI port %u status: %s\n", id, midi_status_name(status));
      return;
    }
    midi_port_print(&info);
    return;
  }

  p = args;
  if (console_arg_word(&p, "clear")) {
    unsigned int id = 0U;
    if (!console_arg_uint(&p, &id) || !console_arg_end(p))
      goto usage;
    if (id >= 1U && id <= MIDI_PORT_CAPACITY) {
      midi_port_sysex_t *session = &console_sysex[id - 1U];
      if (session->active) {
        midi_status_t status = midi_port_sysex_cancel(session);
        if (status != MIDI_STATUS_OK) {
          console_printf("MIDI port %u SysEx cancel: %s\n", id,
              midi_status_name(status));
          return;
        }
      }
    }
    midi_status_t cleared = midi_port_undefine(id);
    console_printf("MIDI port %u: %s\n",
        id, midi_status_name(cleared));
    return;
  }

  p = args;
  if (console_arg_word(&p, "open")) {
    unsigned int id = 0U;

    if (!console_arg_uint(&p, &id) ||
        !console_arg_end(p))
      goto usage;

    midi_status_t opened = midi_port_open(id, console_port_handle(id));

    console_printf("MIDI port %u: %s\n",
        id, midi_status_name(opened));
    return;
  }

  p = args;
  if (console_arg_word(&p, "close")) {
    unsigned int id = 0U;

    if (!console_arg_uint(&p, &id) || !console_arg_end(p) ||
        id == 0U || id > MIDI_PORT_CAPACITY)
      goto usage;

    if (console_sysex[id - 1U].active) {
      console_puts("End or cancel SysEx before closing this port.\n");
      return;
    }

    midi_status_t closed = midi_port_close(console_port_handle(id));
    console_printf("MIDI port %u: %s\n", id, midi_status_name(closed));
    return;
  }

  p = args;
  if (console_arg_word(&p, "read")) {
    unsigned int id = 0U;

    if (!console_arg_uint(&p, &id) ||
        !console_arg_end(p))
      goto usage;

    midi_port_event_t received = {0};

    midi_status_t status = midi_port_read(console_port_handle(id), &received);
    if (status != MIDI_STATUS_OK) {
      console_printf("MIDI port %u read: %s\n", id, midi_status_name(status));
      return;
    }

    console_printf("MIDI port %u event (generation %u):\n",
        received.port, (unsigned int)received.generation);

    midi_stream_output_t output = {
      .count = 1U
    };

    output.events[0] = received.event;
    midi_console_emit(&output);
    return;
  }

  p = args;
  if (console_arg_word(&p, "send") || console_arg_word(&p, "write")) {
    unsigned int id = 0U;

    if (!console_arg_uint(&p, &id))
      goto usage;

    midi_short_message_t message = {0};
    if (!midi_console_short_message(p, &message))
      goto usage;

    midi_status_t queued = midi_port_write(console_port_handle(id), &message);
    console_printf("MIDI port %u: %s\n",
        id, midi_status_name(queued));
    return;
  }

  p = args;

  if (console_arg_word(&p, "change")) {
    unsigned int id = 0U;

    if (!console_arg_uint(&p, &id) ||
        !console_arg_end(p))
      goto usage;

    midi_port_change_t change = {0};

    midi_status_t status = midi_port_next_change(id, &change);
    if (status != MIDI_STATUS_OK) {
      console_printf("MIDI port %u change: %s\n", id, midi_status_name(status));
      return;
    }

    console_printf("MIDI port %u change:\n", change.port);
    console_printf("  Previous : %s (generation %u)\n",
        midi_port_state_name(change.previous),
        (unsigned int)change.previous_generation);
    console_printf("  Current  : %s (generation %u)\n",
        midi_port_state_name(change.current),
        (unsigned int)change.current_generation);
    return;
  }

usage:
  console_puts("Usage: midi port "
      "[list | status <id> | clear <id> | verify |\n"
      "  bind <id> in|out <generation> <cable> |\n"
      "  open <id> | read <id> | change <id> | close <id> |\n"
      "  write|send <id> <hh> [hh ...] | sysex ...]\n");
}

void console_midi_command(const char *args) {
  if (console_arg_word(&args, "rx")) {
    cmd_midi_rx(args);
    return;
  }
  if (console_arg_word(&args, "auto")) {
    cmd_midi_auto(args);
    return;
  }
  if (console_arg_word(&args, "input")) {
    cmd_midi_input(args);
    return;
  }
  if (console_arg_word(&args, "output")) {
    cmd_midi_output(args);
    return;
  }
  if (console_arg_word(&args, "port")) {
    cmd_midi_port(args);
    return;
  }
  console_puts("Usage: midi [rx | auto | input | output | port]\n");
}

