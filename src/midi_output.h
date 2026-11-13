#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "midi_stream.h"

#define MIDI_OUTPUT_QUEUE_CAPACITY 64U
#define MIDI_OUTPUT_BATCH_PACKETS 16U

typedef enum {
  MIDI_OUTPUT_IDLE,
  MIDI_OUTPUT_RUNNING,
  MIDI_OUTPUT_STOPPED,
  MIDI_OUTPUT_DISCONNECTED,
  MIDI_OUTPUT_FAILED
} midi_output_state_t;

typedef struct {
  midi_output_state_t state;
  uint32_t generation;
  uint8_t slot_id;
  uint8_t cables;
  uint64_t accepted;
  uint64_t confirmed;
  uint64_t transfers;
  uint64_t failures;
  uint64_t full_rejections;
  unsigned int queue_depth;
  unsigned int queue_peak;
  unsigned int in_flight;
  uint32_t last_error;
  bool sysex_active;
  uint8_t sysex_cable;
  uint8_t sysex_staged;
  uint64_t sysex_started;
  uint64_t sysex_completed;
  uint64_t sysex_payload_bytes;
} midi_output_info_t;

bool midi_output_start_generation(uint32_t generation);
bool midi_output_stop_generation(uint32_t generation);
bool midi_output_owns_generation(uint32_t generation);

// Nonblocking. True means queued, NOT delivered.
bool midi_output_enqueue_short(uint32_t generation,
    unsigned int cable, const midi_short_message_t *message);

// Begin a SysEx stream on one virtual cable.
// F0 is staged; it may not yet be transmitted.
bool midi_output_sysex_begin(uint32_t generation, unsigned int cable);

// Accept one seven-bit SysEx payload byte.
// False leaves the byte unconsumed.
bool midi_output_sysex_write(uint32_t generation, uint8_t byte);

// Enqueue the final fragment, including F7.
// False leaves the stream active for retry.
bool midi_output_sysex_end(uint32_t generation);

bool midi_output_get_info(uint32_t generation, midi_output_info_t *info);

void midi_output_service(void);
