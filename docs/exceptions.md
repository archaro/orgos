# Recoverable exceptions (H2.1a and H2.1b)

The kernel can return from its deliberately limited EL1 `SVC` diagnostic.
This establishes the mechanism for preserving an interrupted context.
H2.1b adds the [bounded EL0 diagnostic](el0-testing.md), using the same
frame for lower-EL synchronous exceptions and IRQs. There is no general
syscall interface or program loader.

## Saved frame and return contract

`src/exceptions.h` defines `exception_frame_t` and the assembly offsets;
C static assertions check the layout. Current-EL SP_EL1 synchronous and
IRQ entry (vectors 4 and 5), and lower-EL AArch64 synchronous and IRQ entry
(vectors 8 and 9), allocate this same 304-byte frame on the EL1 stack,
keeping SP aligned to 16 bytes.

| Offset | Contents |
| --- | --- |
| 0..240 | x0..x30, eight bytes each |
| 248 | Interrupted EL1h or EL0 SP (read-only snapshot) |
| 256 | ELR_EL1 (return PC) |
| 264 | SPSR_EL1 (return processor state) |
| 272 | ESR_EL1 (entry syndrome) |
| 280 | FAR_EL1 (not meaningful for every exception) |
| 288 | Vector number |
| 296 | SP_EL0 (restored on return) |

Entry saves every GPR before using scratch registers or calling C. The
frame belongs to the assembly entry stub and is valid only until dispatch
returns. The dispatcher receives its address; the current IRQ dispatcher
accepts the same frame but does not inspect it.

The common return path writes ELR_EL1 and SPSR_EL1 from the frame, restores
x0..x30, releases the frame, and executes `ERET`. SP is restored by releasing
the frame, rather than loading its diagnostic snapshot. SP_EL0 is saved
and restored separately, including during EL1 IRQs. Handlers must not
unmask exceptions. FP/SIMD state is outside this contract; reachable C code
must retain the build's `-mgeneral-regs-only` constraint. Return supports
EL1h and AArch64 EL0t; EL1t is not implemented.

## Dispatch and fatal diagnostics

The existing EL1 diagnostic requires all of:

- Vector 4 (current EL using SP_EL1).
- An AArch64 SVC syndrome with immediate `0x21`.
- Saved processor mode EL1h.
- ELR exactly equal to `exception_test_resume`, the instruction immediately
  following the diagnostic's SVC.

The dispatcher leaves the frame unchanged. SVC already saves the address
of the following instruction, so advancing ELR would skip an instruction.
The active EL0 harness owns only its exact SVC and privilege-fault sites,
with the expected vector, mode, syndrome and test stage. Any synchronous
exception outside these two diagnostics, including another SVC site or
immediate, a BRK, an undefined instruction or an abort, calls
`exception_panic` with the saved entry registers. Other unsupported
vectors retain their direct fatal path. The existing vector, exception class, ESR, ELR, FAR and SPSR
diagnostics and masked halt remain intact.

## Validation

`make check` includes a host test of production dispatch. It verifies that
the expected trap leaves the entire frame untouched and that unsupported
vectors, exception classes, modes, SVC immediate, instruction length and
return address reach the fatal handler with the original diagnostic
arguments. This does not execute the entry assembly or `ERET`.

Build with `make`, boot the image on the Raspberry Pi 4B with the existing
UART console, then:

1. Run `irq` and `uptime` to establish normal console operation.
2. Run `svc`. Expect:
   `EL1 SVC context test: PASS (x0-x30, SP, NZCV, DAIF)`.
3. Repeat `svc` several times. The assembly diagnostic seeds all GPRs with
   distinct 64-bit patterns, sets NZCV to `0xa0000000`, records SP and the
   incoming DAIF masks, and checks their values immediately after return.
   A nonzero mismatch mask reports x0..x30 in bits 0..30, SP in bit 31,
   NZCV in bit 32 and DAIF in bit 33.
4. Run `irq` and `uptime` again. UART input must still work, time must
   advance, and interrupt handling must continue. Exercise existing MIDI
   traffic if connected, since IRQ entry now shares the frame.

For an optional destructive diagnostic check on a disposable test image,
change only the test instruction to `svc #0x22`, rebuild and run `svc`.
Expect a vector 4 panic reporting SVC and ESR `0x56000022`, followed by
`System halted.` Restore the instruction and rebuild before normal use.
This confirms the live fatal path; the host test only mocks its endpoint.

The cross-build and host tests cannot establish successful trap return on
hardware. The `svc` command tests EL1 only. Use the separate `el0` procedure to
exercise the lower-privilege boundary.
