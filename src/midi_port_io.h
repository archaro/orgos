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
