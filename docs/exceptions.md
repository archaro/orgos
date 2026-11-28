# Exception frames, diagnostics and context fault containment

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
The legacy `el0` harness owns only its exact SVC and fault sites, with the
expected vector, mode, syndrome, fault address and stage. An owned running
context instead saves its frame and returns to the kernel on yield, exit
or synchronous fault termination. A fault marks only that selected context
FAULTED and never resumes its faulting instruction. Kernel faults, SError
and exceptions outside the owned transition retain fatal diagnostics.
The existing vector, class, ESR, ELR, FAR and SPSR diagnostics and masked
halt remain intact.

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

The [two explicit contexts](el0-two-contexts.md) keep independent copies
of the common frame in kernel storage. The live entry frame retains its
short lifetime; no persistent context stores a kernel continuation pointer.
The active slot and borrowed kernel return pointer exist only during one
explicit resume transition and are cleared before foreground execution
continues. Shared return assembly restores a copied context on resume.
There are no general resume-after-abort cases. The two contexts now use
independent TTBR0 hierarchies with a common stack VA and distinct physical
backings. Persistent TTBR1 supplies a
shared privileged upper range; see the address-space and virtual-stack
guides for the implemented isolation and its limits.


## H2.6f2: one synchronous entry through high vectors

`kmap vectors test` temporarily installs the existing table's higher-half
alias in VBAR_EL1, reuses the known EL1 SVC 0x21/site, and restores the
original low VBAR before restoring DAIF. TTBR1 remains persistently active;
normal kernel execution and the 16 KiB EL1 stack remain low. The complete
[implementation and Pi procedure](higher-half-kernel.md) distinguish this
bounded experiment from permanent vectors or higher-half IRQ verification.

The frame layout and SAVE_FRAME/ERET contract above are unchanged. After
all registers have been saved, the sync stub obtains its actual executing
VA with ADR and passes it as a second argument to a low C observer. The
observer records it only while this diagnostic is armed. Otherwise it
forwards the untouched frame to the original dispatcher.

Entry assembly uses literal-loaded, absolute LOW C targets and BLR rather
than relative BL. This prevents high assembly from accidentally entering
C at the high alias, where linker-symbol comparisons could disagree with
the saved low ELR. The literal pool is part of the validated RO/RX extent;
relative table/return branches preserve the alias. Linker assertions check
2 KiB table alignment and sixteen 128-byte slots. Low IRQ/lower-EL entry
continues calling the same C handlers and common return path.

The armed observer requires one exact vector-4 SVC, EL1h, expected flags
and masks, fixed low continuation and actual high ADR site. It verifies
hardware translation of that observed address and the live temporary VBAR
and preserved translation regime. Unexpected entry evidence is fatal.
The existing dispatcher leaves the full saved frame unchanged; ERET
returns low. The original register test plus an additional SP_EL0 sentinel
check must report zero mismatches. Bits 0..33 retain their existing meaning;
bit 34 identifies an SP_EL0 mismatch. The launcher restores the caller's
original SP_EL0, and the controller verifies final architectural state.

No arbitrary exception is made recoverable. Precondition rejection makes
no VBAR write; failure after transition is fatal, with asynchronous
exceptions masked. The normal persistent-owner rule still requires low
VBAR; it is not widened to accept a new permanent vector regime. The busy
interval excludes other MMU operations until verified low VBAR restoration.

Host tests cover controller ordering, all DAIF masks, exact evidence and
rejection/fatal paths. Cross-build and linked assembly audit cover branch,
literal, ADR and return encodings. Physical high synchronous entry remains
pending until the documented Pi test passes. IRQ, FIQ and SError are masked
throughout this experiment; later low-vector IRQ/USB/MIDI regressions do
not prove higher-half IRQ entry. Permanent high vectors require separate
live IRQ/lower-EL/nested-exception review and compatible activation guards.
