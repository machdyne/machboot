# machboot

A boot manager for Machdyne FPGA computers. At power-on it shows a
startup screen and counts down to your default system. Press a key and
you get a menu to start, install, update and remove systems -- Zeitlos,
Mach86, Kakao, anything else that is an ECP5 bitstream -- plus a USB
card reader, settings and diagnostics. Think of a PC's BIOS setup, for a
machine whose "BIOS" is a bitstream.

```
machboot 0.4  Machdyne Sergei ML2
Starting Mach86 in 3 ... (Enter: start now, F8, Esc or m: menu)

== Start a system ==
 > Mach86 r32                          default, in flash
   Zeitlos                                        module
   Kakao Linux head                             in flash

 Enter starts Mach86 from flash (nothing is written).
[ ^v Choose   Enter Start   F3 F4 Move   Del Remove   < Menu]
```

## What it does

- **A list of systems, in your order.** The top one is the default and
  starts by itself after a short wait. F3 and F4 move a system up or
  down; Del removes one.
- **Install from an SD card or a USB flash drive.** Put systems in
  `\SYS\GATEWARE\<NAME>\` on the card, then press F2 (docs/sdcard.md).
- **Flash writes are said out loud.** Anything that will write flash
  says so before you press Enter, and how long it takes: "Enter copies
  Kakao from the SD card into flash (about 8 s), then starts it."
  Copies are safe against power cuts.
- **The SD card as a USB drive.** The Card reader page shares the card
  with your PC, so you can copy systems onto it without taking it out.
- **Keyboard and screen.** A USB keyboard (through hubs too) on DVI at
  640x480, or the same menu as text on the serial console.
- **Diagnostics**: memory, flash, SD card and video tests, and a Board
  page with what was found.

## Boards

| Board | FPGA | Systems kept | How machboot is installed |
|---|---|---|---|
| **Sergei** (Sechzig ML0, ML1, ML2) | ECP5 25F / 45F | several, in the RP2040's flash; switching writes nothing | a UF2 file for the RP2040 (hold BOOT, copy) |
| **Mozart** (Sechzig ML0, ML1, ML2) | ECP5 25F / 45F | as Sergei | as Sergei |
| **Lakritz** | ECP5 25F | one in the system slot, the others copied in from the card when started | `dfu-util` once, from tinydfu |
| **Konfekt** | ECP5 12F | as Lakritz | as Lakritz |

On Lakritz and Konfekt, Zeitlos's 1 MB region in flash is always left
alone, whatever is installed.

## Getting started

Download the files from the latest release on GitHub; its QUICKSTART.md
(docs/quickstart.md) has the same steps.

### Sergei and Mozart

1. Hold the RP2040's BOOT button while plugging in the USB-C cable. A
   drive called `RPI-RP2` appears.
2. Copy `machboot-sergei_ml2.uf2` (or the file for your carrier and
   module) onto it. The RP2040 restarts.
3. Power-cycle the board. The menu appears after about a second; your
   module's own system is in the list as "module".

The RP2040 is still the JTAG cable and serial bridge it was before
(dirtyJTAG-compatible, so openFPGALoader works as usual), and a PC using
JTAG always takes priority. Details: docs/power-on.md, docs/rp2040.md.

### Lakritz and Konfekt

1. Try it first without writing anything:
   `openFPGALoader -c <cable> machboot-lakritz-sram.bit`.
2. Put Zeitlos's gateware on the SD card
   (`python3 tools/mksys.py <card> zeitlos-lakritz_gpio-gateware.bit --name Zeitlos --version 0.0.8`).
3. During tinydfu's 5 seconds:
   `dfu-util -a 0 -D machboot-lakritz-install.bin`. machboot starts in
   install mode: Enter, Y. About 20 seconds later it restarts as the
   board's boot manager and starts Zeitlos.

Read docs/install-mode.md before step 3: as with upgrading tinydfu, a
power cut in the last seconds of that one copy needs JTAG to recover.

### Putting systems on the card

```
python3 tools/mksys.py /media/$USER/SDCARD mach86.bit --name Mach86 --version r32 --board lakritz
python3 tools/mksys.py /media/$USER/SDCARD mach86.bit --name Mach86 --version r32 --board sergei_ml2
```

This makes `\SYS\GATEWARE\MACH86\SYSTEM.CFG` and `LAKRITZ.BIT` (or
`SERGEI2.BIT`, ...). Any FAT partition of the card works, so it can be
the same card a system boots from. docs/sdcard.md.

## Releases

Each release on GitHub carries the installers, every UF2, every
bitstream, `mksys.py` and the quick start (docs/quickstart.md).
`make release` builds and gathers them into build/release/;
`tools/gh-release.sh` publishes them (GitHub CLI). The CI does both by
itself when a tag `v<version>` matching fw/mb.h's `MB_VERSION_STR` is
pushed.

## Building

```
make                       # Lakritz: build/machboot-lakritz.bin, -install.bin, -sram.bit
make BOARD=konfekt         # Konfekt
make sechzig uf2           # Sergei and Mozart: the UF2 files, with the RP2040 firmware
make test                  # the tests: the menu on a PC, the RP2040 firmware, the SoC in simulation
```

You need yosys, nextpnr-ecp5 and ecppack (native, or YoWASP from pip), a
RISC-V GCC, an ARM GCC and the pico-sdk for the RP2040 firmware, Python 3
with Pillow, and g++ for the simulation. docs/building.md.

## Documentation

**Using machboot**

| | |
|---|---|
| [docs/ui.md](docs/ui.md) | the startup screen, the menu and its keys, the serial console |
| [docs/systems.md](docs/systems.md) | the list of systems: starting, installing, removing, where they are kept |
| [docs/sdcard.md](docs/sdcard.md) | the SD card and `\SYS\GATEWARE` |
| [docs/install.md](docs/install.md) | installing a system, and power cuts |
| [docs/install-mode.md](docs/install-mode.md) | Lakritz and Konfekt: replacing tinydfu with machboot |
| [docs/power-on.md](docs/power-on.md) | Sergei and Mozart: the UF2 files, power-on, the supervisor's log |
| [docs/settings.md](docs/settings.md) | the settings |
| [docs/usb.md](docs/usb.md) | USB keyboards, hubs and flash drives |
| [docs/usb-device.md](docs/usb-device.md) | Lakritz's USB-C port: console, DFU, card reader |
| [docs/zeitlos.md](docs/zeitlos.md) | Zeitlos under machboot |
| [docs/boards.md](docs/boards.md) | the boards in detail |

**How it works**

| | |
|---|---|
| [docs/boot.md](docs/boot.md) | power-on to a running system |
| [docs/flash-layout.md](docs/flash-layout.md) | where everything is in flash |
| [docs/memory-map.md](docs/memory-map.md) | the SoC's address map and registers |
| [docs/rp2040.md](docs/rp2040.md) | the RP2040 firmware for Sergei and Mozart |
| [docs/xlink.md](docs/xlink.md) | the link between the FPGA and the RP2040 |
| [docs/overlays.md](docs/overlays.md) | Lakritz: the menu's pages loaded from flash |
| [docs/font.md](docs/font.md) | the code page and colours |
| [docs/building.md](docs/building.md) | toolchain and make targets |
| [docs/provenance.md](docs/provenance.md) | clean-room statement; what is reused from Zeitlos |

**Development**

| | |
|---|---|
| [docs/testing.md](docs/testing.md) | the tests, and trying machboot on a board |

## Source layout

```
rtl/              the gateware (mb_*.v machboot's own; top_*.v one per board)
  zeitlos/ cpu/ usb/ ulixxe/   reused from Zeitlos (docs/provenance.md)
bootrom/          the boot ROM: loads the firmware from flash
fw/               the menu's firmware (C); fw/host/ builds it for a PC
rp2040/           the RP2040 firmware for Sergei and Mozart
boards/           pin constraints
tools/            mksys.py, mkuf2.py and the build tools
sim/, tests/      simulation and tests
zeitlos-patches/  two small changes for Zeitlos under machboot (docs/zeitlos.md)
```

## Status

machboot 0.4. On **Sergei ML2** it has run on hardware: the menu at
power-on, the list of systems (Zeitlos, Mach86 and Kakao, installed from
the card and started from the RP2040's flash), settings and the card
reader. On **Lakritz** it has been installed from tinydfu with the
installer and runs as the board's boot manager. Konfekt, Mozart and the
ML0/ML1 modules are built and tested in simulation only.

Known issues:

- The copy times shown are estimates until they are measured
  on the board.
- On some Linux PCs the card reader has mounted read-only; this is
  being looked into.

## License

See LICENSE.md.
