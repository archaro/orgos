
#include <stdbool.h>
#include <stdint.h>

#include "usb_midi.h"

static uint16_t read_u16(const uint8_t *p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

bool usb_midi_out_packet(const usb_midi_config_t *config, unsigned int cable,
    unsigned int cin, const uint8_t message[3], uint8_t packet[4]) {
  if (!config || !message || !packet || !config->out_found ||
      config->bulk_out_jacks == 0U ||
      config->bulk_out_jacks > USB_MIDI_MAX_CABLES ||
      cable >= config->bulk_out_jacks || cin > 0x0fU)
    return false;

  // USB-MIDI 1.0 packet header:
  // bits 7..4 = virtual cable number
  // bits 3..0 = Code Index Number (CIN)
  packet[0] = (uint8_t)((cable << 4U) | cin);
  packet[1] = message[0];
  packet[2] = message[1];
  packet[3] = message[2];

  return true;
}

bool usb_midi_decode_packet(const uint8_t packet[4],
    usb_midi_event_t *event) {

  // USB-MIDI 1.0, Table 4-1:
  // valid MIDI data bytes for each CIN.
  // CIN 0 and 1 are reserved.
  static const uint8_t cin_lengths[16] = {
    0U, 0U, 2U, 3U,
    3U, 1U, 2U, 3U,
    3U, 3U, 3U, 3U,
    2U, 2U, 3U, 1U
  };

  if (!event)
    return false;

  *event = (usb_midi_event_t){0};

  if (!packet)
    return false;

  uint8_t cable = packet[0] >> 4U;
  uint8_t cin = packet[0] & 0x0fU;

  uint8_t length = cin_lengths[cin];
  if (length == 0U)
    return false;

  event->cable = cable;
  event->cin = cin;
  event->length = length;
  for (uint8_t i = 0U; i < length; i++)
    event->data[i] = packet[i + 1U];

  return true;
}

bool usb_midi_parse_config(const uint8_t *data,
    unsigned int length, usb_midi_config_t *result) {

  if (!data || !result || length < 9U)
    return false;

  *result = (usb_midi_config_t){0};

  if (data[0] != 9U || data[1] != 0x02U ||
      read_u16(&data[2]) != length)
    return false;

  result->configuration = data[5];
  bool in_midi_interface = false;
  bool pending_bulk_endpoint = false;
  bool pending_in = false;
  unsigned int offset = 0U;
  while (offset < length) {
    if (length - offset < 2U)
      return false;
    const uint8_t *d = &data[offset];
    uint8_t size = d[0];
    uint8_t type = d[1];
    if (size < 2U || size > length - offset)
      return false;
    if (type == 0x04U) {
      // Standard Interface Descriptor.
      if (size < 9U)
        return false;
      pending_bulk_endpoint = false;
      in_midi_interface = d[5] == 0x01U && d[6] == 0x03U;
      if (in_midi_interface) {
        if (result->midi_found)
          return false;
        result->midi_found = true;
        result->midi_interface = d[2];
        result->midi_alternate = d[3];
      }
    } else if (type == 0x05U) {
      // Standard Endpoint Descriptor.
      if (size < 7U)
        return false;
      pending_bulk_endpoint = false;
      if (in_midi_interface && (d[3] & 0x03U) == 2U) {
        uint8_t address = d[2];
        uint16_t mps = read_u16(&d[4]) & 0x07ffU;
        bool input = (address & 0x80U) != 0U;
        if ((address & 0x0fU) == 0U || mps == 0U)
          return false;
        if (input) {
          if (result->in_found)
            return false;
          result->bulk_in = address;
          result->bulk_in_mps = mps;
          result->in_found = true;
        } else {
          if (result->out_found)
            return false;
          result->bulk_out = address;
          result->bulk_out_mps = mps;
          result->out_found = true;
        }
        pending_bulk_endpoint = true;
        pending_in = input;
      }
    } else if (type == 0x24U && in_midi_interface) {
      // Class-specific MIDI interface descriptor.
      if (size < 3U)
        return false;
      if (d[2] == 0x01U) {
        // MIDI Streaming header.
        if (size < 7U)
          return false;
      } else if (d[2] == 0x02U) {
        // MIDI IN Jack Descriptor.
        if (size < 6U)
          return false;
      } else if (d[2] == 0x03U) {
        // MIDI OUT Jack Descriptor.
        if (size < 7U || size < 7U + 2U * (unsigned int)d[5])
          return false;
      }
    } else if (type == 0x25U && in_midi_interface) {
      // Class-specific endpoint jack associations.
      if (size < 4U || d[2] != 0x01U || size < 4U + (unsigned int)d[3])
        return false;
      if (pending_bulk_endpoint) {
        if (pending_in)
          result->bulk_in_jacks = d[3];
        else
          result->bulk_out_jacks = d[3];
        pending_bulk_endpoint = false;
      }
    }
    offset += size;
  }

  return result->midi_found && result->in_found &&
      result->out_found;
}

