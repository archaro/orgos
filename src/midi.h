#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Canonical application MIDI interface. Discover provisioned logical ports
// with midi_port_next/get_info. Each definition has one direction; an
// application may independently open IN and OUT definitions for one device.
// Provisioning and binding reconciliation are owned by the system.
//
// Context and progress: calls must be serialised in the foreground context
// that runs the normal USB/MIDI service loop. Do not call from interrupts or
// transport callbacks, or concurrently from multiple threads/cores. Calls are
// nonblocking: they do not wait for hardware or run the service loop. Keep
// normal foreground servicing running between calls so queued I/O progresses.
//
// Storage and lifetime: zero-initialise handles and SysEx sessions before first
// use, and treat their identity fields as read-only. Retain them until closed
// or ended/cancelled; overwriting an active capability does not release it.
// Supplied messages/payload bytes are copied before return; no caller pointers
// are retained. Returned events/inspection snapshots are caller-owned copies.
// A snapshot can become stale after foreground servicing; I/O rechecks validity.
//
// Results: compare explicitly with MIDI_STATUS_OK (never as a Boolean).
// OK: operation accepted locally, not confirmation of external delivery.
// EMPTY: no input event or connection notification is available.
// NOT_OPEN: closed/zero handle, stopped service or inactive SysEx session.
// DISCONNECTED: no binding, changed definition/generation or revoked open.
// BUSY: another open or active SysEx claim prevents the operation.
// QUEUE_FULL: output backpressure; retry the unaccepted byte/operation.
// FAILED: service failure or an unclassified transport rejection.
// INVALID_ARGUMENT: null argument, invalid ID, direction or MIDI payload.
// NOT_DEFINED: valid ID without a logical-port definition.
// AMBIGUOUS: multiple devices match the logical port.
// DATA_LOST: cable FIFO overflow; accepted events/ABORT remain readable, then
// reads return this terminal status. For application-owned input, explicit
// close/reopen starts a fresh session. Externally owned diagnostic input retains
// its explicit lifecycle; opening a logical port does not replace that owner.
// Read/query output structures are cleared on errors, except that successful
// handle/session inspection returns OK with validity in info.status.

// Application results: compare explicitly with MIDI_STATUS_OK.
// OK means accepted locally, never confirmation of external delivery.
typedef enum {
  MIDI_STATUS_OK,
  MIDI_STATUS_EMPTY,
  MIDI_STATUS_NOT_OPEN,
  MIDI_STATUS_DISCONNECTED,
  MIDI_STATUS_BUSY,
  MIDI_STATUS_QUEUE_FULL,
  MIDI_STATUS_FAILED,
  MIDI_STATUS_INVALID_ARGUMENT,
  MIDI_STATUS_NOT_DEFINED,
  MIDI_STATUS_AMBIGUOUS,
  MIDI_STATUS_DATA_LOST,
} midi_status_t;

static inline const char *midi_status_name(midi_status_t status) {
  switch (status) {
    case MIDI_STATUS_OK: return "OK";
    case MIDI_STATUS_EMPTY: return "EMPTY";
    case MIDI_STATUS_NOT_OPEN: return "NOT_OPEN";
    case MIDI_STATUS_DISCONNECTED: return "DISCONNECTED";
    case MIDI_STATUS_BUSY: return "BUSY";
    case MIDI_STATUS_QUEUE_FULL: return "QUEUE_FULL";
    case MIDI_STATUS_FAILED: return "FAILED";
    case MIDI_STATUS_INVALID_ARGUMENT: return "INVALID_ARGUMENT";
    case MIDI_STATUS_NOT_DEFINED: return "NOT_DEFINED";
    case MIDI_STATUS_AMBIGUOUS: return "AMBIGUOUS";
    case MIDI_STATUS_DATA_LOST: return "DATA_LOST";
    default: return "UNKNOWN";
  }
}

// A complete MIDI message of one to three bytes.
typedef struct {
  uint8_t status;
  uint8_t data[2];
  uint8_t length;
} midi_short_message_t;

typedef enum {
  MIDI_STREAM_SHORT,
  MIDI_STREAM_SYSEX_BEGIN,
  MIDI_STREAM_SYSEX_DATA,
  MIDI_STREAM_SYSEX_END,
  MIDI_STREAM_SYSEX_ABORT
} midi_stream_event_type_t;

// For SHORT, message contains the complete message.
// For SYSEX_DATA, byte contains one payload byte.
// For SYSEX_ABORT, byte contains the interrupting
// status, or zero for explicit cancellation.
//
// F0 and F7 are represented by BEGIN and END.
typedef struct {
  midi_stream_event_type_t type;
  midi_short_message_t message;
  uint8_t byte;
} midi_stream_event_t;

#define MIDI_PORT_CAPACITY 32U
#define MIDI_PORT_MAX_CABLES 16U

typedef enum {
  MIDI_PORT_IN,
  MIDI_PORT_OUT
} midi_port_direction_t;

typedef enum {
  MIDI_PORT_UNBOUND,
  MIDI_PORT_BOUND,
  MIDI_PORT_AMBIGUOUS
} midi_port_state_t;

// Inspection identity within the current single-internal-hub USB topology.
// These values describe a provisioned port; they do not grant hardware access.
typedef struct {
  uint8_t root_port;
  uint8_t hub_port;
  uint16_t vendor_id;
  uint16_t product_id;
  uint8_t cable;
  midi_port_direction_t direction;
} midi_port_key_t;

// Live definition/binding snapshot. generation and slot_id describe only the
// current binding; handles grant access to one specific definition/generation.
typedef struct {
  unsigned int id;
  midi_port_key_t key;
  midi_port_state_t state;
  uint32_t generation;
  uint8_t slot_id;
  // Changes whenever this port ID is redefined.
  uint32_t definition_serial;
} midi_port_info_t;

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
// validity. Writes can still return BUSY or QUEUE_FULL. Identity describes
// the supplied handle, never a replacement.
// Stale handles may still have retired input events to drain.
// service_* fields describe the shared device service; cable_* fields
// describe the selected input cable and expose loss immediately, before its
// accepted events drain. Service/cable fields are reported only for a current
// handle, so a stale open cannot inspect a replacement open even on the same
// device generation.
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
  // Input-only. Queue fields describe this cable; loss counters exclude
  // earlier opens. Lower diagnostics retain generation-lifetime loss totals.
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

// Inspection never starts services or drains application events. Both return
// OK for an inspected closed/stale handle/session; check info.status.
midi_status_t midi_port_get_handle_info(const midi_port_handle_t *handle,
    midi_port_handle_info_t *info);
midi_status_t midi_port_sysex_get_info(const midi_port_sysex_t *session,
    midi_port_sysex_info_t *info);

// The caller owns this session and must initialise
// it to zero before its first use. Copies alias the same claim; successful
// end/cancel releases that claim for every copy. Only the supplied session
// has its active field cleared. The session never targets a replacement claim.
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

// Accept a prefix of a seven-bit payload without appending F7. accepted is
// required; *accepted is always set, including on errors. QUEUE_FULL leaves
// the suffix unconsumed; retry at bytes + *accepted. Validate the entire
// payload first: INVALID_ARGUMENT never consumes a prefix. A zero-length
// payload permits NULL bytes but still checks the session's availability.
midi_status_t midi_port_sysex_write_bytes(midi_port_sysex_t *session,
    const uint8_t *bytes, size_t count, size_t *accepted);

// Enqueue the final fragment, including F7.
// QUEUE_FULL leaves the session active for retry.
midi_status_t midi_port_sysex_end(midi_port_sysex_t *session);

// Abandon the unsent suffix. If earlier fragments
// were queued and the device is running, enqueue
// F7 on that original generation.
//
// QUEUE_FULL leaves the session active for retry.
// OK also releases a session whose original claim no longer exists.
midi_status_t midi_port_sysex_cancel(midi_port_sysex_t *session);

// Remove the oldest pending connection transition for this definition.
// This queue is shared by callers; EMPTY means no transition is pending.
midi_status_t midi_port_next_change(unsigned int id, midi_port_change_t *change);

// Inspect lifecycle state and counters for this definition, across opens.
// Retired storage and connection notifications are finite: retired_dropped
// counts newer events discarded when retirement storage is full;
// retirement_missed counts inaccessible old input records; changes_dropped
// counts older connection notifications displaced by newer transitions.
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
// Input closure releases its selected application-owned cable, discarding
// its unread events and resetting its parser/fault for the next open. Other
// cables and any independently owned diagnostic receiver remain unaffected.
// Output closure revokes permission to enqueue; accepted output may still send.
//
// Closing a current OUT port returns BUSY while its cable has an active
// SysEx claim; finish or cancel the session first. Closing a stale handle does
// not cancel its original claim; use the pinned session to cancel it.
// Closing a stale handle releases only that handle and its retired events;
// it cannot close a replacement open. It returns OK even after undefine.
// Closed handles return NOT_OPEN. Identity fields are retained on close.
midi_status_t midi_port_close(midi_port_handle_t *handle);

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
// A revoked binding can drain its own retired events before DISCONNECTED,
// even while a replacement is open, as long as the original definition remains.
// Closing the handle or removing the definition discards those events. A read
// never consumes another open's events.
// An open, attached input with no event returns EMPTY, or DATA_LOST once
// its accepted events/terminal ABORT have drained after FIFO overflow.
// Repeating open or freeing queue space cannot reset this fault. Explicitly
// close/reopen application-owned input to recover only this cable, without
// rebinding or restarting the shared receiver. Loss counters exclude earlier
// opens; the new open does not inherit the old fault or queued events.
// Disconnection/stale permission takes precedence after retired events drain.
midi_status_t midi_port_read(const midi_port_handle_t *handle,
    midi_port_event_t *event);
