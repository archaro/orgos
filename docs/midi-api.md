# OrgOS application MIDI API

Include [`src/midi.h`](../src/midi.h). It is the complete application interface;
applications do not need USB, xHCI, parser, registry or service headers. This guide
describes the implemented GNU17 interface, not a proposed configuration format.
See [architecture](midi-architecture.md) for the implementation and
[testing](midi-testing.md) for verification.

## Calling context and storage

Serialise calls in the foreground context that runs OrgOS's normal USB/MIDI
service loop. Do not call from interrupts, transport callbacks or concurrently
from multiple threads/cores. All operations are nonblocking: they do not wait for
hardware or pump services. Return to the foreground loop between attempts;
busy-waiting on `EMPTY` or `QUEUE_FULL` prevents progress.

Messages and accepted payload bytes are copied before return. No caller pointers
are retained. Returned events and information structures are caller-owned
snapshots, not live objects. Service progress can invalidate a snapshot; each I/O
operation rechecks permission.

Zero-initialise handles and SysEx sessions before first use. Their fields are
read-only to the application, except through the API. Keep active objects until
explicitly closed, ended or cancelled. Do not forge tokens, edit generations,
clear an active object, or overwrite it to acquire a replacement. Copying a
handle/session aliases one capability, not an independent open or claim. A
successful close/end/cancel releases the underlying permission for all copies;
only the object supplied to the operation has its `active` field cleared. Other
copies can still look active locally and must not be treated as valid permission.
There is no automatic cleanup when a C object leaves scope. Capabilities and
identity counters are scoped to this boot; do not persist handles across reboot.

## Identity, binding and lifetime

Logical IDs are **1 through `MIDI_PORT_CAPACITY` (32)**. Each provisioned definition
selects a socket/device identity, a zero-based virtual cable and one direction.
`MIDI_PORT_MAX_CABLES` is 16; actual device associations can advertise fewer.
IN and OUT are separate definitions and opens, even for the same device/cable.
MIDI channel numbers are encoded inside message status bytes and are independent
of USB virtual cable numbers.

A definition's `definition_serial` changes when its numeric ID is removed and
redefined. Its USB `generation` identifies one enumerated device incarnation.
Disconnect/reconnect may bind the same definition to a **new generation**; neither
the numeric ID nor a reused xHCI slot authorises an old handle to use it. Handles
also contain an `open_token` identifying the particular open. Tokens do not
repeat within a boot; exhausted token allocation fails rather than wrapping.

Definitions are provisioned by the system/diagnostic console, not by this API.
Discovery includes unbound and ambiguous definitions. It neither creates them nor
automatically selects a device when several match.

## Results and error handling

Every operation except `midi_status_name()` returns `midi_status_t`. Compare
explicitly with `MIDI_STATUS_OK`; its numeric value is zero.

| Status | Meaning and response |
| --- | --- |
| `MIDI_STATUS_OK` | Operation succeeded locally. For output, this means accepted into local staging/queueing, **not delivery confirmation**. |
| `MIDI_STATUS_EMPTY` | No input event or connection notification; discovery has reached its end. Normal idle condition. |
| `MIDI_STATUS_NOT_OPEN` | Zero/closed handle, stopped input/output service, or inactive/missing SysEx claim. |
| `MIDI_STATUS_DISCONNECTED` | Unbound device or obsolete definition, generation or open permission. Drain eligible retired input, release the old capability, then explicitly open again. |
| `MIDI_STATUS_BUSY` | Another open or a SysEx claim prevents this operation. Do not overwrite the owning object. |
| `MIDI_STATUS_QUEUE_FULL` | Output queue backpressure. Return to servicing and retry only the unaccepted operation/suffix. |
| `MIDI_STATUS_FAILED` | Service failure, resource exhaustion or an unclassified lower-layer rejection. It is not a promise that retrying is safe or sufficient. |
| `MIDI_STATUS_INVALID_ARGUMENT` | Null required pointer, out-of-range ID, wrong direction or invalid MIDI payload. |
| `MIDI_STATUS_NOT_DEFINED` | Valid numeric ID has no definition; handle operations generally classify a removed definition as `DISCONNECTED`. |
| `MIDI_STATUS_AMBIGUOUS` | Definition matches multiple eligible devices; opening refuses to choose one. |
| `MIDI_STATUS_DATA_LOST` | Sticky input FIFO loss on this cable. Drain the accepted prefix and any terminal ABORT, then close/reopen application-owned input to recover. |

`midi_status_name(status)` returns a static uppercase name, or `"UNKNOWN"` for an
unrecognised value. Do not free or modify it.

Read/query output structures are cleared before errors when a valid output
pointer is supplied. A successful handle/session query returns `OK` even when the
queried capability is stale or closed: examine **`info.status`**. Output may still
return `BUSY` or `QUEUE_FULL` after an inspection reports `OK`. Return-code
precedence follows validation and ownership checks; do not infer that one error
code is the only possible result for an operation with several invalid inputs.

## Public value types

All integer fields use the types declared in `midi.h`. Inspect identity values;
do not manufacture handles from them.

| Type | Fields and interpretation |
| --- | --- |
| `midi_status_t` | Result enumeration described above. |
| `midi_short_message_t` | `status` is a complete status byte; `data[2]` holds up to two seven-bit bytes; `length` is total MIDI byte count, including status (1–3). Unused data bytes are not transmitted. |
| `midi_stream_event_type_t` | `MIDI_STREAM_SHORT`, `MIDI_STREAM_SYSEX_BEGIN`, `MIDI_STREAM_SYSEX_DATA`, `MIDI_STREAM_SYSEX_END`, `MIDI_STREAM_SYSEX_ABORT`. These are application event kinds; no parser object is exposed. |
| `midi_stream_event_t` | `type` selects the interpretation. `message` is meaningful for SHORT; `byte` is a payload byte for DATA and the interrupting status, or zero for explicit cancellation, for ABORT. BEGIN/END represent F0/F7; those framing bytes are not DATA. |
| `midi_port_direction_t` | `MIDI_PORT_IN` or `MIDI_PORT_OUT`, from the application's perspective. |
| `midi_port_state_t` | `MIDI_PORT_UNBOUND` (no eligible device), `MIDI_PORT_BOUND` (one), `MIDI_PORT_AMBIGUOUS` (more than one). |
| `midi_port_key_t` | `root_port`, `hub_port` identify a socket in the current single-internal-hub topology; `vendor_id`, `product_id` identify the device model; `cable` is zero-based; `direction` selects IN/OUT. This is an inspection identity, not permission. |
| `midi_port_info_t` | `id`, `key`, `state`, `generation`, `slot_id`, `definition_serial`. Generation/slot describe the current binding and are zero for unbound/ambiguous definitions. A slot ID is diagnostic information, not an I/O handle. |
| `midi_port_handle_t` | `active`, `port`, `definition_serial`, `generation`, `open_token`, `cable`, `direction`. Caller-owned capability pinned to one definition, incarnation and open. `active` alone does not establish current validity. |
| `midi_port_event_t` | `port` is the logical ID; `generation` is the event's original incarnation, including retired input; `event` is a `midi_stream_event_t`. The cable/direction follow the handle. |
| `midi_port_change_t` | `port`, `previous`, `current`, `previous_generation`, `current_generation` describe one observed binding transition. Generation zero means no binding. |
| `midi_port_lifecycle_info_t` | `opened_generation` is the current authorised open's generation, or zero; `retired_depth` counts retained events across old opens of this definition; `retired_dropped` counts newer events lost when retirement storage fills; `retirement_missed` counts inaccessible old input records, not packets; `pending_changes` and `changes_dropped` count queued and displaced connection notifications. Counters belong to the definition across opens, not a replacement definition. |
| `midi_port_sysex_t` | `active`, `port`, `definition_serial`, `generation`, `cable`, `claim`, `open_token`. Caller-owned SysEx capability pins both the original open and its exclusive output claim. Identity remains after end/cancel/disconnect. |

### Handle inspection: `midi_port_handle_info_t`

| Field(s) | Meaning |
| --- | --- |
| `handle`, `status` | Supplied identity and current permission/service status. A loss-faulted cable reports `DATA_LOST` immediately, even with readable accepted events. |
| `retired_depth` | Retired input events belonging to this particular open token, not all opens. |
| `service_present` | A matching input/output service record was found for a current handle. |
| `service_queue_depth`, `service_queue_capacity` | Shared device-service queue totals. Input sums independent cable FIFOs; output includes packets still in flight. These are events for IN and USB-MIDI packets for OUT. |
| `service_pending_aborts` | Input terminal ABORTs held outside full FIFOs; not included in input queue depth/capacity. |
| `service_in_flight` | Output packets in the submitted batch; part of output queue depth. |
| `sysex_active`, `sysex_cable` | Output service's shared SysEx ownership, which can belong to another cable. |
| `cable_queue_depth`, `cable_queue_capacity`, `cable_queue_peak` | Selected input cable's event FIFO occupancy, capacity and high-water mark. Pending ABORT is separate. |
| `cable_data_lost` | Sticky fault for this cable. |
| `cable_overflow_packets` | Packets rejected at the FIFO boundary, measured since this open. |
| `cable_dropped_packets` | Boundary-rejected packets plus subsequent packets discarded while faulted, measured since this open. |

Service/cable fields describe only a current handle. Stale handles cannot inspect
a replacement open's counters, even within the same device generation.
Input-only/output-only fields stay zero when not applicable. Normal cable closure
resets queue/parser/fault state; lower diagnostic loss counters remain cumulative
within the generation. A new application's loss-counter baseline excludes earlier
opens.

### Session inspection: `midi_port_sysex_info_t`

`session` copies the supplied identity. `status` describes permission/service
validity for write/end. `claim_present` says whether the **original** claim still
exists for cancellation, even if status is `DISCONNECTED`. `staged` counts bytes
currently held for a fragment (including a staged F0 when applicable);
`fragments` counts fragments enqueued for the active claim, not delivered fragments.
No field refers to a replacement claim.

## Discovery and lifecycle operations

```c
midi_status_t midi_port_get_info(unsigned int id, midi_port_info_t *info);
midi_status_t midi_port_next(unsigned int *cursor, midi_port_info_t *info);
midi_status_t midi_port_next_change(unsigned int id, midi_port_change_t *change);
midi_status_t midi_port_lifecycle_get_info(unsigned int id,
    midi_port_lifecycle_info_t *info);
```

All output pointers are required. IDs outside 1–32 are invalid. An undefined valid
ID returns `NOT_DEFINED` for direct definition/change/lifecycle queries.

- `get_info` inspects without opening or starting services. `OK` includes
  unbound/ambiguous definitions: inspect `state`.
- `next` enumerates defined IDs in ascending order, including unbound/ambiguous
  ones. Initialise `*cursor = 0`. `OK` advances it to `info.id`; `EMPTY` sets it to
  `MIDI_PORT_CAPACITY`. Other errors leave the cursor unchanged. Reset to zero to
  restart. A cursor greater than capacity is invalid. This is a live view.
- `next_change` consumes the oldest pending transition for this definition, or
  returns `EMPTY`. All callers share that notification queue. It retains the
  newest transitions if full and counts displaced older ones.
- `lifecycle_get_info` consumes no notification and reports the counters above.
  Retirement and notification storage are finite, so check loss counters if
  complete history matters.

```c
#include "midi.h"

midi_status_t discover_pair(unsigned int *in_id, unsigned int *out_id) {
    unsigned int cursor = 0U;
    *in_id = *out_id = 0U; /* caller supplies non-null pointers */
    for (;;) {
        midi_port_info_t p = {0};
        midi_status_t s = midi_port_next(&cursor, &p);
        if (s == MIDI_STATUS_EMPTY)
            break;
        if (s != MIDI_STATUS_OK)
            return s;
        if (p.state != MIDI_PORT_BOUND)
            continue;
        if (p.key.direction == MIDI_PORT_IN && *in_id == 0U)
            *in_id = p.id;
        if (p.key.direction == MIDI_PORT_OUT && *out_id == 0U)
            *out_id = p.id;
    }
    return *in_id != 0U && *out_id != 0U ? MIDI_STATUS_OK : MIDI_STATUS_EMPTY;
}
```

This example chooses the first bound port in each direction; they need not be on
the same device. A real application can select by inspected key instead.

## Opening, closing and inspecting handles

```c
midi_status_t midi_port_open(unsigned int id, midi_port_handle_t *handle);
midi_status_t midi_port_close(midi_port_handle_t *handle);
midi_status_t midi_port_get_handle_info(const midi_port_handle_t *handle,
    midi_port_handle_info_t *info);
```

`open` requires a non-null, initially zero/closed handle and a defined, bound ID.
It starts or reuses the appropriate service and acquires permission. It does not
restart/displace an already-running shared receiver or diagnostic owner. Explicit
opening works even if automatic MIDI attachment is disabled.

One open per definition is allowed. Another handle returns `BUSY`. Reopening the
same current handle for the same ID is idempotent; asking an active handle to open
a different ID returns `BUSY`. An obsolete active handle is never retargeted and
returns `DISCONNECTED` for its original ID. Failed opens leave the handle
unchanged, though underlying service startup may have been attempted.

`close` releases the selected application's input cable and discards its unread
events/ABORT, resetting that cable's parser and active loss state. Other cables
continue. Closing the last logical input leaves the ordinary shared USB receiver
running and discarding unclaimed traffic. Externally owned diagnostic input keeps
its explicit lifecycle; logical close/reopen does not replace its owner or reset
its retained input state.

Output close revokes permission to enqueue; already accepted output may still
transmit. A current OUT cable with a live SysEx claim returns `BUSY`: end/cancel it
first. A stale handle can be closed with `OK`, even after undefinition; this removes
only its permission/retired events, never a replacement open. Stale close does not
cancel an outstanding original SysEx claim. Closed handles return `NOT_OPEN`.
Close retains identity fields but clears the supplied handle's `active` flag.

`get_handle_info` requires both pointers. `OK` means the inspection succeeded;
`info.status` describes the capability. It starts no service and does not deliver
or drain application events.

```c
#include "midi.h"

/* Caller supplies two distinct zero-initialised handles. */
midi_status_t open_pair(unsigned int in_id, unsigned int out_id,
                       midi_port_handle_t *in, midi_port_handle_t *out) {
    midi_status_t s = midi_port_open(in_id, in);
    if (s != MIDI_STATUS_OK)
        return s;
    s = midi_port_open(out_id, out);
    if (s != MIDI_STATUS_OK)
        (void)midi_port_close(in); /* input acquisition has no SysEx claim */
    return s;
}
```

## Reading, ordering and cable-local loss

```c
midi_status_t midi_port_read(const midi_port_handle_t *handle,
    midi_port_event_t *event);
```

Both pointers are required. Only IN handles may read. Each successful call removes
one event from that cable, preserving FIFO order without consuming another
cable's events. Applications using separate handles choose their own cross-cable
read order; this API does not expose a device-wide merged reader.

`EMPTY` is normal for a current attached input with no available event. On cable
FIFO saturation, a whole USB-MIDI packet is rejected atomically. That cable
becomes sticky-faulted; every subsequent packet for it is discarded, even after
space becomes available. Already accepted events remain readable in order. If an
accepted SysEx was incomplete, a terminal ABORT follows its accepted prefix;
no successful END is fabricated. Realtime SHORT events can occur inside a SysEx
stream; ABORT means discard the incomplete message, not accept it as completed.
After those events drain, reads return
`DATA_LOST`, not `EMPTY`. Inspect loss immediately through `get_handle_info`.
Other cables remain usable and retain their handles and data.

For ordinary application-owned input, explicitly close then reopen the same
logical ID to establish a fresh cable session. This requires neither rebinding
nor restarting the receiver. It clears the active fault/parser and excludes
previous sessions' loss from the new handle's counters. It cannot recover missing
MIDI state; the application decides how to reset musical state. Closing early
intentionally discards any unread accepted prefix.

On binding loss, eligible accepted events are retired under their original open
token and generation. The old handle can read them while the original definition
remains, even while a new handle uses a replacement incarnation. Afterwards it
returns `DISCONNECTED`; it never silently switches generations. Retirement is
bounded: consult lifecycle counters for discarded/inaccessible history. Closing
that handle, removing/redefining the logical definition, or losing its retained
storage ends that drain opportunity. Disconnection takes precedence over cable
loss once the original binding has been revoked and its retired events drain.

```c
#include "midi.h"

/* Application supplies this hook; SysEx events need their own handler. */
void accept_short(const midi_short_message_t *message);

midi_status_t receive_step(const midi_port_handle_t *in) {
    for (unsigned int n = 0U; n < 32U; ++n) {
        midi_port_event_t e = {0};
        midi_status_t s = midi_port_read(in, &e);
        if (s != MIDI_STATUS_OK)
            return s; /* EMPTY is idle; DATA_LOST/DISCONNECTED need handling */
        if (e.event.type == MIDI_STREAM_SHORT)
            accept_short(&e.event.message);
        /* Handle BEGIN/DATA/END/ABORT here if this application accepts SysEx. */
    }
    return MIDI_STATUS_OK; /* budget used; return to the foreground loop */
}
```

For reconnection, drain the old handle until `DISCONNECTED`, close it, inspect the
logical definition and explicitly open once bound again. Preserve the old object
if you intend to drain it; do not zero it to force reopening. A replacement can
also be opened in a separate zeroed object while the old object drains.

```c
#include "midi.h"

/* Call after receive_step returns DATA_LOST or DISCONNECTED, once the accepted
   prefix has been handled. Also usable on later iterations with a closed h. */
midi_status_t recover_input(unsigned int id, midi_port_handle_t *h) {
    if (h->active) {
        midi_status_t s = midi_port_close(h);
        if (s != MIDI_STATUS_OK)
            return s;
    }
    return midi_port_open(id, h);
}
```

After `DATA_LOST`, this recovers the same generation if still bound. After
`DISCONNECTED`, a not-yet-reconnected definition returns `DISCONNECTED` (or
`AMBIGUOUS`/`NOT_DEFINED` as appropriate). Keep the now-closed object and try on a
later foreground iteration or connection change, not in a tight loop. Re-check
the definition/key if provisioning may have changed. Do not apply this helper to
OUT with an active SysEx session; cancel the pinned claim first.

## Short-message output

```c
midi_status_t midi_port_write(const midi_port_handle_t *handle,
    const midi_short_message_t *message);
midi_status_t midi_port_send_short(const midi_port_handle_t *handle,
    const midi_short_message_t *message);
```

`send_short` is an existing spelling with identical behaviour. Both pointers are
required and only OUT handles may send. `OK` accepts the whole message;
`QUEUE_FULL` accepts none of it. Retry the same message after service progress.
A shared output SysEx claim blocks short output on **all cables of that device**
with `BUSY`. No public function waits for or confirms delivery to a MIDI peer.

Supported statuses/lengths follow the existing encoder:

| Status bytes | Total length |
| --- | --- |
| `80`–`EF` channel voice, except `Cn`/`Dn` | 3 |
| `Cn`, `Dn` | 2 |
| `F1`, `F3` | 2 |
| `F2` | 3 |
| `F6`, `F7`, `F8`, `FA`, `FB`, `FC`, `FE`, `FF` | 1 |

Other status values, wrong lengths and high-bit payload data are rejected with
`INVALID_ARGUMENT`. F0/streamed SysEx use session operations. The accepted
standalone F7 is an EOX message, not a successful streamed SysEx session.

```c
#include "midi.h"

midi_status_t send_cc(const midi_port_handle_t *out, uint8_t value) {
    midi_short_message_t cc = {
        .status = 0xbfU, .data = {102U, value}, .length = 3U
    }; /* channel 16, controller 102; value must be 0..127 */
    return midi_port_write(out, &cc);
}
```

Keep a rejected message pending in application state. Retrying an already accepted
message duplicates it. Terminal failures or uncertain USB completion do not
justify speculative retransmission.

## SysEx output sessions

```c
midi_status_t midi_port_sysex_begin(const midi_port_handle_t *handle,
    midi_port_sysex_t *session);
midi_status_t midi_port_sysex_write(midi_port_sysex_t *session, uint8_t byte);
midi_status_t midi_port_sysex_write_bytes(midi_port_sysex_t *session,
    const uint8_t *bytes, size_t count, size_t *accepted);
midi_status_t midi_port_sysex_end(midi_port_sysex_t *session);
midi_status_t midi_port_sysex_cancel(midi_port_sysex_t *session);
midi_status_t midi_port_sysex_get_info(const midi_port_sysex_t *session,
    midi_port_sysex_info_t *info);
```

All handle/session/info/accepted pointers are required. `bytes` may be NULL only
for `count == 0`. Identity and claim fields must not be edited.

- `begin` requires a current OUT handle and a zeroed/inactive session. It pins the
  definition, generation, open token, cable and a boot-unique nonzero claim. Only
  one session per device output service exists; another claim returns `BUSY`.
  F0 is staged locally and need not have been queued/transmitted yet.
- `write` accepts one seven-bit payload byte, without F0/F7. Non-OK leaves that
  byte unconsumed. Some accepted bytes remain staged until a fragment is complete.
- `write_bytes` validates the entire supplied payload before consuming a prefix.
  An invalid byte returns `INVALID_ARGUMENT` with `*accepted == 0`. On other
  failures, the prefix count still tells exactly what was consumed. In particular,
  `QUEUE_FULL` leaves the suffix unconsumed: advance by `*accepted` and retry only
  that suffix. A zero-length request still checks session validity. A NULL
  `accepted` is invalid and cannot be written to.
- `end` enqueues the final fragment including F7 and releases the claim on `OK`.
  `QUEUE_FULL` leaves it active; retry end, not the accepted payload.
- `cancel` discards the locally staged suffix. If fragments were already queued
  and the original service is running, it enqueues F7 after them; this may return
  `QUEUE_FULL`, leaving the session active for retry. Before any fragment was
  queued, cancellation needs no outgoing terminator. For a failed/disconnected
  service it releases local ownership without retransmitting old data. If the
  original claim no longer exists, cancellation returns `OK` and clears the
  supplied object's active flag. It never cancels a replacement claim.
- `get_info` returns `OK` for a successful query even when `info.status` is
  `NOT_OPEN`/`DISCONNECTED`/`FAILED`. `claim_present` may still be true for a stale
  open, so retain the session until cancellation succeeds. Inspection consumes no
  payload and creates no claim.

End/cancel retain identity fields. Copies remain aliases with potentially stale
local `active` flags. Neither session completion nor fragment counters confirm
receipt by the external MIDI device.

```c
#include "midi.h"

struct sysex_job {
    midi_port_sysex_t session;
    size_t offset;
    bool begun;
    bool done;
}; /* initialise the entire job to zero */

midi_status_t sysex_step(const midi_port_handle_t *out, struct sysex_job *job) {
    static const uint8_t payload[] = {0x7dU, 0x01U, 0x02U, 0x03U};
    if (job->done)
        return MIDI_STATUS_OK;
    if (!job->begun) {
        midi_status_t s = midi_port_sysex_begin(out, &job->session);
        if (s != MIDI_STATUS_OK)
            return s;
        job->begun = true;
    }
    size_t accepted = 0U;
    midi_status_t s = midi_port_sysex_write_bytes(&job->session,
        payload + job->offset, sizeof(payload) - job->offset, &accepted);
    job->offset += accepted;
    if (s != MIDI_STATUS_OK)
        return s; /* on QUEUE_FULL, call again after service progress */
    s = midi_port_sysex_end(&job->session);
    if (s == MIDI_STATUS_OK)
        job->done = true;
    return s;
}

midi_status_t abandon_sysex(struct sysex_job *job) {
    midi_status_t s = midi_port_sysex_cancel(&job->session);
    if (s == MIDI_STATUS_OK)
        job->done = true;
    return s; /* retain the job and retry cancellation on QUEUE_FULL */
}
```

Call once per suitable foreground iteration. On a terminal error, stop attempting
payload/end and cancel the original session; do not bind its staged suffix to a
new handle. Keep servicing while cancellation needs queue space.

## Implementation references

The declarations are in [`midi.h`](../src/midi.h); application checks, retirement
and claims are in [`midi_port_io.c`](../src/midi_port_io.c). Tests in
[`midi_port_io_test.c`](../tests/midi_port_io_test.c) exercise ownership, statuses,
loss and independent directions. Payload validation is in
[`usb_midi.c`](../src/usb_midi.c); FIFO and ABORT behaviour are in
[`midi_input.c`](../src/midi_input.c) and
[`usb_midi_rx_test.c`](../tests/usb_midi_rx_test.c).
