# MIDI testing and validation

The host regression suite is deterministic and needs no USB/MIDI hardware. It
checks production software with synthetic inputs and selected hardware/service
mocks. Physical testing with CME interfaces is a separate activity. A host PASS
is not a claim that controller DMA, interrupts, cables or a physical multi-cable
device have been validated.

## Build and host commands

From the repository root:

```sh
make check                 # native host compilation and regression executables
make                       # AArch64 kernel build; produces kernel8.img
make kernel8.img           # explicit equivalent image target
```

The default cross prefix is `aarch64-linux-gnu-`; native tests use `cc` with GNU17,
`-Wall -Wextra -Werror -O2`. Overrides are supported, for example:

```sh
make HOSTCC=gcc check
make CROSS=aarch64-linux-gnu- kernel8.img
```

`make check` compiles/runs the suites listed below on each invocation. The public
API compilation test is compile-only. A successful image build establishes that
the target sources compile/link; it does not execute the kernel or test hardware.

## Regression coverage

The commands and link composition are authoritative in the [`Makefile`](../Makefile).

| Test | Real code and intended coverage | Boundary of simulation |
| --- | --- | --- |
| [`midi_api_compile_test.c`](../tests/midi_api_compile_test.c) | Includes only `midi.h`, uses public types/operations, and guards against selected parser/transport header leakage. | Compiles only; does not execute its example calls. |
| [`midi_output_test.c`](../tests/midi_output_test.c) | Output FIFO/order, batching, short encoding/decoding, SysEx staging/fragments/claim ownership, backpressure, cancellation and cumulative loss accounting. Disconnect followed by success/failure, unresolved record replacement, never-submitted tails and repeated accounting are covered. | xHCI asynchronous transfer state, USB identity and time are mocked; this is not execution of the real xHCI ring/DMA code. |
| [`midi_ports_test.c`](../tests/midi_ports_test.c) | Registry validation, unique definitions/keys, direction/cable matching, complete snapshots, unbound/bound/ambiguous state and generation changes. | Devices are supplied as registry snapshots, not enumerated over USB. |
| [`midi_port_io_test.c`](../tests/midi_port_io_test.c) | Public status classification, discovery/inspection, IN/OUT independence, open-token/definition/generation ownership, stale handles, retirement, bounded lifecycle history, SysEx partial writes/cancel/retry and receiver-start policy. Cable loss, accepted prefix/ABORT, close/reopen, counter baselines and replacement isolation are included. | Uses real application boundary, input/parser/registry components with receiver/output/USB fixture mocks. Single-direction fixture tests exercise application independence, not broader physical enumeration support. |
| [`midi_stream_bridge_test.c`](../tests/midi_stream_bridge_test.c) | MIDI stream parsing, running status, realtime, malformed input, SysEx events/cancellation and per-cable USB bridge preparation/commit behaviour. | Synthetic bytes/packets; no controller or physical link. |
| [`usb_midi_rx_test.c`](../tests/usb_midi_rx_test.c) | Real receiver, input, codec, bridge and stream code; discard/retain ownership policies, stale receiver queues/staging, atomic packet fit, cable isolation, SysEx ABORT ordering, sticky loss, device-wide order and generation recycling. Includes the three-cable integration scenarios below. | Simulated completed xHCI Bulk IN records; no physical descriptors/controller/DMA. |
| [`usb_midi_config_test.c`](../tests/usb_midi_config_test.c) | Synthetic valid/invalid configurations, required associations on both endpoints, zero/excess cable counts, exact descriptor lengths, truncation/boundaries and CME-style nine-byte audio endpoint descriptors. | Descriptor parsing only; no hardware enumeration or broadened endpoint compatibility. |
| [`usb_hub_change_test.c`](../tests/usb_hub_change_test.c) | White-box production USB lifecycle/enumeration code: ordinary disconnect, unexpected disable, harmless/repeated changes, safe retirement/admission, fail-closed ownership and narrowly gated pre-slot disappearance rearm. | Hub status, slot/endpoint ownership and lifecycle hardware actions are deterministic mocks; unrelated hardware/MSI sections are excluded. |
| [`midi_verify_test.c`](../tests/midi_verify_test.c) | Real console verifier command/state/service code: correct and paced sequences, empty reads, extras/missing/malformed events, first mismatch, early stop/reset, terminal status/handle invalidation, manual compatibility and 32-event auto budget. Service output is asserted absent. | Public MIDI reads/inspection and console output are mocked. A paced sequence is simulated with empty service iterations, not wall-clock timing. |

### Three-cable receive integration

`check_three_cable_interleaved_input()` advertises **three** valid input cables in
the enumeration fixture. Each completed transfer contains six distinguishable
Control Changes in cable order `0, 1, 2, 1, 0, 2`. Two successive completions are
queued before a service pass. Distinct channel/controller bytes and monotonically
increasing per-cable values expose cross-cable contamination as well as loss,
duplication and reordering.

The test queues 40 events per cable before reading, drains cable 1 completely,
drains cable 2 partly, receives more interleaved traffic, then drains cable 0.
Further traffic takes all three FIFO tails across wraparound while other cables
retain older events. Exact payloads and per-cable/aggregate depths are checked
through the final zero-depth state. A fresh generation tests device-wide arrival
order, then mixed cable-specific reads followed by the earliest remaining
device-wide events, with no repeats.

`check_three_cable_overflow_isolation()` fills cable 0 to its 64-event capacity,
then supplies interleaved packets on all three cables. It checks one initial
boundary-overflow episode and exactly 40 discarded cable-0 packets, including
faulted packets arriving after the accepted prefix has drained. Cables 1 and 2
are drained independently in both directions of the experiment. Neither gets a
false fault. Cable-0 close/reopen preserves the others' queued data, clears its
active fault and retains cumulative lower counters; new cable-0 data is read in
order. Receiver state, exact accepted/consumed counts, drained completion/FIFO/
staging queues and unchanged stream-start count are asserted. Ordinary
`make check` prints `USB MIDI three-cable integration: PASS` when it succeeds.

The fixture exercises the production receive callback; it does not inject
application FIFO events directly. It simulates the interface
`xhci_bulk_in_stream_read()` provides **after** completion handling. It cannot
prove actual transfer-event routing, DMA coherency/ownership, ring wrap on the
controller, interrupt delivery, real device jack associations, USB/DIN timing or
physical multi-cable operation. Those require separate target/hardware evidence.

## Automatic application receive verifier

The diagnostic verifier in [`console_midi.c`](../src/console_midi.c) consumes
through the existing opened logical IN handle and `midi_port_read()`. It does not
read the USB transport directly or open a replacement generation. The main loop
calls `console_midi_service()` after USB service and before console polling;
automatic mode consumes at most 32 events per iteration without printing,
delaying, blocking or recursively servicing USB.

Implemented console commands:

```text
midi port verify start <id> <count>       # manual mode
midi port verify start <id> <count> auto  # continuous bounded receive
midi port verify drain [1..64]           # manual only; default 64
midi port verify status
midi port verify stop
midi port verify reset
```

The port must already be a bound, opened IN definition. Start also requires empty
input queues/pending ABORTs, retired events and receiver FIFO/staging; several of
these checks cover the whole device/definition, not only the chosen cable.

Every observed event advances the expected pattern: **channel 16, CC 102, values
0 through 127 cycling**, beginning at zero. The verifier records total observed,
mismatches and the first mismatch's one-based position, expected value and
actual event. Realtime/SysEx/unexpected messages are events too, and do not get
silently filtered out. Extras can match the cyclic pattern with zero mismatches
and still fail by count.

`EMPTY` is idle. Disconnection, stale permission and other terminal read/service
statuses freeze distinct terminal outcomes; `DATA_LOST` is a terminal read
failure. Handle inspection can detect a loss before the accepted prefix is
consumed; this verifier is not a general drain-after-loss utility.

Automatic mode keeps looking for extras after the expected count. Status can
report FAIL while still consuming; reaching the count alone is not PASS. Stop
checks the original handle, stops consumption and freezes the result. **The sender
must have finished and the receive path must have drained before stop.** Stop
itself does not drain remaining events. PASS requires exact count, zero
mismatches and a valid original handle at the stop decision. Missing events yield
INCOMPLETE; excess/mismatching events yield FAIL. Later input/handle invalidation
does not change the frozen result. No receiver can prove that a sender will not
emit future messages after this boundary.

Manual mode preserves its older exact-count PASS behaviour on status without
requiring stop, so it does not prove absence of later extras. Manual drain is
rejected during running automatic verification. Reset terminates either mode and
clears verifier state without closing the application's port or draining its
queues. Stop likewise does not close the port.

## Physical CME test procedure

Use the supported CME USB-MIDI interface path (the source specifically contains
CME U2MIDI Pro descriptor/initialisation handling), an external MIDI sender and
appropriate MIDI cabling. Record actual model(s), firmware, topology, cable
number, generation and kernel revision with results. This repository does not
provide an automated physical-results ledger; no new physical PASS is implied by
these instructions. Multi-cable correctness above is host-tested, not a claim of
multi-cable physical CME validation.

For a 600-message receive check:

1. Boot `kernel8.img`, use the UART console to inspect the device generation, and
   provision/open a logical IN cable. For example, replacing `G` with the current
   generation and choosing an unused logical ID:

   ```text
   midi port bind 1 in G 0
   midi port open 1
   midi port status 1
   midi input status G
   midi rx status G
   ```

2. Ensure earlier input and pending/retired events are handled and queues are
   empty. Do not run an independent diagnostic receiver/read or another consumer
   on the tested stream. Arm before starting the external sender:

   ```text
   midi port verify start 1 600 auto
   ```

3. Send exactly 600 channel-16 CC-102 messages, values `i & 127` for `i = 0..599`,
   at 10 ms spacing. Do not issue manual drain commands. Wait for the sender to
   finish and allow foreground receive processing to reach the expected count
   with empty queues. Inspect status/queue snapshots, then stop and inspect:

   ```text
   midi port verify status
   midi input status G
   midi rx status G
   midi port verify stop
   midi port verify status
   midi port status 1
   ```

4. Expect frozen PASS, 600 observed, zero mismatches and no terminal failure.
   Inspect input per-cable fault/overflow/drop counters, malformed counts,
   pending ABORTs and queue depths; inspect receiver rejected counts, FIFO/staged
   depth and hardware queue/peak/pauses/failed state. Compare cumulative values
   with the baseline rather than assuming a fresh boot. A clean normal run
   should not introduce loss, malformed/rejected packets or hardware failure;
   record pauses and peaks, not just the verifier verdict. Idle-discard counts
   can legitimately reflect traffic sent before subscription.

Output tests require observing the external receiver as well as local queue/
completion statistics. `midi output status G` and `midi output losses` distinguish
local acceptance, USB confirmation and abandoned packets; none proves external
MIDI delivery. Physical unplug/replug, overflow recovery and extra/missing-message
runs should record their own expectations rather than being labelled normal PASS.

## Target-only synthetic diagnostics

Existing console commands include `midi input selftest`, `usb test stopout`,
`usb test faultpaths`, `usb test routefault`, and related EP0/disable diagnostics.
`usb test stopout` includes synthetic Bulk OUT completion validation for full
success, nonzero residual, wrong TRB, failed code and late uncertain completion.
These firmware self-tests are not all linked into `make check`, and should not be
reported as host-executed xHCI coverage. Low-level MIDI stream/stress diagnostics
have explicit ownership and can conflict with an application receiver; use them
in separate, controlled runs.
