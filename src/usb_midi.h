#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "midi_stream.h"

typedef struct {
  uint8_t configuration;
  uint8_t midi_interface;
  uint8_t midi_alternate;
  uint8_t bulk_in;
  uint8_t bulk_out;
  uint8_t bulk_in_jacks;
  uint8_t bulk_out_jacks;
  uint16_t bulk_in_mps;
  uint16_t bulk_out_mps;
  bool midi_found;
  bool in_found;
  bool out_found;
} usb_midi_config_t;

#define USB_MIDI_MAX_CABLES 16U

// One decoded USB-MIDI 1.0 event packet.
//
// The length field identifies the valid bytes
// in data[]. Unused entries are zeroed.
//
// CIN is retained because it distinguishes
// complete messages from SysEx fragments.
typedef struct {
  uint8_t cable;
  uint8_t cin;
  uint8_t length;
  uint8_t data[3];
} usb_midi_event_t;

// Decode one four-byte USB-MIDI event packet.
//
// Reserved CIN values are rejected. This does
// not validate MIDI message semantics or assemble
// multi-packet SysEx messages.
bool usb_midi_decode_packet(const uint8_t packet[4], usb_midi_event_t *event);

// Encode one USB-MIDI 1.0 event packet for a
// device's advertised Bulk OUT virtual cable.
//
// The caller supplies the correct CIN and three
// MIDI payload bytes, including any required
// zero padding.
bool usb_midi_out_packet(const usb_midi_config_t *config, unsigned int cable,
    unsigned int cin, const uint8_t message[3], uint8_t packet[4]);

// Encode a complete MIDI short message into a
// USB-MIDI 1.0 event packet.
//
// Rejects SysEx, invalid statuses, incorrect lengths
// and non-seven-bit data bytes.
bool usb_midi_short_packet(const usb_midi_config_t *config, unsigned int cable,
    const midi_short_message_t *message, uint8_t packet[4]);

bool usb_midi_parse_config(const uint8_t *data,
    unsigned int length, usb_midi_config_t *result);

