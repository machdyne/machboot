# Testing

`make test` runs everything that needs no board; the CI runs it on
every push.

| Target | What it runs |
|---|---|
| `make host` | the menu firmware built as a PC program (`fw/host/`), driven through every page by `tests/run_host.py` |
| `make sv-test` | the RP2040 supervisor's code on the PC: JTAG, the ECP5 loader, the link, the system cache (`rp2040/tests/`) |
| `make xlink-test` | the link's RTL (`rtl/mb_xlink.v`) in CXXRTL against a real-time SPI master |
| `make sim` | the whole SoC in CXXRTL: boot ROM, firmware, flash, SD card, DVI and USB at the pins |

## The PC build (`make host`)

`fw/host/hw_host.c` stands in for the hardware: registers backed by
variables, the flash by an image file, time by a virtual clock, keys by
a script. Each case saves its screens as text and PNG in `build/host/`
and checks what they say. The cases cover:

- **Starting:** the countdown, Enter, an empty slot, a bitstream for the
  wrong FPGA.
- **The card and USB drives:** FAT32, FAT16 and FAT12, with and without a
  partition table; installing byte for byte; "already installed" writing
  nothing; a newer version on the card.
- **Power cuts:** the power fails at a chosen flash write during an
  install or update; the next start finds it and installs again.
- **Install mode:** replacing tinydfu, Esc writing nothing, cuts early in
  the copy leaving tinydfu untouched, damaged or wrong installers refused.
- **USB:** hubs, keyboards, mice and flash drives on the host ports;
  enumeration, the console, DFU and the card reader on the USB-C port.
- **Settings:** changed, kept, and only the settings sectors written.
- **Sergei and Mozart:** the link to the RP2040, settings over the link,
  the list of systems, and not one write to the module's flash.

The flash model enforces the controller's rules (the write fence, NOR
programming only clearing bits). The SD card model (`tests/sdmodel.c`)
is written from the SD specification. The USB model fails a test when
two devices answer one address, a data toggle is wrong, or a device is
addressed at the wrong speed.

## The supervisor (`make sv-test`)

The RP2040 code that does not touch its hardware: the dirtyJTAG
protocol, the ECP5 SRAM load against a model of the FPGA's JTAG port,
the link protocol, and the system cache with a modelled flash. The cache
tests copy, list, remove and run systems, cut the power at chosen
writes, and unplug the PC's USB in the middle of a copy.

## Simulation (`make sim`)

CXXRTL compiles the SoC to C++. `sim/sim_main.cc` adds a SPI flash
model, a serial port, an SD card, and a DVI receiver that decodes the
TMDS lanes into frames. The runs:

1. power-on: the boot ROM loads and checks the firmware, the startup
   screen, F8, the menu, Enter, PROGRAMN;
2. the JTAG test build on a blank flash;
3. installing from a card through the real flash controller, checking
   that nothing outside the system slot and settings changed;
4. the USB device at the wire, against a USB host written from the USB
   2.0 specification: enumeration, a DFU download, the console, the card
   reader;
5. install mode on the installer gateware, from tinydfu to machboot;
6. Sergei ML2's card reader, with the RP2040's own link code driving the
   XA-XD pins.

`tests/check_sim.py` then compares the decoded frames with the PC
build's screens: pixel for pixel identical.

Not covered by simulation: the PLL and pads, real flash and card timing,
a real PC's USB stack, and the USB host for keyboards (that is tested
against a register model in the PC build).

## On a board

Try the SRAM build first: it writes nothing, and a power cycle puts the
board back as it was (docs/quickstart.md). The JTAG test build
(`make sram`) has its firmware in RAM, so it runs on a board with
anything, or nothing, in flash. It lists the card's systems but does
not install them, and refuses DFU downloads.

docs/quickstart.md has what to do when something goes wrong, and what
to send with a report.
