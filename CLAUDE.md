# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A SANA-II network device driver (`openpci-rtl8029.device`) for RTL8029 (NE2000-compatible) PCI Ethernet cards on classic AmigaOS (m68k), using `openpci.library`. It is a port of Grzegorz Kraszewski's 2001–2002 `prm-rtl8029.device` (Prometheus) driver; history up to 1.3 is in `ne2000/release_notes`. 

The repo also contains a TCP client/server pair used to exercise the driver end to end.

## Build

Cross-compiled on the host with the m68k-amigaos GCC toolchain installed at `/opt/amiga` (path is hard-coded in `makefile` as `TOOLPREFIX`).

```bash
make                 # driver + Amiga test client -> bin/
make driver          # bin/openpci-rtl8029.device (+ unstripped .debug)
make client          # bin/client (Amiga-side test program)
make s2stat          # bin/s2stat (Amiga-side tool printing the driver's diagnostic counters)
make adf             # bin/openpci-rtl8029.adf: floppy image with driver + tools (needs xdftool)
make RELEASE=1       # -O2, no debug output (default is -O0 -g -DPDEBUG)
make clean

cd macOS/src && make -f Makefile.server   # host-side test server -> macOS/src/server
```

- The build uses `-Wall -Werror`; any warning fails it.
- `include/` holds third-party headers the toolchain does not ship: the openpci.library SDK (Aminet `driver/other/openpci68k.lha`), mmu.library (MuManual from NDK 3.2) and `devices/sana2.h` (NDK 3.2). `include/proto/*.h` and `include/inline/*.h` are generated — do not hand-edit; regenerate from `include/fd/*.fd` with `fd2sfd <fd> <clib protos> -o x.sfd` then `sfdc --target=m68k-amigaos --mode=macros|proto`.
- The Amiga-side tools (`client`, `s2stat`) are linked against libnix (`-noixemul`), not the toolchain's default newlib. They run on the shell's default 4 KB stack with no stack swap: their own call chains measure about 1.2 KB at most, so keep large buffers on the heap, not the stack.
- Driver sources live in `ne2000/`; to add one, append its bare filename to `SRCS` in `makefile` (no directory prefix).

## Testing

There are no automated tests; nothing can be run on the host except the server. Testing needs real Amiga hardware (or an emulator with an RTL8029) and is manual — see `TEST_PROCEDURE.md`:

1. Run `macOS/src/server` on the host (listens for the Amiga, prints its IPs).
2. Copy `bin/openpci-rtl8029.device` to `DEVS:Networks/` and `bin/client` to the Amiga, then run `client SERVERIP <ip>`.

The user runs the Amiga side and pastes back results (the `output*.txt` files in the repo root are such captures). After changing the driver, a successful `make` is the only verification available locally — say so rather than claiming a fix works.

Files reach the Amiga on a floppy image: `make RELEASE=1 adf` builds `bin/openpci-rtl8029.adf` (volume `OpenPCI`) from scratch with xdftool. Never update files inside an existing image: doing that with ADFMaster 3 produced corrupt copies of the right size, which the Amiga rejects with "bad loadfile hunk". That error is therefore a transfer problem first, not a build problem (builds are deterministic).

Diagnosing on the real machine:

- `s2stat` on the Amiga prints the driver's counters and unit-task state (`S2_GETSPECIALSTATS`, answered in the caller's context so it works even if the unit task hangs). Run it twice a few seconds apart and compare.
- The Amiga can be pinged from the development Mac; a size sweep (`ping -s <n> <amiga-ip>`, frame = n + 42) is a quick deterministic check of frame-size handling. The client/server test did not catch a bug that dropped every frame of 1511+ bytes; the sweep did.
- `sudo tcpdump -i en0 -w file.pcap host <amiga-ip>` on the Mac shows which frames the Amiga fails to acknowledge.

The client and server implement one fixed protocol and must be changed together: handshake, then 14 length-prefixed pattern packets (64–4096 bytes, odd and even sizes, same list in both sources) server→client, the same 14 client→server, then a packed 22-byte binary struct in each direction to check byte order. Last comes a UDP echo phase (client sends datagrams of exact sizes, server echoes them, client compares): one datagram is exactly one frame of payload + 42 bytes, covering 60 bytes and every size from 1506 to the 1514-byte maximum. This is the part that checks frame-size handling; the TCP phases cannot, because TCP retransmits and macOS quietly falls back to smaller segments when full-size ones go unanswered. Any step added on one side needs its counterpart on the other, in the same order.

## Driver architecture

Nearly everything is in `ne2000/driver.c` (sections are delimited by `///` fold markers). `ne2000/header.s` supplies the ROMTag that makes the binary a loadable device; `ne2000/rev.h` holds the version/ID strings (the device name also appears literally in `header.s`). There is no driver header: struct and macro definitions are at the top of `driver.c`.

Three execution contexts share a `UnitData`:

- **Caller context** — `DevBeginIO()` handles quick commands (`S2_*` queries, online/offline, config) inline and forwards `CMD_READ`, `CMD_WRITE`, `S2_BROADCAST`, `CMD_FLUSH` to the unit task's message port.
- **Unit task** — `UnitTask()`/`MainLoop()`, one per opened unit. Queues read requests on `ud_RxQueue` and write requests on `ud_TxQueue`; `StartNextWrite()` sends the next one with `SendPacket()` when `ud_PendingWrite` is empty. Only one transmit is in flight at a time.
- **Interrupt server** — `IntCode()`, installed via `pci_add_intserver()`. It does no packet work: it masks the card's interrupts and signals the unit task (`ud_ServiceMask`).

The unit task then runs `ServiceCard()`: TX done/error completes `ud_PendingWrite`; RX drains the card's ring buffer (`PacketReceived()` → `GetPacketHeader()` + `GetPacket()` copy each frame via remote DMA into `ud_RxBuffer`, then `SearchReadRequest()` finds a pending read of the matching packet type and completes it); finally it starts the next queued transmit and unmasks the card. A frame with no matching pending read request is silently discarded.

Rules that follow from this (each has been broken before and caused stalls):

- Do not use `Disable()`/`Enable()` (discouraged on current OS 3.x). The design avoids needing them: every access to the card except the mask-and-signal in `IntCode()` happens in the unit task.
- `IntCode()` must stay minimal: no `Delay()`, no semaphores, no debug output, no packet work.
- The request queues are shared between the unit task and callers (`DevAbortIO()`) and are protected by `ud_RxSem`/`ud_TxSem`. `ud_PendingWrite` is touched by the unit task only.
- Register page 1 may only be selected while the card's interrupts are masked (`INT_MASK` = 0): the interrupt chain is shared, so `IntCode()` can run at any time and treats "page 1 selected" as "not our interrupt".
- TX upload and RX download share the card's single remote-DMA channel; they are serialized by both running in the unit task.
- No per-packet debug output: serial `KPrintF` takes longer than a packet.

Hardware details that matter when touching the RX/TX paths:

- Card memory layout is in 256-byte pages: TX buffer at page `0x40`, RX ring from `0x46` (`RX_BUFFER`) to `0x80` (`BUFFER_END`). `ud_NextPage` is the software read pointer; the `BOUNDARY` register is kept one page behind it.
- The byte count in the RX ring header is the Ethernet frame length plus 4. `PacketReceived()` strips those 4 bytes; passing them on makes full-size frames exceed the MTU and the TCP/IP stack drops them.
- Remote DMA counts must be even (the data port is 16 bits wide); `SendPacket()` and `GetPacket()` round up.
- Registers are byte-wide (`ud_Hardware->regs[]`); packet data moves through the 16-bit `DMAPort`. The card is little-endian and the CPU big-endian, so byte order of each word read from `DMAPort` is significant — `GetPacketHeader()` documents the layout, and this has been wrong and reverted more than once in git history. `ud_RxBuffer` is indexed in 16-bit words (e.g. `[6]` is the EtherType, offset 7 skips the 14-byte header).
- Cache coherency for the memory-mapped registers: if `mmu.library` is available the region is marked cache-inhibited (`setup_cache_inhibited_hw()`); otherwise `hw_read_barrier()`/`hw_write_barrier()` fall back to `CacheClearE()`. Barrier placement has been reworked several times trading correctness against per-packet cost.

Conventions:

- `__NOLIBBASE__` is defined: there are no global library bases. Each function declares the bases it needs locally with `USE(SysBase)` (from `dd`) or `USE_U(SysBase)` (from `ud`); calling an OS function without the matching macro will not compile.
- Entry points take arguments in registers via the `reg(a1)`-style macro.
- The driver links with `-nostdlib`: no C library. `strcpy` and the 32-bit division helpers (`__divsi3.c`, `__udivsi3.c`) are provided locally.
- Debug output goes to the serial port via `KPrintF`. `DBG*` macros compile away unless `PDEBUG` is set; `INIT_DBG*` macros always print and are meant for one-time initialisation messages only.
