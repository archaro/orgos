#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "midi_stream.h"
#include "midi_ports.h"

typedef struct {
  unsigned int port;
  uint32_t generation;
  midi_stream_event_t event;
} midi_port_event_t;

// A transition between two observed bindings.
// Generation zero means there was no bound device.
typedef struct {
  unsigned int port;
  midi_port_state_t previous;
  midi_port_state_t current;
  uint32_t previous_generation;
  uint32_t current_generation;
} midi_port_change_t;

typedef struct {
  uint32_t opened_generation;
  unsigned int retired_depth;
  uint64_t retired_dropped;
  uint64_t retirement_missed;
  unsigned int pending_changes;
  uint64_t changes_dropped;
} midi_port_lifecycle_info_t;

typedef struct {
  bool active;
  unsigned int port;
  uint32_t definition_serial;
  uint32_t generation;
  uint8_t cable;
  uint32_t claim;
} midi_port_sysex_t;

// The caller owns this handle and must initialise
// it to zero before its first use.
//
// A successful begin pins the current generation,
// cable and logical-port definition.
bool midi_port_sysex_begin(unsigned int id, midi_port_sysex_t *session);

// Only seven-bit SysEx payload bytes are accepted.
// False leaves the supplied byte unconsumed.
bool midi_port_sysex_write(midi_port_sysex_t *session, uint8_t byte);

// Enqueue the final fragment, including F7.
// Backpressure leaves the handle active.
bool midi_port_sysex_end(midi_port_sysex_t *session);

// Abandon the unsent suffix. If earlier fragments
// were queued and the device is running, enqueue
// F7 on that original generation.
//
// False with an active handle means retry is needed.
bool midi_port_sysex_cancel(midi_port_sysex_t *session);

// Reconcile the application's logical-port state
// after the registry has inspected USB topology.
void midi_port_io_service(void);

// Remove the oldest pending connection transition.
bool midi_port_next_change(unsigned int id, midi_port_change_t *change);

// Inspect outstanding events and loss counters.
bool midi_port_lifecycle_get_info(unsigned int id,
    midi_port_lifecycle_info_t *info);

// Ensure that the service behind a bound logical
// port is running. Idempotent for an open service.
// Call from foreground context, not an interrupt.
bool midi_port_open(unsigned int id);

// Nonblocking: success means accepted for sending,
// not delivered to the external MIDI receiver.
bool midi_port_send_short(unsigned int id,
    const midi_short_message_t *message);

// Read one event belonging to the logical IN port.
// Other virtual cables retain their queued events.
bool midi_port_read(unsigned int id, midi_port_event_t *event);
