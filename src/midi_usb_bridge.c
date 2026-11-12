#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "midi_usb_bridge.h"

static bool system_single(uint8_t byte) {
  return byte == 0xf6U || byte == 0xf7U || byte == 0xf8U || byte == 0xfaU ||
      byte == 0xfbU || byte == 0xfcU || byte == 0xfeU || byte == 0xffU;
}

static bool valid_packet(const midi_usb_bridge_t *bridge,
    const usb_midi_event_t *packet) {

  static const uint8_t lengths[16] = {
    0U, 0U, 2U, 3U, 3U, 1U, 2U, 3U,
    3U, 3U, 3U, 3U, 2U, 2U, 3U, 1U
  };

  if (!bridge || !packet || packet->cable >= bridge->cables ||
      packet->cin >= 16U || packet->length != lengths[packet->cin] ||
      packet->length == 0U)
    return false;

  const uint8_t *d = packet->data;
  bool sysex = bridge->parsers[packet->cable].in_sysex;
  switch (packet->cin) {
    case 0x2U:
      return (d[0] == 0xf1U || d[0] == 0xf3U) && d[1] < 0x80U;
    case 0x3U:
      return d[0] == 0xf2U && d[1] < 0x80U && d[2] < 0x80U;
    case 0x4U:
      return (d[0] == 0xf0U || (sysex && d[0] < 0x80U)) &&
          d[1] < 0x80U && d[2] < 0x80U;
    case 0x5U:
      return system_single(d[0]);
    case 0x6U:
      return (d[0] == 0xf0U || (sysex && d[0] < 0x80U)) && d[1] == 0xf7U;
    case 0x7U:
      return (d[0] == 0xf0U || (sysex && d[0] < 0x80U)) &&
          d[1] < 0x80U && d[2] == 0xf7U;
    case 0xfU:
      // CIN F transports one unparsed MIDI byte.
      // The stream parser interprets that byte.
      return true;
    default:
      // Parsed Channel messages must agree with
      // their CIN and contain seven-bit data.
      return packet->cin >= 0x8U && packet->cin <= 0xeU &&
          (d[0] >> 4U) == packet->cin && d[1] < 0x80U &&
          (packet->length == 2U || d[2] < 0x80U);
  }
}

bool midi_usb_bridge_init(midi_usb_bridge_t *bridge, unsigned int cables) {
  if (!bridge || cables == 0U || cables > USB_MIDI_MAX_CABLES)
    return false;

  *bridge = (midi_usb_bridge_t){0};
  bridge->cables = (uint8_t)cables;
  return true;
}

bool midi_usb_bridge_prepare(const midi_usb_bridge_t *bridge,
    const usb_midi_event_t *packet, midi_stream_parser_t *next,
    midi_usb_batch_t *batch) {

  if (!next || !batch)
    return false;

  *next = (midi_stream_parser_t){0};
  *batch = (midi_usb_batch_t){0};

  if (!valid_packet(bridge, packet))
    return false;

  // Parse against a copy. A subsequent consumer
  // may decline the batch without advancing
  // the live MIDI stream.
  *next = bridge->parsers[packet->cable];
  batch->cable = packet->cable;

  for (unsigned int i = 0U; i < packet->length; i++) {
    midi_stream_output_t output = {0};

    if (!midi_stream_parser_feed(next, packet->data[i], &output) ||
        batch->count + output.count > MIDI_USB_BRIDGE_MAX_EVENTS)
      return false;

    for (unsigned int j = 0U; j < output.count; j++)
      batch->events[batch->count++] = output.events[j];
  }

  return true;
}

bool midi_usb_bridge_commit(midi_usb_bridge_t *bridge,
    const midi_usb_batch_t *batch,
    const midi_stream_parser_t *next) {

  if (!bridge || !batch || !next ||
      batch->cable >= bridge->cables ||
      batch->count > MIDI_USB_BRIDGE_MAX_EVENTS)
    return false;

  bridge->parsers[batch->cable] = *next;
  return true;
}

bool midi_usb_bridge_cancel(midi_usb_bridge_t *bridge,
    unsigned int cable, midi_usb_batch_t *batch) {

  if (!bridge || !batch || cable >= bridge->cables)
    return false;

  *batch = (midi_usb_batch_t){
    .cable = (uint8_t)cable
  };

  midi_stream_output_t output = {0};
  if (!midi_stream_parser_cancel(&bridge->parsers[cable], &output))
    return false;

  for (unsigned int i = 0U; i < output.count; i++)
    batch->events[batch->count++] = output.events[i];

  return true;
}
