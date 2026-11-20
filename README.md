# openpci-rtl8029.device

A SANA-II network device driver for RTL8029 (NE2000-compatible) PCI Ethernet cards on classic
AmigaOS (m68k), using `openpci.library`. It works with Roadshow and other SANA-II TCP/IP
stacks.

Maintained by Steen Lund.

## Origin

This driver began as `prm-rtl8029.device`, written in 2001–2002 by Grzegorz "Krashan"
Kraszewski for the Prometheus PCI board. Its source was released to the public by Matay,
Poland, in 2004; its history up to version 1.3 is in `src/ne2000/release_notes`.

Since 2024 it has been ported to `openpci.library` and largely rewritten: the request
handling, interrupt handling, receive and transmit paths, overflow recovery and diagnostics
are new. The git history records every change and its date.

## Building

See `CLAUDE.md` for the toolchain and all make targets. In short:

```bash
make RELEASE=1 adf   # dist/openpci-rtl8029.adf: driver, test client and s2stat
make server          # bin/macOS/server: host side of the network test
```

## Installing

The driver needs `openpci.library` from the openpci package (Aminet
`driver/other/openpci68k.lha`). Write `dist/openpci-rtl8029.adf` to a floppy (or mount it) and
double-click the `Install` icon on it. From a Shell, the same is:

```
cd OpenPCI:
Installer SCRIPT Install APPNAME openpci-rtl8029.device
```

The installer copies the driver to `DEVS:Networks/`, or removes it. It checks for the card
with the `lspci` command of the openpci package; if `lspci` cannot be run, it says so and
installs anyway. To install by hand, copy `openpci-rtl8029.device` to `DEVS:Networks/`.

## License

The repository holds parts under different licenses:

| Part | Copyright | License |
|---|---|---|
| The driver, `src/ne2000/` | 2001–2004 Matay (written by Grzegorz Kraszewski); 2024–2026 Steen Lund | GPL-2.0-or-later, with Matay's original release condition below |
| The test and diagnostic tools, `src/Tools/` (`client`, `s2stat`, `server`) | 2025–2026 Steen Lund | MIT |
| `src/ne2000/__divsi3.c`, `__udivsi3.c` | from libnix (bebbo/libnix) | public domain |
| Build files and documentation | 2024–2026 Steen Lund | MIT |
| Third-party headers, `include/` | see below | their own terms |

### The driver

Matay released the original source under the terms of the GNU General Public License,
without naming a version (`licenses/matay-prm-rtl8029.txt`, kept unchanged). This driver is
distributed under the GNU General Public License, version 2 or (at your option) any later
version; the full text is in `LICENSE`.

Matay's release also states that no part of the driver may be included in a commercial
product. That condition comes from the original copyright holder and this project honours
it: do not sell the driver or include it in a commercial product.

### The tools

`client`, `s2stat` and `server` were written from scratch for this project and are separate
programs that talk to the driver only through the SANA-II interface or the network. They
are under the MIT license (`licenses/MIT.txt`).

### Third-party headers

`include/` holds headers that the cross toolchain does not ship. They are included only so
that the project builds, and they remain under their authors' terms:

- `include/libraries/`, `include/clib/openpci_protos.h`, `include/fd/openpci_lib.fd`: the
  openpci.library SDK (Aminet `driver/other/openpci68k.lha`).
- `include/mmu/`, `include/clib/mmu_protos.h`, `include/fd/mmu_lib.fd`: mmu.library by
  Thomas Richter (THOR-Software), from the MuManual of NDK 3.2.
- `include/devices/sana2.h`: the SANA-II header, (C) Amiga, Inc., from NDK 3.2.
- `include/proto/` and `include/inline/` are generated from the `.fd` files.
