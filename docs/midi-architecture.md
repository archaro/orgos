# Implemented MIDI architecture

OrgOS provides a foreground-serviced USB-MIDI 1.0 implementation on Raspberry Pi
4B, with an application boundary in [`midi.h`](../src/midi.h). The
[API guide](midi-api.md) describes what an application may call. This document
explains the existing layers; it does not specify a future touchscreen,
configuration-file format, general USB stack or scheduler.

## Data flow

MIDI IN:

```text
USB device, generation G
    -> xHCI Bulk IN DMA/completion handling
    -> completed-transfer queue
    -> usb_midi_rx: validate transfer, decode four-byte USB-MIDI packets
    -> shared decoded-packet FIFO, one exclusive subscriber
    -> midi_input / midi_usb_bridge: prepare parser update for packet's cable
    -> independent cable FIFO + optional pending terminal ABORT
    -> logical IN handle: midi_port_read()
    -> application short message or SysEx event

Binding loss: accepted input -> retired events tagged with original open token
                            -> old handle only, then DISCONNECTED
```

MIDI OUT:

```text
application OUT handle
    -> midi_port_write(), or pinned SysEx session operations
    -> MIDI validation / USB-MIDI encoding
    -> one device output FIFO (all output cables)
    -> paced packet batch, one asynchronous transfer in flight
    -> xHCI Bulk OUT DMA/completion handling
    -> confirmed batch removed, or terminal/uncertain outcome retained/accounted
    -> external USB-MIDI device / MIDI connection
```

An accepted application call is not confirmation that any external MIDI peer
received or acted on its data. Even a successful USB completion confirms only the
host's transfer, not the downstream DIN endpoint or musical effect.

## Enumeration, generations and retirement

[`usb_enumerate.c`](../src/usb_enumerate.c),
[`usb.c`](../src/usb.c) and [`usb_hub.c`](../src/usb_hub.c) own physical discovery,
admission, enumeration and teardown. The supported path is the Pi's internal hub
topology and downstream devices. An enumerated incarnation has a nonzero
generation and an xHCI slot; a socket identity, connection epoch, generation and
slot ownership are distinct concepts.

USB 2.0 hub disconnection, or an unexpected enable-change with an active port
still connected but disabled, removes the old binding. An ordinary enable change
associated with successful reset is not itself a disconnection. Repeated
notifications must not duplicate admission. A physically connected replacement is
reconsidered only after the preceding generation is safely retired. Quiescence,
slot disable/recycle and uncertain ownership are fail-closed: an uncertain slot
is not automatically reusable. A narrowly gated pre-slot `DEVICE_NOT_FOUND`
attempt can be rearmed only after verified disconnection and absence of ownership;
it is not an unconditional enumeration retry policy.

[`usb_midi_parse_config()`](../src/usb_midi.c) checks descriptor boundaries and
requires both Bulk endpoints with immediately associated class-specific jack
counts in 1–16. The parser accepts one MIDIStreaming interface descriptor rather
than searching alternatives. Enumeration selects the described configuration and
issues SET_INTERFACE. The current xHCI MIDI endpoint setup accepts IN `0x81`, OUT
`0x02`, and 64-byte maximum packets. This is not general compatibility with
single-direction devices, arbitrary endpoints, packet sizes or alternate-setting
collections. Tests of independently usable application directions do not broaden
these enumeration restrictions.

The CME U2MIDI Pro descriptor format is supported. The existing VID/PID quirk
(`7104:1400`) sends startup EOX and waits during enumeration to address the
observed first-message framing issue; this is separate from nonblocking
application I/O. See [`usb_midi_quirks.c`](../src/usb_midi_quirks.c).

## xHCI Bulk IN and OUT

[`xhci.c`](../src/xhci.c) owns transfer rings, completion waiters, DMA ownership and
controller resources. Persistent Bulk IN uses 64-byte requests and a queue of
completed transfers (currently 16 records), with completion validation and rearm
in foreground service. The MIDI receiver obtains completed-transfer records,
not raw DMA ownership. A full hardware/completion queue and actual transport
failures retain their transport-level behaviour; cable-local application
saturation does not weaken these checks.

Bulk OUT has an asynchronous state machine. Fully successful completion requires
an event identifying the original TRB, completion code Success and **zero residual
bytes in the low 24 status bits**, plus successful ownership transitions. A
nonzero residual is definitive failure, including when the code says Success.
Timeout/dispatch faults can leave ownership uncertain; they do not prove DMA is
safe to reuse. A late completion can resolve the original request, but an
uncertain/failed endpoint is not automatically restored for further sending.
The synchronous diagnostic wrapper uses this asynchronous implementation and its
completion semantics. None of these controller calls is part of `midi.h`.

## Packet codec, receiver and per-cable parsing

[`usb_midi.c`](../src/usb_midi.c) decodes a four-byte USB-MIDI event packet into
cable, Code Index Number (CIN), length and up to three MIDI bytes. Reserved CINs
are rejected. Decoding packet framing alone does not establish valid MIDI message
semantics. The same module validates/encodes short messages and SysEx fragments
for output.

[`usb_midi_rx.c`](../src/usb_midi_rx.c) owns one persistent receiver per managed
device incarnation. It checks device/slot/connection identity, transfer length,
residual and completion fields, stages decoded packets, then feeds a shared
64-packet FIFO to its exclusive subscriber. A false callback return retains the
head for retry. A true return means **consumed**, including an explicit discard;
it does not necessarily mean application delivery.

Application/automatic receiver startup selects discard-unclaimed policy. An
unopened cable does not consume an application's FIFO. After the last logical
input closes, the receiver continues draining/discarding. New subscriptions clear
old retained receiver FIFO/staging data under this policy. Existing receivers
are reused without replacing their ownership. Explicit diagnostic startup instead
retains unclaimed data and exclusive ownership. An explicitly started all-cable
input parser keeps that selection policy when logical ports are subsequently
opened. Applications must not mix low-level diagnostic consumers with their own
logical reads and expect independent copies.

[`midi_stream.c`](../src/midi_stream.c) parses MIDI bytes, including running
status, realtime messages and SysEx. [`midi_usb_bridge.c`](../src/midi_usb_bridge.c)
maintains a parser per advertised cable and prepares a complete packet's proposed
parser state/event batch before committing it. Cable 0's incomplete SysEx never
becomes cable 1's parser state. The application receives complete SHORT messages
or BEGIN/DATA/END/ABORT events, not USB packets or parser objects.

## Independent input queues and overflow isolation

[`midi_input.c`](../src/midi_input.c) provides a static 64-event FIFO **per virtual
cable**, not a shared device-wide 64-event allowance. One USB packet can produce
several events. Its whole batch must fit before parser/event state is committed.

If it cannot fit, the packet is consumed without inserting any of its events.
The affected cable increments its boundary-overflow/drop counters and enters
sticky loss of synchronisation. Further packets for that cable increment its
drop count and are consumed without parsing/delivery. This prevents a full cable
from pinning the shared USB receiver head while other cables have space.

Already accepted events remain ordered/readable. An incomplete committed SysEx
is cancelled; a pending terminal ABORT has reserved storage outside the FIFO and
is returned after its earlier accepted events. Rejected packet proposals are not
committed, and overflow never fabricates successful END. Malformed MIDI has a
separate device-level counter; a malformed packet that requires an ABORT when its
cable FIFO is full can also cause cable-local loss. Neither statistic is a
transport restart/failure counter.

Space becoming available does not recover the fault. Normal application cable
close discards that cable's remaining events/ABORT and resets its parser and
active fault; reopen starts a new receiving session. Other cables' queues and
parsers survive. Generation-lifetime lower counters remain cumulative; application
handle inspection subtracts a per-open baseline for its cable's loss counts.
Explicit all-cable diagnostics observe faults in lower input statistics and
recover through their own stop/drain/restart lifecycle, not a logical close that
would displace their owner.

Cable-specific lower reads consume only their FIFO. The lower device-wide
`midi_input_read()` selects the earliest remaining accepted head/ABORT using
acceptance order, including after mixed cable-specific reads. It neither waits
for a faulted cable to recover nor repeats consumed events. Applications use
`midi_port_read()` and choose their own order across handles.

## Output queueing, claims and accounting

[`midi_output.c`](../src/midi_output.c) has a 64-packet FIFO per device generation,
shared by output cables. Up to 16 four-byte packets form a Bulk OUT batch. The
submitted batch remains in the queue until complete success is reconciled, so
queue depth includes in-flight packets. Sending is paced using each packet's MIDI
byte length at 320 microseconds per DIN byte; no new scheduler is implied.

One SysEx claim per output service prevents other SysEx and short writes on that
device while active. Up to two bytes, initially including F0, are staged until
a packet is complete. Byte/prefix acceptance is exact: backpressure never consumes the failed
byte. End appends F7. Cancellation preserves queued fragments and, while running,
appends an EOX if needed; it does not retract already accepted packets.

Disconnect/failure prevents new submissions for the old generation. An outstanding
request is not immediately counted as irrevocably unconfirmed while a matching
old-generation completion remains reconcilable. Late success removes/confirms
only that original batch; late failure records it as unconfirmed. When ownership
or the original record is gone, including record recycling, remaining packets
are accounted once. Cumulative boot-lifetime diagnostics distinguish
`not_submitted` from `submitted_unconfirmed`, with invariant-violation tracking.
No packet is counted as both confirmed and unconfirmed. There is **no speculative
retransmission** after uncertainty; a late success does not resume a failed
service's queued tail.

These completion/loss diagnostics are lower-level inspection, not a public
per-message delivery acknowledgement. `midi_port_write()` and SysEx acceptance
return local queue/staging results only.

## Logical binding and capability boundary

[`midi_ports.c`](../src/midi_ports.c) owns explicit logical definitions keyed by
socket, VID/PID, cable and direction. Duplicate same-direction aliases and
redefinition of occupied IDs are rejected. Complete topology snapshots yield
unbound, bound or ambiguous state. [`midi_ports_usb.c`](../src/midi_ports_usb.c)
adapts eligible enumerated devices to that registry. Provisioning remains an
internal/system concern; there is no implemented configuration-file format here.

[`midi_port_io.c`](../src/midi_port_io.c) implements the public operations. A handle
pins definition serial, generation, cable/direction and a unique open token. A
SysEx session additionally pins its exclusive claim. Reconnection, redefinition
and another open cannot retarget those capabilities. IN/OUT are independent even
though each direction shares appropriate device services with other cables.

Bindings are reconciled and old input retired before lifecycle teardown can
recycle records. Retired events keep their original open token and generation;
a replacement open cannot consume them. Retirement storage is currently 65
events per definition across old opens; full storage preserves older events and
counts newer discards. Connection changes use an eight-entry queue that keeps the
newest transitions and counts displaced older notifications. Removing a
definition clears its history. Handle/lifecycle inspection makes these finite
storage limits observable.

## Foreground service and ownership boundaries

The main loop in [`main.c`](../src/main.c) calls `usb_service()`, then bounded
`console_midi_service()`, then `console_poll()`. It currently delays 1 ms on idle
console iterations. Application calls do not recursively service USB.

The housekeeping path in [`usb.c`](../src/usb.c) processes port/hub changes,
services xHCI Bulk IN/OUT, reconciles MIDI output, handles diagnostic input,
decodes persistent receive traffic, checks input ownership and updates logical
bindings. It then performs lifecycle work and admission, automatic MIDI
attachment, and another binding update. Interrupt notifications and event-ring
dispatch feed foreground work; interrupt handlers are not application MIDI
consumers.

[`midi_auto.c`](../src/midi_auto.c) starts eligible services without taking over
existing diagnostic streams. Disabling automatic attachment does not prevent an
application's explicit port open. Applications own capabilities and copied data;
services own queues/parsers; xHCI owns hardware/DMA transitions; USB lifecycle owns
admission and safe retirement. Keep those ownership boundaries intact when adding
application logic.
