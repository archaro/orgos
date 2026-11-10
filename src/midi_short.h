#pragma once

#include <stdbool.h>
#include <stdint.h>

// A complete MIDI message of one to three bytes.
// Length includes the status byte.
typedef struct {
  uint8_t status;
  uint8_t data[2];
  uint8_t length;
} midi_short_message_t;

// Parser state for one MIDI byte stream.
//
// Never share one parser between independent
// input ports or virtual MIDI cables.
typedef struct {
  uint8_t running_status;
  uint8_t active_status;
  uint8_t needed;
  uint8_t collected;
  uint8_t data[2];
  bool in_sysex;
} midi_short_parser_t;

void midi_short_parser_reset(midi_short_parser_t *parser);

// Feed one byte.
//
// Returns true when a complete short message
// has been produced. At most one short message
// can be completed by each input byte.
//
// SysEx payloads are recognised and skipped
// until we introduce SysEx assembly.
bool midi_short_parser_feed(midi_short_parser_t *parser,
    uint8_t byte, midi_short_message_t *message);
