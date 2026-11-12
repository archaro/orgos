#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "midi_stream.h"

typedef struct {
  uint32_t generation;
  uint8_t cables;
  bool attached;
  bool lost;

  uint64_t packets;
  uint64_t malformed;
  uint64_t short_messages;
  uint64_t sysex_begins;
  uint64_t sysex_bytes;
  uint64_t sysex_ends;
  uint64_t sysex_aborts;

  bool have_last;
  uint8_t last_cable;
  midi_stream_event_t last;
} midi_input_info_t;

// Claim an already-running persistent USB-MIDI
// receiver without modifying its hardware state.
bool midi_input_start_generation(uint32_t generation);

bool midi_input_stop_generation(uint32_t generation);

bool midi_input_get_info(uint32_t generation, midi_input_info_t *info);

// Cancel parser state when a device incarnation
// is no longer eligible to deliver MIDI.
void midi_input_service(void);
