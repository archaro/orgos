#include <stdbool.h>
#include <stdint.h>

#include "console.h"
#include "console_args.h"
#include "console_midi.h"
#include "usb_midi.h"
#include "usb_midi_rx.h"
#include "midi_monitor.h"
#include "midi_auto.h"
#include "midi_input.h"
#include "midi_output.h"
#include "midi_stream.h"

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
      !console_arg_end(args)) {
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
  if (console_arg_word(&args, "start")) {
    action = RX_START;
  } else if (console_arg_word(&args, "status")) {
    action = RX_STATUS;
  } else if (console_arg_word(&args, "read")) {
    action = RX_READ;
  } else {
    console_puts("Usage: midi rx [start | status | read] <generation>\n");
    return;
  }
  uint32_t generation = 0U;
  if (!console_arg_generation(args, &generation)) {
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
  if (console_arg_word(&args, "start")) {
    action = MONITOR_START;
  } else if (console_arg_word(&args, "stop")) {
    action = MONITOR_STOP;
  } else if (console_arg_word(&args, "status")) {
    action = MONITOR_STATUS;
  } else {
    console_puts("Usage: midi monitor "
        "[start | stop | status] <generation>\n");
    return;
  }
  uint32_t generation = 0U;
  if (!console_arg_generation(args, &generation)) {
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
  if (console_arg_word(&p, "on") && console_arg_end(p)) {
    midi_auto_set_enabled(true);
    console_puts("Automatic MIDI reception: ON\n");
    return;
  }
  p = args;
  if (console_arg_word(&p, "off") && console_arg_end(p)) {
    midi_auto_set_enabled(false);
    console_puts("Automatic MIDI reception: OFF\n");
    console_puts("Existing receivers retain their ownership.\n");
    return;
  }
  p = args;
  if (console_arg_end(args) || (console_arg_word(&p, "status") && console_arg_end(p))) {

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
  while (!console_arg_end(args)) {
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
  while (!console_arg_end(args)) {
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
  if (console_arg_word(&p, "reset") && console_arg_end(p)) {
    midi_stream_output_t output = {0};
    midi_stream_parser_cancel(&console_stream_parser, &output);
    midi_console_emit(&output, true);
    console_puts("MIDI stream parser reset.\n");
    return;
  }
  p = args;
  if (console_arg_word(&p, "status") && console_arg_end(p)) {
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

  if (console_arg_word(&args, "start")) {
    action = INPUT_START;
  } else if (console_arg_word(&args, "stop")) {
    action = INPUT_STOP;
  } else if (console_arg_word(&args, "status")) {
    action = INPUT_STATUS;
  } else if (console_arg_word(&args, "read")) {
    action = INPUT_READ;
  } else {
    console_puts("Usage: midi input [start | stop | status | read] <generation>\n");
    return;
  }

  uint32_t generation = 0U;

  if (!console_arg_generation(args, &generation)) {
    console_puts("Usage: midi input [start | stop | status | read] <generation>\n");
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
  if (console_arg_word(&p, "begin")) {
    unsigned int generation = 0U;
    unsigned int cable = 0U;
    if (!console_arg_uint(&p, &generation) || generation == 0U ||
        !console_arg_uint(&p, &cable) || !console_arg_end(p)) {
      console_puts("Usage: midi output sysex begin "
          "<generation> <cable>\n");
      return;
    }
    bool started = midi_output_sysex_begin(generation, cable);
    console_printf("MIDI SysEx: %s\n", started ? "STARTED" : "NOT READY");
    return;
  }
  p = args;
  if (console_arg_word(&p, "write")) {
    unsigned int generation = 0U;
    if (!console_arg_uint(&p, &generation) || generation == 0U || console_arg_end(p)) {
      console_puts("Usage: midi output sysex write "
          "<generation> <hh> [hh ...]\n");
      return;
    }
    // Validate the whole argument list before
    // accepting any part of it.
    const char *probe = p;
    unsigned int total = 0U;
    uint8_t byte = 0U;
    while (!console_arg_end(probe)) {
      if (!midi_next_hex_byte(&probe, &byte) || byte >= 0x80U) {
        console_puts("SysEx payload must contain "
            "seven-bit hexadecimal bytes.\n");
        return;
      }
      total++;
    }
    unsigned int accepted = 0U;
    while (!console_arg_end(p)) {
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
  if (console_arg_word(&p, "end")) {
    uint32_t generation = 0U;
    if (!console_arg_generation(p, &generation)) {
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
  if (console_arg_word(&p, "sysex")) {
    cmd_midi_output_sysex(p);
    return;
  }
  if (console_arg_word(&p, "send")) {
    unsigned int generation = 0U;
    unsigned int cable = 0U;
    uint8_t bytes[3] = {0};
    unsigned int count = 0U;
    if (!console_arg_uint(&p, &generation) || generation == 0U ||
        !console_arg_uint(&p, &cable)) {
      console_puts("Usage: midi output send "
          "<generation> <cable> <hh> [hh ...]\n");
      return;
    }
    while (!console_arg_end(p)) {
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
  if (console_arg_word(&p, "test")) {
    unsigned int generation = 0U;
    unsigned int count = 0U;
    if (!console_arg_uint(&p, &generation) || generation == 0U ||
        !console_arg_uint(&p, &count) || count == 0U ||
        count > 128U || !console_arg_end(p)) {
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
  if (console_arg_word(&args, "start")) {
    action = OUTPUT_START;
  } else if (console_arg_word(&args, "stop")) {
    action = OUTPUT_STOP;
  } else if (console_arg_word(&args, "status")) {
    action = OUTPUT_STATUS;
  } else {
    console_puts("Usage: midi output "
        "[start | stop | status | send | test | sysex]\n");
    return;
  }
  uint32_t generation = 0U;
  if (!console_arg_generation(args, &generation)) {
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

void console_midi_command(const char *args) {
  if (console_arg_word(&args, "decode")) {
    cmd_midi_decode(args);
    return;
  }
  if (console_arg_word(&args, "rx")) {
    cmd_midi_rx(args);
    return;
  }
  if (console_arg_word(&args, "monitor")) {
    cmd_midi_monitor(args);
    return;
  }
  if (console_arg_word(&args, "auto")) {
    cmd_midi_auto(args);
    return;
  }
  if (console_arg_word(&args, "stream")) {
    cmd_midi_stream(args);
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
  console_puts("Usage: midi [decode | rx | monitor | auto | stream | input | output]\n");
}

