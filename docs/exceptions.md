# Exception stacks after H2.6f5

Normal execution, EL1 exceptions and EL0 kernel continuations now use the
high alias of the same linker-reserved 16 KiB EL1 stack. High vectors
remain permanent; ordinary instructions and global pointers remain low.
The earlier stage descriptions below are historical and their low-stack
and temporary-stack runtime assumptions are superseded here.

The boot-only handoff is one-way: validated EL1h with DAIF masked, fresh
high SP, cleared FP/LR and a branch to a non-returning low continuation.
There is no frame copying, low return chain or post-switch rollback.
BOOT_LOW, TRANSITION and HIGH are explicit owner states; ordinary runtime
guards reject TRANSITION and, after commit, reject every low or unrelated
high SP. Physical translation and exact translation-register/VBAR contracts
remain enforced.

Exception assembly still allocates/restores the same 304-byte frame with
SP-relative accesses and returns with ERET. Frame layout and recoverable
fault authorization are unchanged. An interrupted EL1 SP is high; an
interrupted EL0 SP remains a user virtual address. The actual kernel
handler SP and saved kernel continuation use the permanent high stack
across either context's TTBR0 switches. Unexpected faults remain fatal.

`kmap vectors test` reports the actual C-handler SP and translated PA of
SP-16. The genuine IRQ recorder captures actual handler SP with its frame;
foreground validation requires exact high bounds and the same hardware
translation relation. The IRQ hot path performs no new page-table walk.
The test does not infer stack use from the installed upper root.

`kmap stack` inspects live high SP and its translation. `kmap stack test`
now performs nested calls on that permanent stack, rather than switching
back to low SP. The old temporary-stack controller is retained only as a
historical native regression; its production buffer binding is removed.

See [the current stack contract and cold-boot procedure](higher-half-kernel.md)
for the full transition, state/translation checks, sync and real IRQ
expectations, EL0 fault regressions and device tests. Host simulation and
image compilation pass; physical H2.6f5 verification remains for the user.

---

## Historical stages through H2.6f4

# Higher-half mappings and controlled exception entry

OrgOS now enables its existing higher-half hierarchy during MMU startup
and leaves it active. Ordinary kernel code, data pointers and stack remain
low. H2.6f3 keeps VBAR_EL1 permanently at the validated higher-half vector
alias. Every TTBR0 environment
still carries its complete privileged identity mappings. Persistent mapping
availability does not mean that kernel execution has migrated high.

`kmap` inspects descriptors and live registers without changing them.
`kmap test` executes the existing bounded leaf through its higher alias,
verifies its returned PC, physical translation and result, then returns low
with TTBR1 still enabled. `kmap vectors test` now checks a controlled EL1
SVC without changing permanent high VBAR. `kmap vectors irq arm` enables
bounded actual-IRQ recording; `kmap vectors irq` validates its evidence.
H2.6f3 physical synchronous/IRQ verification is pending. The Pi
procedures below distinguish expected results from physical observations.

## Geometry and architectural basis

The lower configuration remains 4 KiB, 36-bit VA and 36-bit PA, starting
at L1. MAIR remains `0xff4400`:
Device-nGnRnE, Normal NC and Normal WB in slots 0, 1 and 2. Live TCR is
now the dual-region value `0x00000001b519351c`, with TTBR1 walks enabled.
L2 blocks cover 2 MiB and L3 pages 4 KiB.

The TTBR1 configuration uses **upper 39-bit VA**, the same 36-bit
PA limit and 4 KiB granule, starting at L1. Canonical upper addresses occupy:

```text
0xffffff8000000000 .. 0xffffffffffffffff
```

Bits [63:39] must all be one; bits [38:0] form the 39-bit offset. L1 uses
[38:30], L2 [29:21], L3 [20:12], and the page offset is [11:0]. A full
512-entry L1 page represents 512 GiB. This avoids a partial initial table
and comfortably includes the CPU's high PCIe MMIO physical window. A
lower address with the same low bits is rejected by the upper walker;
noncanonical prefixes and tagged addresses are rejected, not masked into
an apparently valid mapping. The existing lower walker still accepts only
its original 36-bit range.

`mmu_geometry_t.region` explicitly distinguishes TTBR0 and TTBR1. Only the
two required geometries are supported. Descriptor encoding and physical
validation remain common. Complete mapping ranges are checked using the
canonical offset, including the final page ending at `UINT64_MAX`; no
exclusive-end addition may wrap into lower addresses. Conflicts still
reject the entire request before any PTE write.

The upper space stores its required dual-region TCR metadata with T1SZ=25,
TG1=0b10 (4 KiB), SH1=0b11, IRGN1=ORGN1=0b01 and EPD1=0. T0SZ=28, lower
walk fields and IPS=0b001 are retained; TBI0/TBI1 and ASIDs remain
zero. The combined runtime value is `0x00000001b519351c`. TG1's encoding
is deliberately different from TG0's 4 KiB encoding. The Cortex-A72 TRM
describes the same setting as bit 31 RES1 and TG1 bit 30 clear. This
metadata is never written to TCR_EL1 by construction or inspection.

See the [Arm memory-management guide][arm-memory] for address ranges,
translation levels and maintenance, and the
[Cortex-A72 TRM, section 4.3.41][arm-tcr], for TCR field encodings. The
table representation is TTBR1-compatible. H2.6e implements the bounded
execution mechanism. H2.6f1 retains that probe under the persistent regime;
Raspberry Pi verification of this change is pending.

[arm-memory]:
  https://documentation-service.arm.com/static/68e680a2587fd957f5efd742
[arm-tcr]:
  https://documentation-service.arm.com/static/5e7b6c287158f500bd5bfb61

## Fixed-offset layout and permissions

The higher kernel VA is `0xffffff8000000000 + physical_address`, with
physical output unchanged. The conversion helper returns a number, never
a usable kernel pointer; conversion alone does not prove that RAM exists
or that an entry is mapped. Only 36-bit PA inputs are accepted.

| Resource | Higher VA range or rule |
| --- | --- |
| Discovered RAM below 8 GiB | Base + physical bank ranges |
| Low kernel pages | Base + linked physical section addresses |
| BCM2711 peripherals | `0xffffff80fc000000..0xffffff8100000000` |
| PCIe outbound MMIO | `0xffffff8600000000..0xffffff8604000000` |

Ranges above use exclusive ends. RAM holes remain invalid. Peripheral
entries within the first 8 GiB window are Device, never Normal RAM;
RAM/MMIO overlap in the physical inventory is rejected. The unused upper
region remains unmapped. The user stack VA `0x800000000` is not part of
this upper hierarchy.

The same boot-policy construction code builds both lower and upper plans,
selecting a region/offset and privileged attributes. It reuses the existing
descriptor, link, mapping and inspection mechanisms. The lowest 2 MiB is
represented by 512 L3 pages, replacing a block rather than aliasing it:

| Physical resource | AP | PXN | UXN | Memory |
| --- | --- | --- | --- | --- |
| Kernel text, including vectors | 10 | 0 | 1 | WB, inner-shareable |
| Kernel rodata | 10 | 1 | 1 | WB, inner-shareable |
| Existing EL0 executable page | 10 | 1 | 1 | WB, inner-shareable |
| Data, BSS, tables and stacks | 00 | 1 | 1 | WB, inner-shareable |
| Other RAM | 00 | 1 | 1 | WB, inner-shareable |
| Peripheral/PCIe MMIO | 00 | 1 | 1 | Device-nGnRnE, non-shareable |

All mappings have AF set and deny EL0 access. There is exactly one
upper VA per mapped PA. Kernel text and rodata do not also acquire a
writable linear-map alias, nor does writable memory gain an executable
alias. The EL0 code page remains RO but is not executable through this
privileged upper mapping. Existing lower mappings are retained separately;
they are not modified to activate or accommodate TTBR1. During the
persistent overlap both aliases retain consistent memory types and
write/execute protections.

## Ownership and boot lifecycle

The existing eleven upper table pages remain static, aligned kernel BSS:
one L1, eight RAM L2s, one PCIe L2 and one low-RAM L3. CPU table pointers
remain low identity aliases; registered physical addresses are encoded in
links and TTBR1. The physical inventory reserves all storage through the
existing kernel-image bounds. Total table storage stays 48 pages (192 KiB).
The 44 KiB exact sealed snapshot also remains in static BSS. There is no
allocation, reclamation, new table page or physical-page policy change.

The existing H2.6e controller is the sole higher-half owner. Admission,
performed immediately after construction, requires SEALED, exact geometry,
permissions, attributes, root and distinct table ownership. It records an
exact descriptor snapshot and pinned registrations. Before activation it
revalidates every descriptor against the low boot policy and snapshot;
no extra writable or executable aliases are admitted.

Boot first enables the original low regime with the unchanged assembly
cache/barrier sequence. Low code, data, stack, vectors and table aliases
are then accessible and the kernel object is ACTIVE. While IRQ delivery
is still masked, the same higher-half controller validates live anchors,
executes the low baseline and performs persistent activation. Only after
successful proof does `mmu_init` admit the three TTBR0 environments and
return success. Page-allocator and GIC/UART interrupt setup follow; USB
startup remains later in the established boot path.

The upper lifecycle is EMPTY -> BUILDING -> SEALED -> ACTIVE. ACTIVE never
returns to SEALED during normal operation. Tables, registry and owner have
kernel lifetime. Inactive init/link/map/seal/build APIs reject the active
object before modifying a descriptor. Raw EL1 corruption remains outside
the trusted kernel contract; snapshot/policy checks detect changes before
subsequent diagnostic or TTBR0 use. EL0 has no access to these pages.
There is no teardown API, root replacement or competing upper controller.

### Persistent activation sequence

The initial live TCR is `0x000000010080351c` (EPD1 set). With DAIF saved and
all asynchronous exceptions masked, activation requires the admitted kernel
TTBR0, compatible MAIR, MMU/cache/WXN enables, EL1h, linked low PC, aligned
low SP and aligned low VBAR. AT S1E1R/W checks vector/stack/code access and
the CPU-to-PA registration of every upper table page. Both probe aliases
must be privileged RO/RX, Normal WB, inner-shareable and EL0-inaccessible.

Activation reuses the enable half of the H2.6e sequence:

```text
DSB SY
write TTBR1_EL1 = registered higher root PA  (EPD1 still set)
ISB
write TCR_EL1 = 0x00000001b519351c
ISB
TLBI VMALLE1
DSB SY
ISB
verify live state, hardware translations and higher-half execution
mark the upper owner persistently ACTIVE
restore and verify the original DAIF
```

Only T1SZ, EPD1, IRGN1, ORGN1, SH1, TG1 and TBI1 are replaced in the saved
TCR. T1SZ=25, TG1=0b10, SH1=3, IRGN1=ORGN1=1, EPD1=0 and TBI1=0; IPS
remains 36-bit and every TTBR0 field remains unchanged. TTBR0, MAIR, SCTLR,
VBAR, SP and SP_EL0 are preserved. Live MAIR stays `0xff4400`.

The first DSB establishes table visibility. ISBs synchronize register
changes and the local TLBI is completed by DSB/ISB before reliance on the
new regime. Tables and memory attributes are unchanged; no live descriptor
replacement or break-before-make operation is introduced. Register
encodings follow the [A72 TRM, sections 4.3.41 and 4.3.46][arm-tcr];
maintenance follows the [Arm memory-management guide, section 8][arm-memory].
Core 0 is the only running core, as enforced by bootstrap. The local flush
is not a multicore shootdown protocol.

An already-persistent owner is revalidated and probed without translation
register writes. Unexpected pre-existing state is rejected, rather than
adopted as a new baseline. No automatic disablement or restoration to the
unused reset-time TTBR1 value occurs in this mode.

## TTBR0 switching under the dual regime

The three lower hierarchies keep their exact original construction
profiles and full compatibility checks. Their `tcr` metadata describes the
lower-only base profile, not the live dual TCR. Runtime validation requires
the exact combined configuration and pinned upper root; it does not simply
accept either TCR or ignore upper fields.

Before every TTBR0 selection, the production hardware boundary checks the
current lower root, exact dual TCR, exact upper root, unchanged MAIR, pinned
SCTLR/VBAR, low execution anchors and EL1h. The upper owner's snapshot and
full policy are revalidated as well. All three lower environments retain
their existing ownership, permissions and per-context stack checks.

The existing root replacement remains:
`DSB SY; MSR TTBR0_EL1; ISB; TLBI VMALLE1; DSB SY; ISB`. It never writes
TTBR1, TCR, MAIR or SCTLR. The full flush may discard cached upper
translations, but their unchanged root/tables remain available for refill.
A new post-write check verifies the intended root and complete dual regime
before publishing the lower ACTIVE state. A failed preflight makes no root
write; a failure after a root write is fatal with asynchronous exceptions
still masked. Original DAIF restoration on successful selection is
unchanged.

Both EL0 contexts retain the common stack VA `0x800000000` with separate
physical backings. Their code, stack permissions, register preservation
and owned exception/fault containment paths are unchanged. Return to the
console restores the kernel TTBR0 while retaining the same TTBR1. The upper
object is outside the admitted lower pool and cannot be selected as TTBR0.

## Higher-half execution diagnostic

`kmap` reports ACTIVE/persistent, live TTBR0/TTBR1/TCR, required dual TCR,
walk enablement and root/configuration agreement. Its kernel, vector,
rodata, data/BSS, stack, table and MMIO samples still use software descriptor
inspection. Samples PASS and register agreement are not execution proof.
The command snapshots registers before/after and makes no state changes.

`kmap test` validates the persistent environment and runs the same 24-byte
assembly leaf through low and high aliases. The leaf uses MOV, MUL, ADR
and RET only: no memory operands, literals, globals or external calls.
It computes 6 times 7 and reports its own instruction-label VA and SP.
BLR from the low wrapper supplies the known low LR; RET returns directly
to that low wrapper. Ordinary C never executes through a high alias.

PASS independently requires:

- Result 42, exact higher ADR label, and actual AT S1E1R translation of
  that returned VA to the expected physical code label.
- Successful high entry/site read translations and a level-3 permission
  fault for its write translation. AT reports through PAR without a target
  load or store.
- A valid low baseline, zero x19-x29/SP/NZCV failures, unchanged aligned
  before/after/probe SP and the exact low return label. The wrapper restores
  its caller's saved registers/flags; x0-x18 follow AAPCS64 caller-save rules.
- Identical original/final TTBR0, TTBR1, TCR, MAIR, SCTLR, VBAR, SP and
  SP_EL0; low continuation, ACTIVE upper ownership and exact DAIF restoration.

No TCR or TTBR1 write, upper disablement, or TLB invalidation is needed by
a successful persistent probe. Output explicitly identifies PERSISTENT mode
and PRESERVED (TTBR1 remains enabled). Instruction bytes and physical
outputs are immutable and retain matching RO/RX Normal-WB attributes at
both aliases. The A72 instruction cache behaves as PIPT (TRM section 6.3);
no new I-cache maintenance is introduced. Boot/cache maintenance and the
established UART-sensitive cache sequence remain unchanged.

### Historical temporary experiment

H2.6e enabled TTBR1 only around the leaf, then restored the original TCR,
invalidated translations and restored the old opaque TTBR1 with walks
already disabled. That path remains covered by host tests on a separately
admitted, nonpersistent fixture and reports TEMPORARY (H2.6e). The current
boot-bound owner becomes persistent before the console starts, so ordinary
`kmap test` never enters that temporary path. There is no console disable
or mode-change command.

## Exceptions, failures and device contracts

Boot activation masks asynchronous exceptions across validation, register
changes and execution proof. The old mask state is restored exactly before
normal startup continues. Low VBAR and the normal 16 KiB EL1 stack are
unchanged. IRQ/MSI handlers continue using their low mappings under every
TTBR0 root; their code and driver interfaces are unchanged.

Precondition rejection leaves translation registers unchanged. Following a
boot-time architectural change, failed proof or changed kernel state is a
fatal kernel diagnostic, not an attempted rollback. In an established
persistent regime, bad diagnostic evidence can report FAIL only after
confirming the low environment and unchanged dual configuration. Failed
architectural readbacks or DAIF restoration are fatal. Unexpected CPU
exceptions retain the existing fatal dispatcher; no recovery rule is added.

Physical allocation continues returning PAs. Kernel table/data pointers,
DMA CPU aliases, PCIe bus translation and device MMIO remain low. DMA
ownership, cache synchronization and USB/MIDI foreground servicing are
unchanged. Console output occurs after the masked probe. Repeat Pi testing
with MIDI between calls is required to establish timing and delivery.
A hardware fault/hang still needs a reboot; host simulation cannot establish
physical transition safety. Image growth may alter reservation counts and
linker addresses; the existing first-2-MiB link assertion remains in force.

## Historical H2.6f2: temporary higher-half exception entry

`kmap vectors test` is an explicit foreground, core-0 diagnostic. The
persistent upper hierarchy and its normal low-VBAR contract are unchanged.
Boot does not install high vectors. TTBR0 switching, EL0 execution and
ordinary kernel pointers retain their established behaviour.

### Assembly suitability and the low C boundary

All sixteen slots remain 128 bytes each in a 2 KiB-aligned table. Linker
assertions enforce alignment and total width. Their relative branches
continue reaching the same entry-stub offsets through either alias. The
existing SAVE_FRAME and common restore/ERET sequence are unchanged.
They access the existing low EL1 stack through SP-relative operands.

A relative BL into C would also retain the high alias. Ordinary C is not
relocatable merely because its instruction pages are mapped there: symbol
comparisons could use the high alias while saved ELR contains a low
continuation. The necessary narrow change is five explicit C call bridges
in the shared entry assembly. Each loads an immutable nearby literal
containing the linker-resolved LOW target, then uses BLR x16. x16 and LR
are already saved on recoverable paths. Panic stubs retain their existing
fatal argument convention. IRQ and lower-sync stubs use the same bridge;
ordinary low entries still call exactly the same low C routines.

The literal pool is explicitly included in `exception_vector_code_end`.
Every vector, stub, restore instruction and literal byte is inside that
validated assembly extent. Table branches and literal loads are local;
absolute 64-bit target relocations select the intended low C addresses
without a high-to-low branch-range dependency or linker veneer. The linked
image is audited for all sixteen branches, ADR, literal targets and ERET.
This is not a higher-half C execution experiment.

After SAVE_FRAME has stored every interrupted GPR, the current-EL sync
stub executes `ADR x1, exception_sync_observed`. The observed value is
passed to a low C observer with the existing frame pointer in x0. Outside
the armed diagnostic the observer forwards the frame unchanged to the
existing dispatcher and records nothing. The 304-byte frame layout and
its ownership/return contract are unchanged.

### Preflight and transition

The controller first saves DAIF and masks all asynchronous exceptions.
It requires the persistent owner's exact root, TCR, MAIR, SCTLR, low VBAR,
EL1h and low execution anchors, plus the established kernel TTBR0. The
upper snapshot and complete lower/upper permission policy are revalidated.

The low vector address must equal the installed, pinned VBAR and be
2 KiB-aligned. The derived high VA is canonical under the existing upper
39-bit geometry and must map to the original physical table. Software
inspection and AT S1E1R/W verify both ends of every page intersecting the
whole assembly/literal extent at both aliases: correct PA, privileged RO,
executable, UXN, AF and Normal WB/inner-shareable. C bridge targets and the
low SVC continuation must be executable low text; the low stack must
translate as writable. Rejection restores DAIF without any VBAR write.

Only after this validation does the controller mark the existing owner
busy and arm one synchronous observation. It performs:

```text
save DAIF; mask asynchronous exceptions; validate the persistent regime
arm the exact diagnostic
MSR VBAR_EL1, validated high vector VA
ISB
read back VBAR and all preserved translation/low execution state
run the existing EL1 SVC context test with the additional SP_EL0 check
verify the observed high entry and safe low continuation
MSR VBAR_EL1, original low vector VA
ISB
verify exact restoration and the persistent owner's original contract
disarm; release busy state; restore and verify original DAIF
```

VBAR selection is synchronized by ISB before use. These immutable mappings
require neither TLBI nor cache maintenance for the VBAR change. The
[AArch64 Exception Model, section 5][arm-exceptions] describes vector
selection and return; Arm's [exception-level setup example][arm-vbar-isb]
also shows VBAR followed by ISB. TTBR0, TTBR1, TCR, MAIR and SCTLR are never
written by this controller. The persistent owner is not repinned to accept
a high VBAR, and context switching cannot occur inside its busy interval.

[arm-exceptions]:
  https://documentation-service.arm.com/static/67ac57fb091bfc3e0a9479cc
[arm-vbar-isb]:
  https://support.arm.com/documentation/102437/0100/Changing-Exception-levels

### Exact entry evidence and context preservation

The trigger reuses `exception_test`: SVC immediate 0x21, vector 4, EL1h,
and the exact low `exception_test_resume` continuation. It seeds x0-x30
with distinct 64-bit values, sets NZCV and checks GPRs/SP/NZCV/DAIF after
ERET. A small low launcher additionally saves SP_EL0, seeds it with
`0x5a5a1000`, checks it after the exception and restores its original value.
The sentinel is a register value, never a new stack or a memory access.
Failure bits retain the original meanings 0..33; bit 34 adds SP_EL0.

The armed observer accepts only one entry with the exact vector, complete
SVC syndrome, EL1h state, expected NZCV/DAIF, low continuation and aligned
interrupted stack, with the SP_EL0 sentinel preserved. The reported ADR
value must equal the exact high `exception_sync_observed` address. Actual
hardware AT of that observed VA must resolve to the original physical
instruction address. It also checks live high VBAR and unchanged TTBRs,
TCR, MAIR and SCTLR from within the low C handler. The saved frame must
remain byte-for-byte unchanged across the original dispatcher.

C returns to the high assembly continuation; the common high restore tail
reloads the frame and ERET resumes the known low SVC continuation. The
controller requires zero context mismatches, unchanged low SP/SP_EL0 and
architectural state, then restores and verifies the original low VBAR.
Only afterwards are ordinary asynchronous exceptions unmasked.

An unexpected armed vector, syndrome, mode, site, duplicate observation,
wrong physical translation or corrupted saved state is a fatal kernel
failure. So are missing evidence, context mismatches, changed registers,
failed VBAR restoration or failed DAIF restoration after transition.
There is no arbitrary exception recovery or fallback to a guessed regime.
Unexpected CPU exceptions retain the existing fatal diagnostics.

### Limitations and permanent-vector prerequisites

Only current-EL SP_EL1 synchronous entry/return is exercised high. IRQ,
FIQ and SError remain masked; high IRQ delivery is not verified. Existing
EL0 and interrupt tests run after low VBAR restoration. The normal stack,
C handlers, kernel pointers and linked code remain low; all lower kernel
mappings remain required. Core 0 is the only active core.

Permanent high vectors would need a separate contract for the persistent
owner/TTBR0 validator, live IRQ/MSI and lower-EL entry testing, fatal and
nested exception review, and continued low C/stack availability or an
explicit migration of those dependencies. This task stops at temporary,
verified high synchronous entry and low VBAR restoration.

## H2.6f3: permanent vectors and bounded IRQ evidence

This is the current runtime configuration. The H2.6f1/f2 descriptions and
Pi procedures below record their earlier stages; instructions that expect
low VBAR restoration no longer apply to the current kernel.

### Installation and ownership

After the original low MMU setup and validated persistent TTBR1 activation,
`mmu_init` invokes the existing vector controller in permanent mode. The
entire table, entry/return assembly and literal pool, low C targets, stack,
physical outputs and protections undergo the H2.6f2 preflight. Installation
uses exactly MSR VBAR_EL1 followed by ISB, verifies the installed value and
runs the controlled SVC proof. A failed proof after installation is fatal.
No TTBR, TCR, MAIR, SCTLR or PTE is written by vector installation.

The same static upper owner then pins the vector manifest and exact high
VBAR. Its normal guard now requires that high address and the registered
low physical vector address; it does not generally accept either alias.
The low-VBAR boot phase is accepted only before this validated publication.
Repeated installation and ordinary vector tests validate/probe without
VBAR writes. There is no public uninstall or alternate vector owner.
The original temporary path remains in host tests on a fresh nonpermanent
fixture, not as an ordinary runtime command mode.

Installation completes before TTBR0 environment admission and before page,
GIC, UART interrupt or USB startup. Bootstrap still uses low vectors before
TTBR1 is ready. Normal startup subsequently enables GIC UART IRQ 153 and
unmasks IRQs with high VBAR already installed. Priorities, targets, source
preparation, acknowledgement and end-of-interrupt policy are unchanged.
The bootstrap still parks other cores; this is a single-core contract.

TTBR0 preflight and post-write verification now check the exact high VBAR,
unchanged TTBR1 root and dual TCR. Vector PA bounds are checked against low
text, while VBAR itself is canonical high. The existing higher-half leaf
probe translates high VBAR to its pinned PA rather than assuming identity.
Both independent EL0 roots retain privileged low kernel mappings and use
the same upper vectors. Neither a root switch nor an owned EL0 return
reinstalls low VBAR. The low C call bridges, stack and common frame/ERET
remain essential and unchanged in meaning.

### Real IRQ observation

The EL1h IRQ stub (vector 5, slot offset 0x280) and lower-AArch64 IRQ stub
(vector 9, offset 0x480) each capture their own actual instruction address
with ADR after SAVE_FRAME. Both pass the saved frame and actual PC to a
low C observer through the existing absolute-low literal bridge. No
interrupted GPR is used before it has been saved.

When unarmed, the observer forwards to the original IRQ dispatcher. It
does no system-register snapshot, frame copy, AT query or output. When
armed, it records at most one acknowledged IRQ per path in fixed BSS
storage. Each record contains the observed PC, frame snapshot, saved mode,
PC/SP/SP_EL0, live VBAR/TTBR0/TTBR1/TCR, actual acknowledged GIC source and
IRQ counts before/after the existing handler.

The existing dispatcher body now has a source-returning adapter while
retaining its original void interface. Both interfaces use exactly one
IAR acknowledgement, the same UART/MSI handlers and full-token EOI. The
unexpected-source masked halt is unchanged, extracted into a hardware
boundary for host testing. Spurious source 1023 performs no EOI and does
not satisfy or consume an armed observation.

Only armed observations compare the saved frame across dispatch and check
that the handler preserved architectural state. Wrong entry sites/modes or
changed frames/state are fatal. No source acknowledgement, UART handling,
MSI quiesce/rearm or device-driver policy is replaced by the observer.
There is no IRQ printing and no AT translation in the IRQ hot path.

Foreground arm/read operations mask DAIF while resetting/copying the
records. Foreground verification requires the actual ADR site, correct
vector/mode, a supported acknowledged source (153 or 180), one handled IRQ
count increment, completed handler/EOI, expected high VBAR/root/TCR and an
admitted interrupted TTBR0. The recorded SP/return PC must fit its EL1 or
EL0 path. Hardware AT of the observed high PC must resolve to the exact
physical IRQ instruction. Raw observed values are distinct from computed
expected values. Incorrect or missing evidence cannot produce PASS.

Handler completion is recorded before ERET. The next foreground report
shows ordinary execution resumed; it is not a separately instrumented
post-ERET register snapshot. Both paths still restore all GPRs, SP_EL0,
ELR and SPSR through the common unchanged assembly tail. The armed frame
comparison, existing register tests, EL0 progress and actual continued
console/device operation provide the corresponding preservation checks.

### Source choice and EL0 limitations

The supported repeatable source is real UART receive IRQ 153. Arm the
recorder, wait for the prompt, then send another command. Its incoming
characters generate a physical UART interrupt through the GIC; the
command does not directly call the IRQ entry path. The timer currently
provides a polled clock, not periodic interrupts, so waiting for uptime to
advance is not IRQ-generation evidence. No new timer driver is introduced.
The existing `pci msi test` can additionally exercise GIC software-pending
IRQ 180, and existing physical MSI diagnostics retain their own procedures.
Those sources must be labelled separately from real UART/device events.

EL0 contexts inherit the kernel mask on initial dispatch; the transition
masks its critical return-state preparation and ERET restores the saved
masks. This change adds no special EL0 unmasking, busy loop or scheduler.
Normal console-driven entry can permit IRQ delivery, but the payload
slices are very short. A vector-9 capture is reported separately and must
actually occur before EL0 IRQ PASS is claimed. EL1 IRQs between resumptions
are not evidence of EL0 IRQ delivery. If no vector-9 interrupt is captured,
record EL0 as NOT OBSERVED, even when both contexts and every EL1 test pass.

### Host evidence

The native vector test retains H2.6f2 rejection/restoration coverage and
adds permanent installation, idempotence, all DAIF combinations, exact
high-VBAR guards, repeated TTBR0 switches and the higher-half leaf probe
under high VBAR. Actual production IRQ dispatch is exercised with mocked
GIC/UART/PCIe hardware: spurious handling, UART/MSI accounting, full-token
EOI, unarmed dispatch, one-shot EL1h and EL0 captures, exact PC translation,
wrong/missing evidence and fatal changed-frame/state paths.

The full EL0 lifecycle/fault fixture now installs permanent vectors with
the production controller before exercising independent roots, yields,
exits, register/stack checks, aliases, peer/kernel faults and unexpected
exceptions. Payload instructions and IRQ delivery in native tests are
simulated. Cross-build/link assertions and a linked audit check all vector
slots, both actual IRQ ADR sites, low C literals and ERET. None of these
host results proves real Pi IRQ entry or physical USB/MIDI delivery.

### Current Raspberry Pi procedure

**H2.6f3 hardware verification is pending.** Save the old image, deploy
`kernel8.img`, cold-boot with the established firmware/serial/USB-MIDI setup
and retain a full log. Expect:

```text
Initialising MMU... OK (persistent TTBR1/high vectors; kernel executes low).
```

Require normal page/GIC/UART startup, USB enumeration and a responsive
console. Run these lines separately:

```text
kmap
kmap vectors
kmap test
kmap vectors test
kmap vectors
svc
```

Require persistent TTBR1 ACTIVE/ENABLED, root agreement, mapping Samples
PASS and live TCR `0x00000001b519351c`. Vector inspection reports
`Permanent higher-half vectors: ACTIVE`, with Live VBAR equal to Required
VBAR and `0xffffff8000000000 + Vector table PA`, 2 KiB-aligned. It does
not trigger an exception. The leaf returns 42/PASS with TTBR1 preserved.

The synchronous diagnostic must report PASS, PERMANENT mode, vector 4,
ESR `0x56000021`, exact observed high entry and matching hardware PA,
zero context failures, Vector configuration preserved YES, persistent
TTBR1 preserved and original/final DAIF identical. Original, Installed and
Final VBAR must all be the same HIGH address. The original `svc` test must
still pass all x0-x30/SP/NZCV/DAIF checks through permanent high vectors.
Repeat both tests at least five times with MIDI I/O between calls.

For actual asynchronous entry, send each line separately and wait for its
prompt. Do not paste arm and query as one buffered UART packet:

```text
kmap vectors irq
kmap vectors irq arm
kmap vectors irq
```

Before arming, empty records report NOT OBSERVED, never PASS. Arming reports
`IRQ observation ARMED`. Typing the next query produces fresh UART input.
Expect its completed report to include:

```text
Higher-half IRQ observations (armed hardware entry)
  EL1h IRQ: PASS
  ... observed/expected high entry VA and observed PA ...
  Vector: 5; GIC source: 153
  ... saved SPSR, interrupted PC/SP and entry roots/VBAR ...
  IRQ count: N -> N+1
  Handler/EOI completed: YES
  Foreground resumed; common ERET restores the saved frame.
  EL0 IRQ: NOT OBSERVED
```

Observed and Expected entry VA must match, and Observed entry PA plus the
high base must equal that VA. Entry VBAR must equal the permanent high
VBAR; Entry TTBR1 must equal the persistent root; Entry TTBR0 for this
foreground test must equal K. Saved mode must be EL1h (low mode bits 5),
interrupted PC and SP low, SP aligned. A counter increment alone is not
sufficient: the ADR, PA, vector, source and handler evidence all must pass.
If no suitable interrupt arrived, send another query or re-arm and type
again; retain NOT OBSERVED until actual evidence exists.

Repeat arm/query at least five times, checking `irq`, `uptime`, UART
overruns and bidirectional physical MIDI between calls. To exercise the
existing MSI/GIC tests, use their established `pci msi ...` procedure.
Ordinary UART input used to type those commands may fill the first EL1
record before source 180 arrives; do not relabel a source-153 record as MSI
or treat GIC software pending as a physical PCIe MSI event.

Exercise contexts under high VBAR:

```text
el0
el0ctx reset 0
el0ctx reset 1
el0ctx start 0
el0ctx start 1
kmap vectors irq arm
el0ctx resume 0
el0ctx resume 1
el0ctx status
kmap vectors irq
kmap vectors
```

Legacy `el0` must pass its existing calculation, SVC, system-register and
three memory-permission tests with zero masks. Both contexts must be
YIELDED at 7 with stack/mapping/switching OK and zero mismatch masks. Keep
MIDI/UART activity running and alternate five more resumptions each; both
must progress 14/21/28/35/42 and EXITED/PASS with six resumptions/five yields.
Interleave vector/leaf probes and vector inspections. Both context roots
and backings remain distinct; kernel TTBR0 returns to K every time.

If EL0 IRQ evidence appears, require EL0 IRQ PASS, vector 9, saved mode
EL0t (0), observed PC matching the separate lower-IRQ high ADR site and
hardware PA, Entry TTBR0 equal to a context root, and saved PC/SP belonging
to the EL0 code/common virtual stack. Subsequent context progress and
register/stack checks must still pass. If it remains NOT OBSERVED, record
that explicitly; neither interleaving nor UART IRQs taken in EL1 establish
physical EL0 IRQ coverage. No artificial long-running EL0 window is added.

Repeat the existing `alias`, `cross` and kernel-data `fault` procedures for
both contexts, including a peer remaining YIELDED. Faults must remain
contained with expected site/FAR, zero mismatches and restored K; high
VBAR/root/TCR must remain fixed. Reset both slots before legacy `el0`.

Finally run the established memory/page/cache/DMA/PCIe regressions:
`memory`, `physmem`, `cache`, `page test`, `page runtest`, `pci dma test`,
`dma owner`, `dma cache`, `dma edge`, plus the supported USB/MSI diagnostics.
Require their established PASS results, enabled caches, advancing IRQ
counts/uptime and zero UART overruns. Confirm real MIDI delivery in both
directions after sustained activity and faults. Finish with `kmap vectors`,
`kmap vectors test`, a fresh IRQ arm/query and `kmap test`.

Record EL1 synchronous, actual EL1 IRQ and actual EL0 IRQ results
separately, alongside USB/MIDI and DMA results. The required genuine IRQ
observation is a Pi result still awaiting your run; host simulation is not
substituted for it.

### Remaining dependencies

VBAR and the shared assembly are high, but the C handlers, normal kernel
stack, data pointers, allocation/DMA aliases, MMIO and all linked kernel
code remain low. Identity mappings and replicated privileged TTBR0
mappings are retained. A later kernel-stack migration must audit trusted
continuations and SP-relative frame construction/return for EL1 and EL0,
stack bounds in every activation guard, IRQ/nested/fatal entry, DMA/cache
VA contracts and boot timing. It must have its own bounded validation;
this change does not move the stack or add scheduling/preemption.

## H2.6f4: controlled high-stack execution

`kmap stack test` temporarily uses a dedicated higher-half stack for a
bounded nested assembly routine. It runs only on explicit request, never
at boot. The normal kernel stack, bootstrap SP setup, exception vectors,
TTBR0 switching and EL0 stack semantics are unchanged.

### Existing assumptions and reserved layout

The linker still reserves a 16 KiB normal EL1 stack, and boot initializes
SP_EL1 from the linked low `__stack_top`. Ordinary C and assembly use
AAPCS64 calls with 16-byte stack alignment. Current MMU/vector/probe guards
require low PC and the normal low stack. Exception entry subtracts the
304-byte frame from the current SP; return releases that same frame and
ERET restores the continuation. Owned EL0 entry retains a trusted EL1
frame and copies its selected user frame below it. There is no separately
allocated IRQ stack or supported nested-IRQ policy.

The experiment reserves five aligned BSS pages through the existing kernel
image reservation: one 4 KiB diagnostic stack and a separate 16 KiB witness
buffer. The witness is not a second execution stack. Both are static and
remain unavailable to the page and DMA allocators. No allocator policy or
active PTE changes are introduced. CPU aliases and registered PAs are
represented separately; boot binding registers their established low
identity aliases and proves CPU-to-PA correspondence with hardware AT.

The existing higher-half policy already maps these BSS bytes privileged
RW, PXN/UXN, AF, Normal WB and inner-shareable. All lower context mappings
must deny EL0 access. Full kernel/context policy validation, explicit BSS
bounds, alignment, registered-table overlap checks and disjoint buffer
ranges reject unsuitable storage. Normal EL1 and both EL0 stacks lie
outside this BSS range. No dynamic stack allocation exists.

For physical base P, the page layout is:

```text
high base = 0xffffff8000000000 + P
base + 0x000..0x00f : lower boundary canaries
base + 0x010..0xf8f : patterned, unused bytes
base + 0xf90..0xfaf : nested 32-byte frame
base + 0xfb0..0xfef : outer 64-byte frame (including checked unused slots)
base + 0xff0..0xfff : upper boundary canaries
usable high SP top = base + 0xff0
```

The backing has its full low alias for initialization and post-return
verification. Linker addresses can move; use reported P rather than a
hard-coded PA. The original first-2-MiB image assertion remains in force.

### Preflight, transition and restoration

With original DAIF saved and all asynchronous exceptions masked, the
controller requires the admitted kernel TTBR0, persistent TTBR1 and exact
permanent VBAR. It validates the current low SP, room for the wrapper,
normal stack length, low code range and both buffer registrations. Software
inspection and AT reads/writes cover each page boundary at the required
aliases, exact PA, privileged RW/XN attributes, and the normal stack and
high vector translation. Rejected preconditions restore DAIF without
changing SP, any translation register, VBAR or backing bytes.

The controller patterns the entire diagnostic page, marks the existing
upper owner busy and calls a small assembly wrapper from the normal stack.
The wrapper saves x19-x30, x18, NZCV and DAIF in a 128-byte low frame. It
copies the complete active interval from that frame through normal stack
top into the static witness. That includes its own saved state and every
live caller frame, but not irrelevant dead bytes below the active interval.
All probe output goes to a separate BSS record while the low frames are
frozen; no C output/report buffer on that stack is modified in this window.

The wrapper retains trusted low-SP/report/witness anchors, installs the
validated high top, then calls the low-linked outer routine. Outer pushes
a 64-byte FP/LR frame, stores operands 6 and 7 and two local guards, and
calls inner. Inner pushes a 32-byte FP/LR frame, loads the outer operands,
stores/reloads them through its own SP, multiplies to 42 and records its
actual SP. Outer verifies its locals and result after the nested return.
The outer and inner SP observations come from running instructions, not
from a calculation based only on the installed value.

After outer returns, the wrapper is still on the high stack. It checks
its trusted anchors, expected high SP and unchanged masked DAIF, then
compares the full frozen low interval byte-for-byte with the witness. A
failed anchor/mask/low-frame check calls the fatal kernel boundary; it does
not reconstruct or resume a guessed low continuation. Only an exact match
allows low SP restoration. Saved callee registers, LR and NZCV are then
restored before normal return. Seeded x25-x28 and restored FP are also
checked across the nested code; x19-x24 carry separately checked anchors.
The controller cross-checks the recorded FP/status against the witness.

The [AAPCS64 general-register and stack rules][stack-aapcs] define the
callee-saved registers and alignment. The 128/64/32-byte frame changes keep
SP 16-byte aligned throughout this experiment. The wrapper additionally
preserves incoming NZCV, though flags are not an AAPCS64 public-interface
preservation requirement. The high interval contains no successful-path C
call, SVC, IRQ unmask, allocation, device access or mapping change. Code
continues at its original low addresses; only stack data uses TTBR1.

[stack-aapcs]:
  https://github.com/ARM-software/abi-aa/blob/main/aapcs64/aapcs64.rst

### Independent execution and integrity evidence

After safe return to the normal stack, the controller checks exact
original/final SP, SP_EL0, TTBR0, TTBR1, TCR, MAIR, SCTLR, VBAR and EL1h
state. It revalidates the ordinary persistent-owner contract, which still
rejects high SP for normal operation. No broad high-SP exception is added
to any activation guard; those guards are never called during the high
assembly interval. Nested diagnostic requests are rejected while busy.

Actual AT of the observed outer and lowest SP must resolve to P+0xfb0 and
P+0xf90. The observed VAs must equal the reported high top minus 64 and 96.
These translations are execution evidence distinct from the preflight
translation of the proposed page. The controller checks both boundary
canaries and every page word: exact FP/LR pairs, operands, local guards
and unchanged patterned bytes everywhere else. Merely returning or passing
the boundary canaries is insufficient.

Nested result must be 42 with zero local/callee failures. A stack-content,
canary or SP-evidence mismatch may report FAIL only after verified low
context restoration. Changed architectural state, missing low-frame proof,
wrong return location or failed status/mask restoration is fatal. The
original external DAIF is restored and read back only on the normal stack.
All translation registers and permanent high VBAR remain unchanged.

### Interrupt limits and future migration

Asynchronous exceptions remain masked throughout the high interval. There
is no deliberate synchronous exception on the new stack. An unexpected
kernel exception remains fatal; there is no invalid-SP recovery. Passing
this experiment establishes ordinary nested stack use, not exception or
IRQ handling on a high stack. IRQ observations made before or after the
command still use the normal low kernel stack.

Permanent migration will require separate changes to bootstrap/normal
SP lifetime, every low-stack range guard, trusted EL0 kernel continuations,
frame copy/release/ERET paths, IRQ/nested/fatal stack usage, and any stack
address-to-PA assumptions in diagnostics or future DMA/cache interfaces.
It must also define stack ownership, capacity, guards and exception headroom.
This task neither converts ordinary pointers nor changes those contracts.

### Host evidence

`make check` adds the production stack controller with an instrumented
assembly boundary. It covers admission, alignment/backing/overlap,
missing/wrong mappings and permissions, context EL0 exposure, wrong roots,
VBAR and original SP, recursive activation, all sixteen DAIF states,
observed-SP/PA checks, complete-page/canary failures, low-context proof and
fatal state corruption. Native execution models the assembly footprint;
it does not switch the host SP or establish physical stack use.
The target build and linked audit cover the actual frame sizes, SP writes,
local/nested loads/stores, calls, saved registers, witness loops and return.
Existing EL0, vector/IRQ, allocation, DMA, PCIe and MIDI tests remain in the
full suite. Physical execution is pending the Pi procedure below.

### H2.6f4 Raspberry Pi procedure

**New high-stack hardware verification is pending.** Save the old image,
deploy `kernel8.img`, cold-boot the established Pi/firmware/serial/USB-MIDI
setup and retain a full console log. Expect normal persistent TTBR1/high
vector startup, USB enumeration and a responsive prompt.

Run:

```text
kmap
kmap vectors
kmap test
kmap vectors test
kmap vectors irq arm
```

Wait for the prompt, then type `kmap vectors irq` separately. Require the
existing inspection, leaf result 42, SVC/vector and actual UART IRQ tests
to pass with persistent high VBAR. Record EL0 IRQ coverage separately.
Then run:

```text
kmap stack test
```

Expect:

```text
Higher-half stack test: PASS
  Status: PASS
  Software/hardware preflight: PASS
  ... original low SP and proposed stack addresses ...
  ... installed/observed SPs and hardware PAs ...
  Nested result: 42 (expected 42)
  Local failures: 0x0000000000000000
  Callee register failures: 0x0000000000000000
  Boundary canaries: OK
  Complete page contents: OK
  Original stack witness: IDENTICAL (N bytes)
  ... original/final registers ...
  Low stack restored: YES
  Translation registers/VBAR preserved: YES
  ... original/final DAIF ...
  Exception masks restored: YES
```

Verify the relationships numerically, using reported physical base P:

- High base equals `0xffffff8000000000 + P`; usable/installed high top
  equals base+0xff0. Observed outer SP equals base+0xfb0; lowest observed
  SP equals base+0xf90. All are 16-byte aligned and inside the private page.
- Observed outer PA equals P+0xfb0 and lowest observed PA equals P+0xf90.
  NOT VERIFIED is a failure; proposed/preflight values alone do not pass.
- Original and final low SP are identical and within the normal kernel
  stack. The low return PC is linked low text; the original stack witness
  is IDENTICAL with a nonzero, 16-byte-multiple size at most 16 KiB.
- Original/final TTBR0 equal kernel K, TTBR1 equals the persistent root,
  TCR stays `0x00000001b519351c`, VBAR stays at the same high vector address,
  and original/final DAIF match. Both failure masks must be zero.

Repeat the stack command at least five times with physical MIDI send and
receive between calls. Then run `kmap vectors`, `kmap test`,
`kmap vectors test`, `svc`, a fresh IRQ arm/query, `irq` and `uptime`.
Require all established PASS results, advancing counters/time and zero UART
overruns. This is IRQ testing on the restored low stack, not the high one.

Reset/start both EL0 contexts and alternate six resumptions per slot,
inserting `kmap stack test` between calls, including while the peer is
YIELDED. Each must progress 7/14/21/28/35/42 and EXITED/PASS with five
yields, distinct roots/backings, common stack VA, intact stack and zero
mismatch masks. Repeat own-alias, peer-alias and kernel-data fault cases
for each ID, inserting stack tests between yield and fault. Faults must
stay contained and the peer and console remain operational. Reset both
slots before rerunning legacy `el0`.

Run the existing `memory`, `physmem`, `cache`, `page test`, `page runtest`,
`pci dma test`, `dma owner`, `dma cache` and `dma edge` diagnostics, requiring
their established PASS results and enabled caches. Confirm USB enumeration
and bidirectional physical MIDI after sustained activity and faults. End
with another stack test, vector inspection and genuine IRQ arm/query.
Malformed `kmap stack` or `kmap stack test extra` must print
`Usage: kmap stack test` without running anything.

Record actual Pi results separately from these expected outcomes. Do not
proceed to permanent stack installation or high-stack exception/IRQ tests
as part of this increment.

## Prerequisites for permanent higher-half migration

Persistent mapping availability is only one prerequisite. Kernel relocation
will need a separate transition with these invariants:

- The selected TTBR1 root PA and compatible MAIR/TCR fields must be valid,
  visible to walkers and configured with appropriate architectural ordering.
  TLB invalidation must complete with the required DSB/ISB sequencing;
  descriptor replacement needs applicable break-before-make rules.
- The running PC, return addresses, stack, VBAR and complete exception paths
  must remain accessible throughout. A high VBAR requires aligned, valid
  vector mappings and code/data references that work at their new VAs.
- Linked/absolute addresses, function pointers, assembly literals and saved
  EL1 continuations must refer to the intended kernel addresses. Merely
  enabling TTBR1 does not relocate a kernel linked at `0x80000`.
- Table CPU access must survive the transition while descriptors and TTBRs
  continue encoding PAs. Software table registrations will need suitable
  kernel mappings rather than pointer-to-PA assumptions.
- User TTBR0 roots can lose duplicated kernel mappings only after shared
  TTBR1 coverage and all entry/return paths are established. The current
  TTBR0 activation controller checks low PC/SP/VBAR and the exact dual TCR, and
  must not simply be used unchanged for that migration.
- Global mappings, eventual user ASIDs, stale translations and teardown
  need explicit ownership/invalidation rules. ASID allocation and multicore
  shootdown are not part of this experiment.

Remaining identity dependencies include early boot PC/SP and FDT handling;
linker bounds used as physical reservations; page-test PA-to-pointer casts;
DMA CPU aliases derived from PAs; mailbox/FDT physical interfaces; UART,
GIC, timer, PCIe and xHCI MMIO constants/register pointers; kernel table
registrations; static user-stack backing aliases; and diagnostic addresses.
Physical allocation must keep returning PAs. DMA descriptors/bus apertures
must keep bus addresses, while cache maintenance must receive CPU VAs.
The outstanding high-address DMA hardware integrity test remains separate
and unverified. This task does not change DMA translation, synchronization
or ownership contracts.

## Host evidence

Run `make check` and `make -B kernel8.img`. The focused native MMU fixture
uses production construction, upper ownership and lower activation code
with instrumented hardware operations. It covers rejection of malformed,
unsealed and changed hierarchies, exact enable ordering/values, all sixteen
DAIF states, repeated persistent/idempotent probes, active-table editing
rejection, distinct context backings, upper preservation across alternating
TTBR0 roots and fatal post-write mismatches. The historical temporary path
and its safe restoration/fatal failure boundaries remain covered.

The existing EL0 lifecycle/exception fixture now uses production upper and
lower MMU controllers and real constructed tables under a simulated dual
regime. It checks both contexts, all interleavings, register/stack state,
physical/peer alias faults, kernel-data faults and rejection of unexpected
exceptions. Payload instructions and hardware AT are simulated; this proves
the exercised controller/dispatcher contracts, not physical execution.
The rest of the suite still covers physical allocation, DMA, PCIe, USB,
MIDI and console behaviour. Cross-compilation/linkage proves buildability
and image bounds, not boot or physical I/O.

## H2.6f2 host evidence

`make check` adds the production vector controller and original dispatcher
under the shared instrumented MMU backend. Tests exercise all sixteen DAIF
states, exact VBAR-write/ISB ordering, observed-PC translation, unchanged
frames, SP_EL0 and persistent-root preservation, and rejected preconditions
without register writes. Wrong alignment, geometry/regime, root, mapping
permissions, PA and unavailable translations are rejected. Unexpected
vectors, syndromes, instruction length, modes, NZCV/DAIF, return/entry
sites, SP, SP_EL0, missing/duplicate observations and post-transition
invariant failures take fatal paths. Fatal cases model disposable boots in
child processes; they do not reset a failed transition into foreground.
Unarmed SVC dispatch and both existing EL0 suites remain covered.

The target build checks assembly/linkage and vector-table assertions. The
linked audit verifies relative branch targets at both aliases, actual ADR
encoding, five indirect low C calls with absolute low literals, and common
ERET. Host execution and this audit cannot establish physical exception
entry. H2.6f2 requires the separate procedure below.

## Historical H2.6f1 Raspberry Pi validation

**H2.6f1 hardware verification is pending.** These are expected outcomes,
not observed results. Use the established firmware, serial console and
USB-MIDI external peer. Save the previous image, deploy `kernel8.img` and
cold-boot; retain the full console log.

Expect:

```text
Initialising MMU... OK (persistent TTBR1; kernel executes low).
```

Require normal page initialization, USB enumeration and a responsive prompt.
Then run:

```text
kmap
kmap test
kmap
```

Both inspections must report:

```text
TTBR1 kernel hierarchy (software inspection only)
  State: ACTIVE; persistent
  ... roots, required and live TCR ...
  Live TTBR1 walks: ENABLED
  Live hierarchy matches: YES
  ... mapping samples ...
  Samples: PASS
  Live regime unchanged: YES
```

Live TCR and Required dual TCR must equal `0x00000001b519351c`.
Live TTBR1 must equal Hierarchy root PA, a stable page-aligned reserved PA.
Live TTBR0 must be the restored kernel root K. Unlike H2.6e, EPD1 must
remain clear throughout normal operation. Existing mapping attributes and
physical outputs must match the tables above.

The execution command must report:

```text
Higher-half execution test: PASS
  Status: PASS
  Mode: PERSISTENT (already enabled)
  Hierarchy validation: PASS
  ... addresses and original/final registers ...
  Calculation: 42 (expected 42)
  Register/stack failures: 0x0000000000000000
  Translation environment: PRESERVED (TTBR1 remains enabled)
  ... original and final DAIF ...
  Exception masks restored: YES
```

Verify the numeric evidence:

- Original, Enabled and Final TCR all equal `0x00000001b519351c`.
- Original/Final TTBR0 equal K; Original/Final TTBR1 equal the upper root.
- Expected observed VA equals Observed high PC and equals
  `0xffffff8000000000 + Low baseline PC`. It is twelve bytes after High
  entry VA, at the ADR instruction.
- Expected code PA, Observed code PA and Low baseline PC are identical;
  NOT VERIFIED is a failure. Addresses are link-dependent.
- Both failure masks are zero, Probe SP is low and 16-byte aligned, Low
  return PC is low text, and Original/Final DAIF match exactly.

Repeat `kmap test` at least five times with actual MIDI send/receive
between calls, then run `kmap` again. Require stable roots/code, PASS and
persistent ENABLED state every time. Unsupported `kmap activate` and
`kmap test extra` must print `Usage: kmap [test | vectors test]` without
changing state.

### EL0 contexts, protection and retained upper mappings

```text
svc
el0
el0ctx reset 0
el0ctx reset 1
el0ctx start 0
el0ctx start 1
el0ctx resume 0
kmap test
el0ctx resume 1
kmap
el0ctx status
```

`svc` must pass preservation checks. The legacy `el0` must return 42 and
verify its expected system-register and memory faults with zero mismatch
masks. Both contexts must be YIELDED at 7 with one yield/resumption,
stack/mapping/switching OK and zero context/kernel mismatch masks. Their
stack VA is the same `0x800000000`; backing PAs and roots are distinct.
Entry and trap TTBR0 equal the selected root; return TTBR0 equals K.

Alternate `el0ctx resume 0`, `kmap test`, `el0ctx resume 1`, `kmap` five more
times, interleaving MIDI I/O. Each context must progress 14, 21, 28, 35, 42
and finish EXITED/PASS with six resumptions and five yields. Upper root and
dual TCR must stay unchanged; inspection always reports ACTIVE/ENABLED.
Repeat reset/start/interleaving at least three times, including resetting
a completed slot while its peer remains YIELDED.

For each ID 0 and 1, run each of `alias`, `cross` and `fault` variants:

```text
el0ctx reset 0
el0ctx reset 1
el0ctx start 0 alias
el0ctx resume 0
kmap test
el0ctx resume 0
el0ctx status 0
kmap
```

Substitute the chosen ID and variant in start/resume/status. Each yields
once at 7, then becomes FAULTED with the expected permission fault, exact
site/FAR, preserved stack/registers and restored K. `alias` targets its own
physical stack alias, `cross` the peer backing, and `fault` kernel data.
A faulted slot must refuse further resume/start until reset. The peer and
console must remain operational. See the [virtual-stack guide] for the
full address and containment checks. TTBR1 must remain ACTIVE/ENABLED with
the same root/TCR after every fault. Reset both slots before legacy `el0`.

[virtual-stack guide]: el0-virtual-stacks.md

### Kernel, interrupt and physical device regressions

```text
el0
memory
physmem
cache
page test
page runtest
pci dma test
dma owner
dma cache
dma edge
irq
uptime
kmap test
kmap
```

Require the established PASS results, page/DMA ALL TESTS PASSED and PCIe
translation 10 passed/0 failed. Caches remain enabled, IRQ counts advance,
uptime increases and UART overruns remain zero. The final upper state
must still be ACTIVE/ENABLED with the same root and TCR. These existing
DMA diagnostics do not establish the separate deferred high-address DMA
hardware integrity test.

Confirm USB enumeration and actual MIDI delivery in both directions at
startup, between context slices/probes and after faults/regressions.
Use the established external-peer/MIDI procedure, including existing IRQ
and MSI diagnostics as applicable to that setup. Retain the raw results;
host PASS cannot substitute for these physical observations.


## Historical H2.6f2 Raspberry Pi validation

**New higher-vector hardware verification is pending.** Save the previous
image, deploy `kernel8.img`, cold-boot the established Pi/firmware/serial
and USB-MIDI setup, and retain a complete log. Expect normal MMU startup
with persistent TTBR1, page initialization, USB enumeration and console.

Run:

```text
kmap
kmap test
kmap vectors test
kmap
```

The existing inspection must report ACTIVE/persistent, ENABLED walks,
live hierarchy agreement YES and Samples PASS. The existing execution
probe must return 42/PASS and preserve TTBR1. The new command must report:

```text
Higher-half vector test: PASS
  Status: PASS
  Mapping validation: PASS
  ... original, temporary and installed VBAR ...
  ... expected and observed entry addresses ...
  Exception vector: 4
  ESR_EL1: 0x0000000056000021
  ... low continuation, SP and SP_EL0 evidence ...
  Context failures: 0x0000000000000000
  ... original/final registers ...
  Low vectors restored: YES
  Persistent TTBR1: PRESERVED (ENABLED)
  ... original and final DAIF ...
  Exception masks restored: YES
```

Check the evidence numerically:

- Original VBAR equals Vector table PA and is low and 2 KiB-aligned.
  Temporary and Installed VBAR both equal
  `0xffffff8000000000 + Original VBAR`.
- Expected entry VA and Observed entry VA match exactly and lie high in
  the entry stub. Observed entry PA equals Expected entry PA, and
  Observed entry VA equals base plus that PA. This capture is after the
  frame save, outside the first 2 KiB of vector slots; it is not fabricated
  from the installed VBAR.
- Vector is 4; ESR is `0x56000021`; saved NZCV is `0xa0000000`, mode EL1h
  and DAIF fully masked (normally SPSR `0xa00003c5`). The low continuation
  is the original SVC resume label. Interrupted SP is low and 16-byte
  aligned; SP_EL0 test seed is `0x5a5a1000`; Context failures is zero.
- Final VBAR exactly equals Original VBAR. Original/Final TTBR0 match K,
  Original/Final TTBR1 match the persistent root, and Original/Final TCR
  remain `0x00000001b519351c`. Original and Final DAIF match exactly.
- The following `kmap` still reports ACTIVE/ENABLED with the same upper
  root/TCR. Addresses can move with image growth; compare relationships.

Repeat the vector command at least five times, sending/receiving physical
MIDI between calls. Then run:

```text
svc
el0
irq
uptime
el0ctx reset 0
el0ctx reset 1
el0ctx start 0
el0ctx start 1
el0ctx resume 0
kmap vectors test
el0ctx resume 1
kmap vectors test
el0ctx status
```

`svc` must still pass its original x0-x30/SP/NZCV/DAIF check. Legacy `el0`
must pass result 42, SVC, register fault and all memory permission tests
with zero mismatch masks. Both contexts must be YIELDED at 7 with one
resumption and zero mismatch masks; their common stack VA must resolve to
distinct backings and roots. Alternate five more `resume 0`, vector test,
`resume 1`, vector test sequences. Both must independently reach
14/21/28/35/42 and EXITED/PASS with six resumptions and five yields.
Interleave physical MIDI I/O and `irq`/`uptime` checks.

Repeat the `alias`, `cross` and kernel-data `fault` procedures for each
context in the H2.6f1/virtual-stack guide, inserting a vector test between
the first yield and the faulting second resumption. Expected faults must
remain contained; the peer, console and MIDI must remain operational.
Every subsequent vector and execution probe must still pass. Reset both
contexts before rerunning legacy `el0`.

Run the established `memory`, `physmem`, `cache`, `page test`,
`page runtest`, `pci dma test`, `dma owner`, `dma cache` and `dma edge`
regressions. Require their established PASS results and enabled caches.
IRQ counts/uptime must advance, UART overruns remain zero, USB must
enumerate and physical MIDI must deliver in both directions after all
probes/faults. Finish with `kmap vectors test`, `kmap test`, `kmap`, `irq`.

Malformed `kmap vectors` and `kmap vectors test extra` must print
`Usage: kmap [test | vectors test]` without running a probe. Record the
actual Pi outcome separately. IRQ delivery between commands exercises the
restored LOW vectors; this procedure makes no claim about high IRQ entry.


## H2.6f4: stack experiment and exception boundary

`kmap stack test` uses a private 4 KiB BSS stack page through its high alias
for a bounded nested assembly routine. It preserves the ordinary low stack
and permanent high VBAR. No frame layout, IRQ path, low-stack range guard
or EL0 transition is changed. The ordinary kernel still uses its linked
16 KiB low stack between commands.

DAIF is saved and all asynchronous exceptions are masked before entering
the wrapper. There is no deliberate SVC/fault during high-stack use. The
normal exception entry always allocates on the current SP, but handling
an exception on this private page is not tested or claimed. Unexpected
synchronous exceptions remain fatal, without invalid-SP recovery.

A separate static 16 KiB witness captures the wrapper's saved frame and
live low caller frames. Before restoring SP the assembly checks its trusted
anchors and compares that interval exactly. FP/LR, callee-saved registers,
NZCV and the original masks are preserved; only a proven low continuation
can resume. Nested data, canaries and the complete page footprint are
checked after return, alongside actual translation of the observed SPs.
See the [high-stack procedure](higher-half-kernel.md) for implementation
and numeric Pi acceptance criteria; hardware verification is pending.

This does not establish exception/IRQ headroom on a high stack. Permanent
migration must separately review SP bootstrap/lifetime, activation bounds,
trusted EL0 continuations, frame copy/release, IRQ/nested/fatal entry and
stack-address/physical-address assumptions. Existing IRQ and EL0 regression
tests continue on the restored ordinary low stack.
