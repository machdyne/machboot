# machboot quick start

machboot is a boot manager for Machdyne FPGA computers: a startup
screen, a list of systems to start, install and remove, a USB card
reader, settings and diagnostics. The full documentation is in the
repository's `docs/`.

## What is in this release

| File | For |
|---|---|
| `machboot-sergei_ml0.uf2`, `_ml1`, `_ml2` | Sergei with a Sechzig ML0, ML1 or ML2: the RP2040 firmware with the menu |
| `machboot-mozart_ml0.uf2`, `_ml1`, `_ml2` | the same for Mozart |
| `machboot-sv.uf2` | Sergei or Mozart: the RP2040 firmware alone (JTAG, serial, card reader), no menu |
| `machboot-<board>-sram.bit` | any board: try machboot over JTAG, writing nothing |
| `machboot-lakritz-install.bin`, `machboot-konfekt-install.bin` | Lakritz, Konfekt: installs machboot from tinydfu with `dfu-util` |
| `machboot-lakritz.bin`, `machboot-konfekt.bin` | Lakritz, Konfekt: the boot-slot image, for writing at 0 over JTAG |
| `mksys.py` | puts a system on an SD card the way machboot looks for it |
| `SHA256SUMS` | checksums of the files above |

## Try it first (any board, writes nothing)

```
openFPGALoader -c <cable> machboot-lakritz-sram.bit        # or -konfekt-, -sergei_ml2-, ...
```

The menu appears on the DVI output and the serial console. A power
cycle puts the board back exactly as it was. On Sergei and Mozart the
cable is the board's own RP2040: `-c dirtyJtag`.

## Sergei and Mozart

1. Hold the RP2040's **BOOT** button while plugging in the USB-C cable.
   A drive called `RPI-RP2` appears.
2. Copy the UF2 for your carrier and module, for example
   `machboot-sergei_ml2.uf2`. The RP2040 restarts.
3. Power the board off and on. The menu appears after about a second.
   Your module's own system is in the list as "module" and starts by
   itself after 3 seconds.

The RP2040 stays a JTAG cable (dirtyJTAG-compatible: openFPGALoader works
as before, and a PC using JTAG always takes priority) and a serial bridge
(`/dev/ttyACM0`). Systems you install are kept in the RP2040's flash; the
module's own flash is never written.

To go back to the board as shipped, copy the stock `dirtyJtag.uf2`.

## Lakritz and Konfekt

1. Put Zeitlos's gateware on the SD card (see "Putting systems on the
   card" below).
2. Power on, and during tinydfu's 5 seconds:
   ```
   dfu-util -a 0 -D machboot-lakritz-install.bin       # or -konfekt-
   ```
3. machboot starts on **Install machboot**: press Enter, then Y. About 20
   seconds later it restarts as the board's boot manager, counts down and
   starts Zeitlos.

**Do not cut the power in the last seconds of that copy**: as with
upgrading tinydfu itself, that needs JTAG to recover. Zeitlos's 1 MB
region in flash is never touched, so Zeitlos's kernel and apps stay.

Over JTAG instead of step 2: `openFPGALoader -c <cable> -f -o 0
machboot-lakritz.bin`.

After the install, machboot answers DFU as tinydfu did:
`dfu-util -a 0 -D zeitlos-lakritz_gpio-dfu.bin` still works.

## Putting systems on the card

```
python3 mksys.py /media/$USER/SDCARD mach86.bit --name Mach86 --version r32 --board lakritz
python3 mksys.py /media/$USER/SDCARD mach86.bit --name Mach86 --version r32 --board sergei_ml2
```

This makes `\SYS\GATEWARE\MACH86\SYSTEM.CFG` (with `name=` and
`version=`) and the bitstream named for the board: `LAKRITZ.BIT`,
`KONFEKT.BIT`, `SERGEI0.BIT` to `SERGEI2.BIT`, `MOZART0.BIT` to
`MOZART2.BIT`. Any FAT partition of the card works. You can also copy
the files with machboot's **Card reader** page, which shares the card
with your PC over USB.

## Using the menu

- **F8** (or Esc, or m) during the countdown opens the menu.
- **Start**: the list of systems. Enter starts one; **F3/F4** move it
  up or down (the top one is the default); **Del** removes one.
- **F2, Install**: the systems on the card. Enter adds or updates one.
- Anything that writes flash says so before you press Enter, and how
  long it takes.
- **Settings**: start by itself or not, the wait, the keyboard layout.
- A USB keyboard works on any host port, through hubs too. The serial
  console shows the same menu as text at 1 Mbaud
  (`minicom -D /dev/ttyACM0 -b 1000000`).

## If something goes wrong

- **Sergei/Mozart, the screen stays dark at power-on:** the module's own
  system still starts as without machboot, and `lsusb` still shows
  1209:c0ca. Copy `machboot-sv.uf2` to load the menu by hand instead, and
  check the serial log.
- **A system from the list does not start:** the serial log ends the
  start with `run: <name>: ok` or says why it failed. If it says ok,
  load the same file with `openFPGALoader -c dirtyJtag <file>.bit` right
  after machboot. If that fails the same way, the system does not like
  starting on a warm board, and machboot is not the cause.
- **A page says "This page could not be loaded"** (Lakritz, Konfekt): a
  bug; please report the line under it.
- **Lakritz/Konfekt, the power was cut at the end of the install:**
  write `machboot-<board>.bin` at 0 over JTAG (see above), or write
  tinydfu back the same way.

When reporting a problem, the serial log helps most: on Sergei and
Mozart its `[sv]` lines are the RP2040's own. Add a photo of the
screen, the board and module, and `lsusb -v` for USB problems.
