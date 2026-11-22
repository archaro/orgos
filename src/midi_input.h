#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "midi.h"

// Capacity per virtual cable, not a shared device-wide budget.
#define MIDI_INPUT_QUEUE_CAPACITY 64U
#define MIDI_INPUT_MAX_CABLES 16U

typedef struct {
  bool data_lost; // Sticky until explicit cable close/reopen (or diagnostic restart).
  uint64_t overflow_packets; // Packets rejected at the FIFO boundary.
  uint64_t dropped_packets; // Overflow packets plus subsequent unsynchronised packets.
  unsigned int queue_depth;
  unsigned int queue_peak;
  bool pending_abort;
} midi_input_cable_info_t;

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
  bool port_filtered;
  uint16_t open_cables;
  uint64_t unclaimed_packets;
  uint64_t closed_events;
  uint64_t packets;
  uint64_t malformed;
  uint64_t short_messages;
  uint64_t sysex_begins;
  uint64_t sysex_bytes;
  uint64_t sysex_ends;
  uint64_t sysex_aborts;
  uint64_t stalls; // Legacy aggregate: cable FIFO loss episodes.

  unsigned int queue_depth;
  unsigned int queue_peak;
  unsigned int pending_aborts;

  bool have_last;
  uint8_t last_cable;
  midi_stream_event_t last;
  // Generation-lifetime loss counters; closing a cable preserves counters.
  // queue_depth/queue_peak above are aggregate device-wide event counts.
  midi_input_cable_info_t cable[MIDI_INPUT_MAX_CABLES];
} midi_input_info_t;

// Explicit all-cable diagnostic ownership. Recovery of its cable faults
// requires explicit stop, draining retained events/aborts, then restart.
bool midi_input_start_generation(uint32_t generation);
// Attach input for one logical MIDI port.
// An explicitly started input parser continues
// accepting all cables; an input opened through
// logical ports accepts only selected cables.
bool midi_input_open_port_cable(uint32_t generation, unsigned int cable);

// Release one cable opened through a logical port.
// Other opened cables retain their parser and FIFO.
// Closing the last cable releases the subscription.
bool midi_input_close_port_cable(uint32_t generation, unsigned int cable);

bool midi_input_stop_generation(uint32_t generation);

bool midi_input_get_info(uint32_t generation, midi_input_info_t *info);

// Read accepted stream events in original packet order.
// Draining remains possible after stop or disconnect,
// until the managed record is reused for a new generation.
bool midi_input_read(uint32_t generation, midi_input_event_t *event);

// Read the earliest event on one virtual cable,
// leaving events for other cables in FIFO order.
//
// Every cable has an independent FIFO. The device-wide reader merges
// their heads in acceptance order; these APIs share one consumer.
bool midi_input_read_cable(uint32_t generation,
    unsigned int cable, midi_input_event_t *event);

void midi_input_service(void);

// Exercise cable-local overflow, ordering and atomic packet acceptance
// without USB hardware or modification of live input records.
bool midi_input_backpressure_selftest(void);
