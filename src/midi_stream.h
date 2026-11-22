#pragma once

#include "midi.h"

// Internal byte-stream parser; applications use midi.h.
// One input byte can abort an unfinished SysEx
// and also start another message.
#define MIDI_STREAM_EVENTS_PER_BYTE 2U

typedef struct {
  uint8_t count;
  midi_stream_event_t events[MIDI_STREAM_EVENTS_PER_BYTE];
} midi_stream_output_t;

// State belongs to exactly one MIDI byte stream.
// Never share it between independent inputs.
typedef struct {
  uint8_t running_status;
  uint8_t active_status;
  uint8_t needed;
  uint8_t collected;
  uint8_t data[2];
  bool in_sysex;
} midi_stream_parser_t;

// Silent initialisation or reset.
void midi_stream_parser_reset(midi_stream_parser_t *parser);

// Cancel an incomplete stream. Produces a SysEx
// abort event if SysEx was in progress.
bool midi_stream_parser_cancel(midi_stream_parser_t *parser,
    midi_stream_output_t *output);

// Consume one byte and return zero, one or two
// ordered events. A true return means the input
// was processed, not necessarily that an event
// was generated.
//
// The caller must retain all returned events
// before feeding the next byte.
bool midi_stream_parser_feed(midi_stream_parser_t *parser,
    uint8_t byte, midi_stream_output_t *output);

