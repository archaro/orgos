# OrgOS — Project Vision and Development Roadmap

## 1. What OrgOS is

OrgOS is an independently developed, bare-metal operating system for the
Raspberry Pi 4B, written primarily in GNU17 C.

Its first application is a configurable touchscreen MIDI controller for a
custom-built organ console using Hauptwerk.

However, OrgOS is intended to become a genuine, more general-purpose operating
system, with a **microkernel architecture as its long-term design objective**.

The organ controller supplies the initial practical requirements. It does not
define the permanent limits of the operating system.

Writing the operating system is itself an important objective of the project.
Much of the implementation is therefore deliberately developed from first
principles, while retaining the freedom to use established standards, sound
algorithms and appropriately licensed third-party code.

**Guiding principle:** Build a useful application while allowing the operating
system to develop independently, through small, complete and architecturally
coherent increments.

## 2. Development model: three independent horizons

Development is organized into three horizons:

- **Horizon I — Application:** Deliver the configuration-driven touchscreen
MIDI controller.
- **Horizon II — Architecture:** Evolve the existing implementation towards a
maintainable, microkernel-based operating system.
- **Horizon III — Expansion:** Explore wider operating-system capabilities
beyond those immediately required by the organ controller.

These horizons are **distinct but not sequential**.

Work may proceed on any horizon at any time, according to available hardware,
developer interest, technical opportunities and immediate requirements.

For example, while awaiting touchscreen hardware, development may concentrate
on an isolated kernel mechanism or a general operating-system facility.

### Bounded work units

Every excursion into any development horizon must:

1. Have a clearly defined objective.
2. Have an explicitly limited scope.
3. Establish testable acceptance criteria.
4. Be independently reviewable.
5. Leave OrgOS buildable and operational.
6. Preserve established interfaces unless a complete, validated transition is
part of the work.
7. Reach a definite stopping point before another substantial undertaking
begins.

A completed, bounded work unit on one horizon must never prevent continued
progress on either of the others.

Where an architectural change would require coordinated modifications across
several subsystems, it must either be implemented as a complete, tested unit
or broken into compatible intermediate stages.

Experimental work should be isolated so that unsuccessful experiments can be
abandoned without destabilizing the main development line.

A horizon is not a milestone that must be completed before another horizon
begins.

**Progress on one horizon should enable progress on the others, never hold it
hostage.**

## 3. Architectural objective: a microkernel

OrgOS should evolve towards a microkernel architecture.

The intended distinction is between a small privileged kernel implementing
essential system mechanisms and separately executing services responsible for
device management, application facilities and higher-level policy.

### Candidate kernel responsibilities

Subject to design and experimentation, the privileged kernel will eventually
provide mechanisms such as:

- Execution-context and thread management.
- Scheduling.
- Address-space management and memory protection.
- Interprocess communication.
- Interrupt delivery and routing.
- Controlled access to hardware resources.
- Minimal facilities required to bootstrap system services.

The precise division of responsibilities is not yet fixed.

In particular, the placement of memory allocation policy, device-memory
mapping and other services should be evaluated rather than assumed.

### Candidate external services

As the architecture matures, suitable components may include:

- Device drivers.
- USB host and device management.
- MIDI services.
- Display and graphics services.
- Input services.
- Storage and filesystem services.
- Configuration services.
- Application lifecycle management.

Not every component must necessarily become an independent process.

Separation should be justified by protection, fault isolation, reuse, clarity
or other measurable benefits.

### Incremental evolution

The current OrgOS kernel contains drivers and services that may ultimately
execute outside privileged kernel space.

This is acceptable at the present stage.

We should progressively establish the interfaces and mechanisms needed to move
components across protection boundaries without requiring a wholesale rewrite.

Possible intermediate steps include:

1. Clarifying existing module and resource ownership boundaries.
2. Separating mechanism from policy.
3. Defining stable service interfaces.
4. Introducing independent execution contexts.
5. Implementing appropriate IPC mechanisms.
6. Establishing address-space isolation.
7. Migrating selected services out of privileged execution.
8. Measuring the resulting complexity, latency and reliability.

These are possible areas of work, not an obligatory implementation sequence.

### What constitutes a microkernel

Good modular organization is necessary but not sufficient.

A system whose drivers are separate C files but all execute with unrestricted
kernel privileges is not thereby a microkernel.

The architectural goal requires meaningful execution and protection
boundaries, explicit resource authority and communication mechanisms that
support independently executing components.

### Performance and practicality

The architecture must remain appropriate for a Raspberry Pi 4B and its
applications.

IPC overhead, interrupt latency, scheduling behaviour and the cost of
protection boundaries should be evaluated empirically.

The microkernel objective should guide architectural decisions without
becoming an excuse for premature complexity.

## 4. Horizon I — The organ-console application

The first application is a touchscreen control surface for a Hauptwerk-based
organ.

The planned hardware includes:

- Raspberry Pi 4B.
- 9.3-inch, 1600 × 600 HDMI touchscreen.
- USB capacitive touch interface.
- CME U2MIDI Pro.
- A separate Hauptwerk computer.

The physical console includes keyboards, drawstops, rocker tabs, pistons and
expression pedals, much of it using midi-hardware.com devices, with some
custom-built components.

OrgOS supplements these physical controls with configurable virtual stops and
other MIDI controls.

### Registration ownership

Hauptwerk remains authoritative for registration and combination memory.

OrgOS sends MIDI messages requesting stop changes and receives messages
describing changes made elsewhere, including through physical pistons.

Bidirectional MIDI synchronization is therefore essential.

The application's displayed state must not assume that an outgoing MIDI
message guarantees the external state has changed.

### Configuration-driven operation

The touchscreen interface will be described by configuration data loaded from
the SD card.

Configuration should ultimately define:

- Screens and navigation.
- Control placement and appearance.
- Labels and visual states.
- Outgoing MIDI messages.
- Incoming MIDI state mappings.
- Startup and application settings.

The application must be configurable without recompiling OrgOS.

Initially, use a straightforward human-editable format. More sophisticated
configuration capabilities should follow demonstrated requirements.

### Principal outstanding work

- HDMI display initialization.
- Basic 2D rendering and text.
- USB touchscreen support.
- Input-event processing.
- Elementary graphical control framework.
- SD-card configuration loading.
- Configuration parsing and validation.
- Graphical control integration with the MIDI APIs.
- Incoming MIDI synchronization.
- Full integration with the organ console and Hauptwerk.

**Horizon I milestone:** OrgOS boots into a functioning configuration-driven
touchscreen MIDI controller.

Where practical, the graphical application should use interfaces compatible
with the intended microkernel direction, even if its first implementation
executes within the present kernel.

It must not depend unnecessarily on privileged access to low-level hardware.

## 5. Horizon II — Operating-system architecture

This horizon develops the mechanisms and internal organization needed to
transform the existing implementation into a maintainable microkernel-based
system.

Potential work includes:

- Execution contexts and scheduling.
- Privilege levels and protected address spaces.
- IPC and service discovery.
- Resource handles and capabilities.
- Memory allocation and mapping services.
- Interrupt routing across protection boundaries.
- Driver isolation.
- Service lifecycle and restart mechanisms.
- Fault reporting and recovery.
- Consistent hardware-driver interfaces.
- Improved separation between applications, services and kernel mechanisms.

A useful architectural experiment may be pursued even if no current organ-
controller feature requires it, provided it is bounded, independently testable
and does not obstruct other development horizons.

### Existing implementation

OrgOS already includes substantial working infrastructure:

- AArch64 initialization and exception handling.
- Firmware Device Tree processing.
- Memory discovery, MMU and page allocation.
- DMA allocation and cache synchronization.
- BCM2711 PCIe support.
- VL805 xHCI support.
- USB hub management and device lifecycle.
- Supported USB-MIDI input and output.
- MIDI parsing, logical ports and ownership.
- Interrupt handling, including MSI.
- UART diagnostics.
- Host-side tests.

This infrastructure is an asset to preserve, not something to discard simply
because the final architecture will differ.

It is not necessary to preserve compatibility, but controlled replacement is
to be preferred over broad rewrites.

### Microkernel-oriented design decisions

For new facilities, consider:

- Whether the component conceptually belongs in privileged code.
- What resources it requires.
- Who owns those resources.
- How another execution context could invoke it.
- Which aspects are policy rather than mechanism.
- How failures can be contained.
- Whether its interfaces remain usable after migration.

These considerations should influence design without forcing immediate
implementation of IPC or process isolation.

## 6. Horizon III — Wider operating-system capabilities

OrgOS may eventually support applications entirely unrelated to MIDI or
musical instruments.

Potential developments include:

- Multiple independent applications.
- Broader graphics and input capabilities.
- General-purpose storage services.
- More extensive hardware support.
- Application installation and management.
- More sophisticated service isolation.
- Multi-user operation.
- Security and permission management.
- Additional system interfaces and programming facilities.

These are areas for exploration, not commitments to particular
implementations.

Horizon III can progress independently where a proposed experiment is useful,
bounded and compatible with the work underway elsewhere.

### Possible multi-user model

Should multi-user operation become relevant, investigate a model in which
effective authority depends on both:

- The permissions or capabilities granted to a user.
- The access layer or execution context in which those permissions are
exercised.

The resulting authority should be appropriately constrained by both.

This reflects an interest in ICL VME's hierarchical access-layer approach, not
a requirement to reproduce its historical implementation.

### Broader architectural influences

The developer's experience includes OpenVMS, ICL VME and Acorn RISC OS,
together with an appreciation of Plan 9's design philosophy.

These influences concern interface coherence, system organization,
transparency and clarity.

They should not be interpreted as instructions to copy historical kernel
architectures or programming practices.

Use modern designs where they are better supported by evidence.

## 7. Existing engineering debt and deferred investigations

Some technical questions remain intentionally unresolved.

These should be documented and revisited without automatically blocking
application or architectural development.

Examples include:

### High-address PCIe DMA integrity

The PCIe implementation uses a 4 GiB inbound BAR2 aperture with a 3 GiB
software DMA allocation ceiling.

Address translation and ordinary USB-MIDI functionality have been tested.

Exact-address page allocation and targeted DMA-buffer allocation have been
implemented and host-tested.

Actual hardware DMA integrity at representative high physical addresses
remains unverified.

A proposed diagnostic harness requires direct-buffer xHCI transfers and an
independently verifiable USB test fixture.

The fixture work is deferred pending additional hardware.

### SuperSpeed USB topology

The current USB implementation supports the established internal-hub USB-MIDI
path.

SuperSpeed topology discovery and enumeration are outstanding.

Do not assume general USB 3.0 device compatibility.

### Additional USB and MIDI hardware testing

More extensive testing with multiple interfaces, ports, cables and
simultaneous devices remains useful.

Prioritize these investigations according to demonstrated need and development
opportunity.

Deferred investigations must distinguish untested behaviour from known
defects.

## 8. Development principles

### Small, complete changes

Prefer narrowly scoped, independently testable work units.

Do not combine unrelated architectural experiments and application changes
unnecessarily.

### Explicit ownership

Memory, DMA buffers, device resources, handles, interrupts and service state
must have clear ownership and lifecycle rules.

This is particularly important for eventual protected execution contexts.

### Mechanism and policy separation

Keep broadly useful system mechanisms separate from the policies of individual
applications.

Avoid embedding organ-specific behaviour in low-level drivers.

### Stable interfaces

Treat existing public interfaces as contracts.

Architectural migration must not casually invalidate consumers or introduce
incomplete cross-subsystem dependencies.

### Observable behaviour

Provide diagnostics sufficient to understand the state of hardware, services
and resources.

Distinguish successful software operations from independently verified
hardware outcomes.

### Evidence-based engineering

Distinguish established hardware behaviour, documented specifications,
hypotheses and experimental results.

Do not conceal unexplained failures behind undocumented workarounds.

### Appropriate licensing

Preserve OrgOS's permissive licensing strategy.

Do not introduce incompatible GPL-derived source code into the implementation.

Maintain upstream notices where adaptation is permitted.

### Practical simplicity

Avoid unnecessary abstractions, frameworks and speculative generality.

Equally, avoid short-term shortcuts that create avoidable obstacles to the
intended microkernel architecture.

## 9. Instructions for Codex

When undertaking OrgOS development:

1. Inspect the latest repository commit and relevant documentation.
2. Identify which horizon the proposed work primarily serves.
3. Define a bounded objective and explicit stopping point.
4. Identify dependencies and possible effects on other horizons.
5. Prefer compatible changes over disruptive redesign.
6. Preserve existing functionality and ownership contracts.
7. Add focused host-side tests where useful.
8. Distinguish host simulation from physical hardware validation.
9. Explain architectural implications and unresolved risks.
10. Do not continue into adjacent work without approval.
11. Do not commit or push unless explicitly instructed.

A task may serve more than one horizon. That is desirable when it produces
clean, reusable infrastructure.

However, no task should expand its scope merely because it reveals
opportunities for broader architectural work.

Where an implementation cannot be completed without disruptive, unfinished
dependencies, stop and propose a smaller compatible work unit.

## 10. The destination

The near-term destination is a working touchscreen organ controller.

The architectural destination is a small, capable microkernel operating
system whose drivers and higher-level services can operate through well-
defined, protected interfaces.

The longer-term destination is deliberately open.

These goals are complementary and may be pursued concurrently.

OrgOS should grow through practical applications, architectural
experimentation and carefully bounded engineering work.

**No development horizon is subordinate to another, and none may prevent
progress on the others.**

And, eventually, Doom.
