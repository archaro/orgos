# OrgOS

OrgOS is a small bare-metal operating system for the Raspberry Pi 4B. Its goal is
a touchscreen MIDI controller whose buttons and behaviour come from a
configuration file. The touchscreen application and configuration-file workflow
are planned; they are not implemented features.

The current implementation provides boot and FDT-based memory initialisation,
MMU/page allocation and DMA support, a UART diagnostic console, and PCIe/VL805
xHCI support for the existing internal-hub USB-MIDI path. MIDI includes USB-MIDI
packet encoding/decoding, independent receive queues and parsers per virtual
cable, nonblocking output queueing, streamed SysEx, device-generation ownership,
and logical IN/OUT ports with application capability handles.

The current hardware targets the supported CME U[2|6]MIDI Pro descriptor and
endpoint format; it is not a general-purpose USB compatibility layer. There are a
few deterministic host tests which exercise most of the critical paths, but
exhaustive testing with multiple adapters and multiple physical MIDI cables has
not been undertaken. As Knuth would say, "the code has been proved correct; it has
not been tested" (at least, not physically).

A largely complete MIDI API is found in `src/midi.h`. Building a MIDI application
into the kernel should just be a matter of including that file in the new
application source, and modifying `main.c` appropriately.

- [Address-space machinery and MMU validation](docs/address-spaces.md)
- [Recoverable exceptions and EL1 trap test](docs/exceptions.md)
- [First EL0 program and privilege-fault test](docs/el0-testing.md)
- [Independent EL0 roots and cross-stack Pi tests](docs/el0-address-spaces.md)
- [Two explicit EL0 contexts, lifecycle and Pi procedure](docs/el0-two-contexts.md)
- [Original single-context diagnostic](docs/el0-context.md)
- [Application MIDI API and GNU17 examples](docs/midi-api.md)
- [Implemented MIDI architecture and ownership](docs/midi-architecture.md)
- [Host tests, receive verifier and physical test procedure](docs/midi-testing.md)

Build the Pi image with an AArch64 GNU toolchain and run host regressions with a
native GNU17 C compiler:

```sh
make check
make
```

`make` produces `kernel8.img`. The default cross-toolchain prefix is
`aarch64-linux-gnu-`; `CROSS`, `HOSTCC` and `HOSTCFLAGS` can be overridden. See
the testing guide for what these checks establish and what still needs
hardware.

OrgOS was written with the assistance of Codex.

The Raspberry Pi 4B firmware and Linux DTB are included in `boot/` for
convenience.  They are not part of OrgOS, but have been used for testing.


# License

Unless otherwise excepted, OrgOS source code is licensed under the MIT License.

Exceptions:

- `src/pcie.c` combines new MIT-licensed OrgOS material with ISC-licensed
adaptations from FreeBSD and OpenBSD. Original copyright and permission
notices are retained in the file.  It is therefore dual-licensed MIT/ISC.
- Firmware binaries in `boot/` are governed by the Broadcom / Raspberry Pi
firmware license.  It is available at https://github.com/raspberrypi/firmware
and is tagged `1.20260915`.
- The Linux DTB is governed by the GNU GPL version 2.0, and is used unmodified:
the sources are available at https://github.com/raspberrypi/linux.
- The full text of all licenses will be found in `LICENSES/`.
