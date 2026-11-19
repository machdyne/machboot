# Plan

The full proposal (decisions, reasoning, alternatives) is kept in the
project as `bootloader-proposal.md` (revision 8). This page is the
working summary: the phases, and what each round actually delivered.

## Boards

| Board | FPGA | Flash | USB to the PC | Menu lives in | Status |
|---|---|---|---|---|---|
| **Lakritz** | ECP5-25F | MMOD, 2 MB (4 MB+ optional) | FPGA | boot slot at 0x000000 | rounds 1-4 |
| **Konfekt** | ECP5-12F | MMOD, 2 MB | FPGA | boot slot at 0x000000 | round 4 |
| **Sergei ML0 / ML1 / ML2** | ECP5-25F / 45F / 45F | module | RP2040 | RP2040 firmware (loaded over JTAG) | round 4: SRAM builds; Phase 3b |
| **Mozart ML0 / ML1 / ML2** | ECP5-25F / 45F / 45F | module | RP2040 | as Sergei | round 4: SRAM builds; with Sergei |
| Obst, Minze, Schoko, Noir, Klinge | | MMOD | FPGA | boot slot | later |
| Vivaldi | | | | (headless: not planned) | |
| Artix-7 (MX1) | | | | | not now |

## Phases

| # | Phase | Status |
|---|---|---|
| 0 | **Measure** the menu on 25F and 45F; repo skeleton | **done, round 1** ([measurements.md](measurements.md)). The Sergei-side measurements (JTAG SRAM load with SYS_RST_N held, load time, JTAG-to-SPI, XA-XD rate) need the board: Phase 3. |
| 1 | **Menu:** startup screen, menu frame, Start/Settings/Board, boot the installed system, serial mirror, settings | **done (Lakritz)**: round 1, and round 2 (USB keyboard, settings stored) |
| 2 | **Card:** MBR/FAT, `/sys/gateware`, packages, install with verify, fenced writer, power-cut test; install mode | **done (Lakritz)**: round 2, install mode round 3 |
| 3a | RP2040 drop-in: dirtyJtag-compatible JTAG + UART bridge | **round 4**: written and tested against a model TAP (docs/rp2040.md); not yet on a board |
| 3b | RP2040 supervisor: menu loading, JTAG arbiter, mailbox, run from card, cache ring, control port | |
| 3c | Sergei: drag-and-drop drive, card reader | |
| 4 | Lakritz USB: card reader, drag-and-drop drive, CDC, DFU | **round 3**: CDC console, DFU (tinydfu-compatible alt 0), the card reader. **Round 4**: drag and drop through the card reader (copy into `\SYS\GATEWARE`, eject, Enter installs) instead of a separate virtual drive |
| 5 | Diagnostics (SDRAM/DDR3, SD, USB, audio, Ethernet, clock, PMOD) | started: RAM, flash, video pattern |
| 6 | 4 MB+ multi-slot: jumploader, slot table | |
| 7 | Mach86 packages, end-to-end switching | |
| 8 | Optional: USB flash drives, signing on by default, en/de, more boards | **round 4**: hubs and flash drives on the host ports (docs/usb.md) |

## Rounds

### Round 1 (2026-11-17)

Delivered:

- The repo, the build, and the toolchain notes.
- **Gateware** for Lakritz: zeitlos32, 64 KB RAM with a separate 1 KB
  boot ROM, text mode 80x30 on DVI 640x480 (own TMDS encoder and
  serialiser), system registers, the Zeitlos SPI flash controller with
  its write fence at 0x060000, the Zeitlos UART, the Zeitlos USB host
  (in the gateware, not yet used by the firmware). Meets 48 MHz with
  margin (57-67 MHz depending on the run).
- **Boot ROM** (630 bytes): finds the firmware at 0x050000 or 0x090000,
  checks it (CRC-32), runs it.
- **Firmware** (13 KB): startup screen with countdown; the menu frame;
  Start (with the failed/missing/wrong-FPGA cases), Settings (shown),
  Diagnostics (RAM march test, flash read, video pattern), Board (with
  F1 details); install-mode detection; starting the system with
  PROGRAMN; the serial mirror and serial keys.
- **Phase 0 measurements**: [measurements.md](measurements.md).
- **Tests**: the PC build drives every page and renders it; the
  simulation boots the real boot ROM and firmware from a modelled flash
  and decodes the DVI output -- pixel-identical to the PC rendering.

Not yet: USB keyboard, SD card, any flash writing, USB device, Sergei.

### Round 2 (2026-11-17)

Delivered:

- **USB keyboards** on both host ports ([usb.md](usb.md)): enumeration
  on Zeitlos's controller, boot protocol, the auto-poll; keys and repeat
  merged with the serial console's; US and German layouts.
- **Settings stored** ([settings.md](settings.md)): Zeitlos's kvlog in
  0x05E000-0x05FFFF, through a writable window in the flash fence (the
  one change to Zeitlos's `spiflash.v`).
- **SD card and FAT** ([sdcard.md](sdcard.md)): Zeitlos's SPI master at
  0x5000_0000; SPI-mode driver and read-only FAT12/16/32 written from the
  specifications; every partition searched for `\SYS\GATEWARE`.
- **Install from the card** ([install.md](install.md)): checks, compare
  first (nothing written if identical), first sector last, read-back,
  recorded in the settings; "did not finish installing" with a one-key
  reinstall; the startup line when the card has a newer version.
- Register-map version 2 (HWVER), firmware 0.2.
- Tests: 33 PC cases (installs byte-exact, a power cut and recovery,
  settings written only to their sectors, a USB keyboard driving the
  menu, FAT32/16/12 cards from pyfatfs); a third simulation run that
  installs through the real RTL and checks the fence.

Not yet: install mode (self-install over tinydfu), hubs, run a file,
the USB device (drive, DFU), German text, Sergei.

### Round 3 (2026-11-17, overnight)

Delivered:

- **Install mode** ([install-mode.md](install-mode.md)): `dfu-util -a 0
  -D machboot-lakritz-install.bin` on a board with tinydfu; machboot
  replaces tinydfu and installs Zeitlos from the card. A third gateware
  build, the installer, with the flash fence turned around (it can
  write 0x000000-0x0FFFFF and not the Zeitlos region); the normal
  build still cannot write its own boot slot. Written from the top
  down so tinydfu survives a power cut for as long as possible; tested
  with a power cut and a second dfu-util.
- **The USB-C port** ([usb-device.md](usb-device.md)): a CDC-ACM
  console (the menu as text, keys) and **DFU**: alt 0 takes what
  tinydfu's alt 0 took, so `dfu-util -a 0 -D` with Zeitlos's
  `-dfu.bin` works as before; the slot is written as an install from
  the card is. Hardware: ulixxe's SIE (MIT, from Zeitlos) under a new
  endpoint-buffer block (`rtl/mb_usbd.v`); everything above it is
  firmware.
- **The card reader**: the menu's Card reader page shares the SD card
  with the PC as a USB drive (bulk-only mass storage, SCSI); ejecting
  it on the PC or leaving the page gives it back, and machboot reads it
  again. SD writes (CMD24) for it.
- **Zeitlos patches** ([zeitlos.md](zeitlos.md), `zeitlos-patches/`):
  the kernel keeps writes out of machboot's boot slot, `zfpga flash`
  finds Zeitlos's gateware at 0x060000; and a fix to zfpga's flash
  tests, which failed at `ede46a8`.
- RAM 64 KB -> 80 KB (49 -> 51 of 56 block RAMs with the USB device);
  firmware 0.3.
- Tests: 71 PC checks (install mode with power cuts, DFU, the console);
  five simulation runs, two new: install mode on the installer
  gateware, and the USB device at the wire against a USB host written
  from the specification -- which found and fixed a real bug in the
  new endpoint block.

Not yet: the drag-and-drop drive for gateware, hubs, Run a file,
German text, Sergei.

### Round 4 (2026-11-18)

- **USB hubs and flash drives** on Zeitlos's USB host controller, in
  machboot's firmware: `fw/usb.c` rewritten as small state machines, one
  owner of the transaction engine, one device on address 0 at a time,
  the hub resets its own ports, a failed child's hub port disabled (the
  rules from Zeitlos's `usbh_hub.c`). Up to two keyboards, two hubs one
  level deep, one flash drive (bulk-only, SCSI, read only). Update lists
  the drive's `\SYS\GATEWARE` next to the SD card's. `usb_hid_host` is
  not used.
- **Drag and drop** through the card reader: a newer system copied onto
  the card is offered first ("Install X ver") when the PC ejects it.
- **RP2040 supervisor, phase 3a** (`rp2040/`, `make sv`):
  dirtyJTAG-protocol JTAG (openFPGALoader `-c dirtyJtag`) plus the
  UART1 bridge, for Sergei and Mozart; the protocol parser tested on the
  PC against a model ECP5 TAP.
- RAM 80 -> 88 KB; the firmware image (59.9 KB) now goes below its
  header, and the boot ROM follows the header's new word 7.
- A PC model of a hub, keyboards, mice and a flash drive, which fails a
  test on address collisions, toggle errors and wrong polarity.
- **More boards** (docs/boards.md): Konfekt (everything Lakritz has), and
  SRAM builds for Sergei and Mozart with ML0, ML1 and ML2 -- the menu
  loaded with openFPGALoader, Start boots the module's flash; the module
  flash is locked in hardware until the RP2040 handles installs. The
  firmware reads the number of USB host ports from the controller.

### Round 5a (2026-11-18): the card reader on Sergei and Mozart

- **The XA-XD link** (docs/xlink.md): `rtl/mb_xlink.v`, an SPI slave
  with two 2 KB message buffers (two block RAMs), the RP2040 its master
  at 6.25 MHz. The roles keep the RP2040 off XC, where Zeitlos's ML1
  images have a UART output; the RP2040 drives the link only while
  machboot answers.
- **The card reader** through it: the RP2040 is a USB card reader
  (`rp2040/src/msc*.c`, a fourth interface in `machboot-sv.uf2`); its
  sectors come from `fw/xlink.c`, which stands in for `fw/usbd.c` in the
  Sergei/Mozart firmware -- the menu's Card reader page is the same as on
  Lakritz. The Sergei/Mozart firmware drops the USB device and install
  mode: 13 KB of RAM to spare there.
- **Fix:** ML0/ML1 builds had the console on B3/A2, which on those
  modules are XC/XD; the module's UART is B4/C4 (pad nets of the
  modules' PCBs). ML2 was right.
- Tests: the RTL in CXXRTL against a real-time master (works to 10.4 MHz,
  fails at 12.5); the RP2040's code against the FPGA firmware's own
  through a byte model; the menu's side on the PC; and a sixth gate-level
  run with all of it together.
- The startup line on the serial console mentions Esc.

### Round 5b (2026-11-18): the card reader, faster

- Hardware: the card reader works on Sergei ML2 (31.7 GB card, both
  partitions); opening a folder read 10 MB (the FAT), slowly.
- Read-ahead in machboot, fetch-ahead on the RP2040 (overlapping USB),
  link 8.93 MHz, SD at 24 MHz (as Zeitlos), word copies: 290 -> about
  740 KB/s in the whole-SoC simulation with USB's time modelled.
- The Card reader page: MB moved, and MB/s while it moves.
- Diagnostics' SD test borrows the FAT layer's buffer (512 bytes of RAM
  back for Lakritz).

### Round 6 (2026-11-18): the menu at power-on on Sergei and Mozart

- The RP2040 loads the menu over JTAG at power-on and after every reset
  of the module, except the one machboot's Start causes (it announces it:
  START); never while a PC uses JTAG, and a load in progress stops for
  the PC (rp2040/src/sv.c, ecp5.c). One UF2 per module carries the
  supervisor and that module's menu (tools/mkuf2.py, `make uf2`).
- Settings on Sergei/Mozart are kept by the RP2040 in its flash (a log
  with CRCs, rp2040/src/store.c), handed to machboot when it starts and
  fetched back when they change (SET/GET over the link).
- The link is probed only in a short window after a load; while a system
  runs the RP2040 sends nothing on XA-XD.
- The countdown follows settings that arrive while it runs.
- Tests: the loader against a model ECP5 configuration engine; the whole
  supervisor against a model module running machboot's own link code;
  gate level: settings at boot, a change fetched, START before PROGRAMN.

### Round 5 (next)

- Hardware results folded back in (Lakritz; the RP2040 firmware on
  Sergei/Mozart).
- RAM: code loaded from flash on demand, before anything new.
- Phase 3b: the RP2040 loads the menu over JTAG at power-on (for each
  module: the right ML0/1/2 build in `machboot-sergei.uf2` /
  `machboot-mozart.uf2`), the mailbox (settings, installs).
