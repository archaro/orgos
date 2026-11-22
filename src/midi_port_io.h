#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "midi_stream.h"
#include "midi_status.h"
#include "midi_ports.h"

// Foreground-only application boundary. Registry and transport APIs retain
// their existing Boolean interfaces during this incremental migration.
// INVALID_ARGUMENT: null argument, invalid ID, direction or MIDI payload.
// NOT_DEFINED: valid ID without a logical-port definition.
// AMBIGUOUS: multiple devices match the logical port.
// DISCONNECTED: no binding or the pinned device generation is gone.
// NOT_OPEN: bound port/service or SysEx handle has not been opened.
// BUSY: an active SysEx session prevents the operation.
// QUEUE_FULL: output backpressure; retry the unaccepted byte/operation.
// FAILED: service failure or an unclassified transport rejection.
// EMPTY: no input event or connection notification is available.
// Output structures are cleared before unsuccessful reads/queries.

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
midi_status_t midi_port_sysex_begin(unsigned int id, midi_port_sysex_t *session);

// Only seven-bit SysEx payload bytes are accepted.
// Non-OK leaves the supplied byte unconsumed.
midi_status_t midi_port_sysex_write(midi_port_sysex_t *session, uint8_t byte);

// Enqueue the final fragment, including F7.
// Backpressure leaves the handle active.
midi_status_t midi_port_sysex_end(midi_port_sysex_t *session);

// Abandon the unsent suffix. If earlier fragments
// were queued and the device is running, enqueue
// F7 on that original generation.
//
// QUEUE_FULL leaves the handle active for retry.
// OK also releases a handle whose original claim no longer exists.
midi_status_t midi_port_sysex_cancel(midi_port_sysex_t *session);

// Reconcile the application's logical-port state
// after the registry has inspected USB topology.
void midi_port_io_service(void);

// Remove the oldest pending connection transition.
midi_status_t midi_port_next_change(unsigned int id, midi_port_change_t *change);

// Inspect outstanding events and loss counters.
midi_status_t midi_port_lifecycle_get_info(unsigned int id,
    midi_port_lifecycle_info_t *info);

// Ensure that the service behind a bound logical
// port is running. Idempotent for an open service.
// Call from foreground context, not an interrupt.
midi_status_t midi_port_open(unsigned int id);

// Close one logical port without stopping the
// underlying USB-MIDI device.
//
// Input closure releases its selected cable.
// Output closure revokes permission to enqueue.
//
// An active output SysEx session must be finished
// or cancelled before closing its port.
midi_status_t midi_port_close(unsigned int id);

// Close and remove a logical-port definition.
//
// Refuses removal if an active output SysEx
// session cannot be released safely.
midi_status_t midi_port_undefine(unsigned int id);

// Nonblocking: success means accepted for sending,
// not delivered to the external MIDI receiver.
midi_status_t midi_port_send_short(unsigned int id,
    const midi_short_message_t *message);

// Read one event belonging to the logical IN port.
// Other virtual cables retain their queued events.
// Retired events return OK before DISCONNECTED/NOT_OPEN. An open,
// attached input with no event returns EMPTY.
midi_status_t midi_port_read(unsigned int id, midi_port_event_t *event);
