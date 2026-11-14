#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "midi_stream.h"
#include "midi_usb_bridge.h"

static midi_stream_output_t feed(midi_stream_parser_t *parser,
    uint8_t byte, unsigned int expected) {
  midi_stream_output_t output = {0};
  assert(midi_stream_parser_feed(parser, byte, &output));
  assert(output.count == expected);
  return output;
}

static void test_stream_messages(void) {
  midi_stream_parser_t parser = {0};
  midi_stream_output_t output = {0};

  feed(&parser, 0x90U, 0U);
  feed(&parser, 0x3cU, 0U);

  // Realtime must not interrupt an unfinished note.
  output = feed(&parser, 0xf8U, 1U);
  assert(output.events[0].type == MIDI_STREAM_SHORT);
  assert(output.events[0].message.status == 0xf8U);

  output = feed(&parser, 0x40U, 1U);
  assert(output.events[0].message.status == 0x90U);
  assert(output.events[0].message.length == 3U);
  assert(output.events[0].message.data[0] == 0x3cU);
  assert(output.events[0].message.data[1] == 0x40U);

  // Running status.
  feed(&parser, 0x3dU, 0U);
  output = feed(&parser, 0x00U, 1U);
  assert(output.events[0].message.status == 0x90U);
  assert(output.events[0].message.data[0] == 0x3dU);

  // Program Change has only one data byte.
  feed(&parser, 0xc1U, 0U);
  output = feed(&parser, 0x22U, 1U);
  assert(output.events[0].message.length == 2U);
  assert(output.events[0].message.status == 0xc1U);

  output = feed(&parser, 0x23U, 1U);
  assert(output.events[0].message.data[0] == 0x23U);

  // System common cancels running status.
  output = feed(&parser, 0xf6U, 1U);
  assert(output.events[0].message.status == 0xf6U);
  feed(&parser, 0x11U, 0U);
}

static void test_stream_sysex(void) {
  midi_stream_parser_t parser = {0};
  midi_stream_output_t output = {0};

  output = feed(&parser, 0xf0U, 1U);
  assert(output.events[0].type == MIDI_STREAM_SYSEX_BEGIN);

  output = feed(&parser, 0x7dU, 1U);
  assert(output.events[0].type == MIDI_STREAM_SYSEX_DATA);
  assert(output.events[0].byte == 0x7dU);

  // Realtime is transparent within SysEx.
  output = feed(&parser, 0xf8U, 1U);
  assert(output.events[0].type == MIDI_STREAM_SHORT);
  assert(parser.in_sysex);

  feed(&parser, 0x01U, 1U);
  output = feed(&parser, 0xf7U, 1U);
  assert(output.events[0].type == MIDI_STREAM_SYSEX_END);
  assert(!parser.in_sysex);

  // Unexpected status aborts SysEx, but must
  // also be processed as the next MIDI status.
  feed(&parser, 0xf0U, 1U);
  feed(&parser, 0x02U, 1U);
  output = feed(&parser, 0x90U, 1U);
  assert(output.events[0].type == MIDI_STREAM_SYSEX_ABORT);
  assert(output.events[0].byte == 0x90U);

  feed(&parser, 0x3cU, 0U);
  output = feed(&parser, 0x40U, 1U);
  assert(output.events[0].type == MIDI_STREAM_SHORT);
  assert(output.events[0].message.status == 0x90U);

  // Explicit cancellation produces one abort.
  feed(&parser, 0xf0U, 1U);
  assert(midi_stream_parser_cancel(&parser, &output));
  assert(output.count == 1U);
  assert(output.events[0].type == MIDI_STREAM_SYSEX_ABORT);
  assert(output.events[0].byte == 0U);

  assert(midi_stream_parser_cancel(&parser, &output));
  assert(output.count == 0U);

  output = feed(&parser, 0xf7U, 1U);
  assert(output.events[0].type == MIDI_STREAM_SHORT);

  // A new F0 can both abort the old SysEx
  // and begin a new one in the same input byte.
  feed(&parser, 0xf0U, 1U);
  output = feed(&parser, 0xf0U, 2U);
  assert(output.events[0].type == MIDI_STREAM_SYSEX_ABORT);
  assert(output.events[1].type == MIDI_STREAM_SYSEX_BEGIN);

  assert(midi_stream_parser_cancel(&parser, &output));
}

static midi_usb_batch_t prepare(midi_usb_bridge_t *bridge,
    usb_midi_event_t packet, midi_stream_parser_t *next,
    unsigned int expected) {

  midi_usb_batch_t batch = {0};

  assert(midi_usb_bridge_prepare(
      bridge, &packet, next, &batch));
  assert(batch.count == expected);
  assert(batch.cable == packet.cable);

  return batch;
}

static void test_bridge(void) {
  midi_usb_bridge_t bridge = {0};
  midi_stream_parser_t next = {0};
  midi_usb_batch_t batch = {0};

  assert(!midi_usb_bridge_init(&bridge, 0U));
  assert(midi_usb_bridge_init(&bridge, 2U));

  usb_midi_event_t first = {
    .cable = 1U,
    .cin = 4U,
    .length = 3U,
    .data = {0xf0U, 0x7dU, 0x01U}
  };

  batch = prepare(&bridge, first, &next, 3U);
  assert(batch.events[0].type == MIDI_STREAM_SYSEX_BEGIN);
  assert(batch.events[1].type == MIDI_STREAM_SYSEX_DATA);
  assert(batch.events[1].byte == 0x7dU);
  assert(batch.events[2].type == MIDI_STREAM_SYSEX_DATA);

  // Prepare must not mutate live parser state.
  assert(next.in_sysex);
  assert(!bridge.parsers[1].in_sysex);

  // If the consumer declines the batch, preparing
  // it again must produce the same events.
  batch = prepare(&bridge, first, &next, 3U);
  assert(midi_usb_bridge_commit(&bridge, &batch, &next));
  assert(bridge.parsers[1].in_sysex);
  assert(!bridge.parsers[0].in_sysex);

  // Reject inconsistent lengths and reserved CINs.
  usb_midi_event_t bad = first;
  bad.length = 2U;
  assert(!midi_usb_bridge_prepare(
      &bridge, &bad, &next, &batch));
  assert(bridge.parsers[1].in_sysex);

  bad = (usb_midi_event_t){
    .cable = 1U,
    .cin = 0U,
    .length = 3U,
    .data = {0x90U, 0x3cU, 0x40U}
  };
  assert(!midi_usb_bridge_prepare(
      &bridge, &bad, &next, &batch));

  // MIDI data bytes must be seven-bit.
  bad = (usb_midi_event_t){
    .cable = 1U,
    .cin = 9U,
    .length = 3U,
    .data = {0x90U, 0x80U, 0x40U}
  };
  assert(!midi_usb_bridge_prepare(
      &bridge, &bad, &next, &batch));

  // Another cable has independent parser state.
  usb_midi_event_t note = {
    .cable = 0U,
    .cin = 9U,
    .length = 3U,
    .data = {0x90U, 0x3cU, 0x40U}
  };

  batch = prepare(&bridge, note, &next, 1U);
  assert(batch.events[0].type == MIDI_STREAM_SHORT);
  assert(midi_usb_bridge_commit(&bridge, &batch, &next));
  assert(bridge.parsers[1].in_sysex);

  // Realtime within the open SysEx.
  usb_midi_event_t realtime = {
    .cable = 1U,
    .cin = 15U,
    .length = 1U,
    .data = {0xf8U, 0U, 0U}
  };

  batch = prepare(&bridge, realtime, &next, 1U);
  assert(batch.events[0].message.status == 0xf8U);
  assert(midi_usb_bridge_commit(&bridge, &batch, &next));
  assert(bridge.parsers[1].in_sysex);

  // CIN 5: one-byte SysEx ending.
  usb_midi_event_t ending = {
    .cable = 1U,
    .cin = 5U,
    .length = 1U,
    .data = {0xf7U, 0U, 0U}
  };

  batch = prepare(&bridge, ending, &next, 1U);
  assert(batch.events[0].type == MIDI_STREAM_SYSEX_END);
  assert(midi_usb_bridge_commit(&bridge, &batch, &next));
  assert(!bridge.parsers[1].in_sysex);

  // CIN 7: F0, one payload byte, F7.
  usb_midi_event_t single = {
    .cable = 1U,
    .cin = 7U,
    .length = 3U,
    .data = {0xf0U, 0x7dU, 0xf7U}
  };

  batch = prepare(&bridge, single, &next, 3U);
  assert(batch.events[0].type == MIDI_STREAM_SYSEX_BEGIN);
  assert(batch.events[1].type == MIDI_STREAM_SYSEX_DATA);
  assert(batch.events[2].type == MIDI_STREAM_SYSEX_END);
  assert(midi_usb_bridge_commit(&bridge, &batch, &next));

  // CIN 6: empty SysEx, F0 F7.
  usb_midi_event_t empty = {
    .cable = 1U,
    .cin = 6U,
    .length = 2U,
    .data = {0xf0U, 0xf7U, 0U}
  };

  batch = prepare(&bridge, empty, &next, 2U);
  assert(batch.events[0].type == MIDI_STREAM_SYSEX_BEGIN);
  assert(batch.events[1].type == MIDI_STREAM_SYSEX_END);
  assert(midi_usb_bridge_commit(&bridge, &batch, &next));

  // Repeated F0 must generate an abort and begin,
  // even when contained in a USB-MIDI fragment.
  batch = prepare(&bridge, first, &next, 3U);
  assert(midi_usb_bridge_commit(&bridge, &batch, &next));

  batch = prepare(&bridge, first, &next, 4U);
  assert(batch.events[0].type == MIDI_STREAM_SYSEX_ABORT);
  assert(batch.events[1].type == MIDI_STREAM_SYSEX_BEGIN);
  assert(midi_usb_bridge_commit(&bridge, &batch, &next));

  // Cancellation affects the selected cable only.
  assert(midi_usb_bridge_cancel(&bridge, 1U, &batch));
  assert(batch.count == 1U);
  assert(batch.events[0].type == MIDI_STREAM_SYSEX_ABORT);
  assert(!bridge.parsers[1].in_sysex);

  assert(midi_usb_bridge_cancel(&bridge, 1U, &batch));
  assert(batch.count == 0U);
}

int main(void) {
  test_stream_messages();
  test_stream_sysex();
  test_bridge();

  puts("MIDI stream and USB bridge: PASS");
  return 0;
}
