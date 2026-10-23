
#include <stdbool.h>
#include <stdint.h>

#include "usb_midi.h"

static uint16_t read_u16(const uint8_t *p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
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

