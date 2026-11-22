#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "midi_stream.h"
#include "midi_status.h"
#include "midi_ports.h"

// Application I/O entry point. Define logical IN and OUT ports through
// configuration, then discover their IDs with midi_port_next/get_info.
// Each definition has one direction; an application can open both an IN
// and an OUT definition for the same physical endpoint independently.
//
// Foreground-only application boundary. Registry and transport APIs retain
// their existing Boolean interfaces during this incremental migration.
// INVALID_ARGUMENT: null argument, invalid ID, direction or MIDI payload.
// NOT_DEFINED: valid ID without a logical-port definition.
// AMBIGUOUS: multiple devices match the logical port.
// DISCONNECTED: no binding, changed definition/generation or revoked open.
// NOT_OPEN: closed/zero handle, stopped service or inactive SysEx session.
// BUSY: another open or an active SysEx session prevents the operation.
// QUEUE_FULL: output backpressure; retry the unaccepted byte/operation.
// FAILED: service failure or an unclassified transport rejection.
// EMPTY: no input event or connection notification is available.
// DATA_LOST: cable FIFO overflow; drain accepted events, then close/reopen.
// Output structures are cleared before unsuccessful reads/queries.

// Caller-owned capability. Initialise to zero; treat fields as read-only.
// One active open per logical port. Copies alias the same permission:
// closing any copy revokes all of them. Open tokens never repeat in a boot.
// An active handle is never retargeted by open, reconnect or redefinition.
// Close it explicitly before reusing its storage for a new open.
typedef struct {
  bool active;
  unsigned int port;
  uint32_t definition_serial;
  uint32_t generation;
  uint32_t open_token;
  uint8_t cable;
  midi_port_direction_t direction;
} midi_port_handle_t;

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
  uint32_t open_token;
} midi_port_sysex_t;

// A successful inspection returns OK; status describes permission/service
// validity. Writes can still return BUSY or QUEUE_FULL. Identity always describes the supplied handle, never a replacement.
// Stale handles may still have retired input events to drain.
// Service counters are shared across cables, not per logical port. They
// are reported only for a current handle, so a stale open cannot inspect
// a replacement open even when it uses the same device generation.
typedef struct {
  midi_port_handle_t handle;
  midi_status_t status;
  unsigned int retired_depth;
  bool service_present;
  unsigned int service_queue_depth;
  unsigned int service_queue_capacity;
  unsigned int service_pending_aborts;
  unsigned int service_in_flight;
  bool sysex_active;
  uint8_t sysex_cable;
  // Input-only, scoped to this open's cable. Counts exclude earlier opens;
  // lower-level input statistics retain generation-lifetime loss totals.
  unsigned int cable_queue_depth;
  unsigned int cable_queue_capacity;
  unsigned int cable_queue_peak;
  bool cable_data_lost;
  uint64_t cable_overflow_packets;
  uint64_t cable_dropped_packets;
} midi_port_handle_info_t;

// Session identity remains pinned after end, cancellation or disconnect.
// status describes permission/service validity for write/end; a write may
// still return QUEUE_FULL. claim_present means the original transport claim
// still exists for cancellation (which may itself need a queue-space retry),
// even when status is DISCONNECTED. It never describes a replacement claim.
typedef struct {
  midi_port_sysex_t session;
  midi_status_t status;
  bool claim_present;
  unsigned int staged;
  unsigned int fragments;
} midi_port_sysex_info_t;

// Inspect a definition without acquiring permission or starting a service.
// OK includes unbound/ambiguous definitions; inspect info.state.
midi_status_t midi_port_get_info(unsigned int id, midi_port_info_t *info);

// Enumerate defined IDs in ascending order, including unbound/ambiguous
// ports. Initialise *cursor to zero. OK advances it to info.id; EMPTY sets
// it to MIDI_PORT_CAPACITY. Other errors leave it unchanged. Reset to zero
// to restart. This is a live view, not a frozen topology snapshot.
midi_status_t midi_port_next(unsigned int *cursor, midi_port_info_t *info);

// Inspection never starts services or consumes events. Both return OK for
// a successfully inspected closed/stale handle/session; check info.status.
midi_status_t midi_port_get_handle_info(const midi_port_handle_t *handle,
    midi_port_handle_info_t *info);
midi_status_t midi_port_sysex_get_info(const midi_port_sysex_t *session,
    midi_port_sysex_info_t *info);

// The caller owns this handle and must initialise
// it to zero before its first use.
//
// A successful begin pins the current generation,
// cable, logical-port definition and open permission.
// Only an OUT handle can begin a session. Subsequent calls use the pinned
// session capability and return the same statuses as ordinary handle I/O.
midi_status_t midi_port_sysex_begin(const midi_port_handle_t *handle,
    midi_port_sysex_t *session);

// Only seven-bit SysEx payload bytes are accepted.
// Non-OK leaves the supplied byte unconsumed.
midi_status_t midi_port_sysex_write(midi_port_sysex_t *session, uint8_t byte);

// Accept a prefix of a seven-bit payload without appending F7. *accepted
// is always set (when non-null), including on errors. QUEUE_FULL leaves
// the suffix unconsumed; retry at bytes + *accepted. Validate the entire
// payload first: INVALID_ARGUMENT never consumes a prefix. A zero-length
// payload permits NULL bytes but still checks the session's availability.
midi_status_t midi_port_sysex_write_bytes(midi_port_sysex_t *session,
    const uint8_t *bytes, size_t count, size_t *accepted);

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

// Start/reuse the underlying service and acquire permission for exactly
// this definition and generation. Another active open returns BUSY.
// Repeating open on the same current handle is idempotent. A stale active
// handle returns DISCONNECTED without changing it or starting a service.
// Failed opens leave the supplied handle unchanged.
midi_status_t midi_port_open(unsigned int id, midi_port_handle_t *handle);

// Close one logical port without stopping the
// underlying USB-MIDI device.
//
// Input closure releases its selected cable.
// Output closure revokes permission to enqueue.
//
// An active output SysEx session must be finished
// or cancelled before closing its port.
// Closing a stale handle releases only that handle and its retired events;
// it cannot close a replacement open. It returns OK even after undefine.
// Closed handles return NOT_OPEN. Identity fields are retained on close.
midi_status_t midi_port_close(midi_port_handle_t *handle);

// Close and remove a logical-port definition.
//
// Refuses removal if an active output SysEx
// session cannot be released safely.
midi_status_t midi_port_undefine(unsigned int id);

// Write one complete short message through an OUT handle. Nonblocking:
// OK means accepted for sending, not delivered to the external receiver.
// Use the SysEx session operations for streamed payloads; input SysEx is
// returned as BEGIN/DATA/END/ABORT events by midi_port_read.
midi_status_t midi_port_write(const midi_port_handle_t *handle,
    const midi_short_message_t *message);

// Compatibility spelling for midi_port_write; identical status semantics.
midi_status_t midi_port_send_short(const midi_port_handle_t *handle,
    const midi_short_message_t *message);

// Read one event belonging to the logical IN port.
// Other virtual cables retain their queued events.
// A handle can drain its own retired events before DISCONNECTED, even
// while a replacement is open. It never consumes another open's events.
// An open, attached input with no event returns EMPTY, or DATA_LOST once
// its accepted events/terminal ABORT have drained after FIFO overflow.
// Repeating open alone cannot reset this fault; explicitly close/reopen.
// Disconnection/stale permission takes precedence after retired events drain.
midi_status_t midi_port_read(const midi_port_handle_t *handle,
    midi_port_event_t *event);
