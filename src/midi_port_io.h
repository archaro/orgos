#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "midi_stream.h"

typedef struct {
  unsigned int port;
  uint32_t generation;
  midi_stream_event_t event;
} midi_port_event_t;

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
