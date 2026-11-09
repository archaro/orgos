#pragma once

#include <stdbool.h>
#include <stdint.h>

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

// Encode one USB-MIDI 1.0 event packet for a
// device's advertised Bulk OUT virtual cable.
//
// The caller supplies the correct CIN and three
// MIDI payload bytes, including any required
// zero padding.
bool usb_midi_out_packet(const usb_midi_config_t *config, unsigned int cable,
    unsigned int cin, const uint8_t message[3], uint8_t packet[4]);

bool usb_midi_parse_config(const uint8_t *data,
    unsigned int length, usb_midi_config_t *result);

