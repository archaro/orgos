#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "midi_stream.h"

#define MIDI_INPUT_QUEUE_CAPACITY 64U

typedef struct {
  uint32_t generation;
  uint8_t cable;
  midi_stream_event_t event;
} midi_input_event_t;

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
  uint64_t stalls;

  unsigned int queue_depth;
  unsigned int queue_peak;
  unsigned int pending_aborts;

  bool have_last;
  uint8_t last_cable;
  midi_stream_event_t last;
} midi_input_info_t;

bool midi_input_start_generation(uint32_t generation);
bool midi_input_stop_generation(uint32_t generation);

bool midi_input_get_info(uint32_t generation, midi_input_info_t *info);

// Read accepted stream events in original packet order.
// Draining remains possible after stop or disconnect,
// until the managed record is reused for a new generation.
bool midi_input_read(uint32_t generation, midi_input_event_t *event);

void midi_input_service(void);
