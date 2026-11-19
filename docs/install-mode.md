# Install mode: replacing tinydfu with machboot

A Lakritz that has the DFU bootloader (tinydfu) installs machboot over
USB, with `dfu-util`, without a JTAG cable. The installer is machboot
itself, in a special image.

## What you do

1. Put an SD card in, with Zeitlos on it under `\SYS\GATEWARE\ZEITLOS\`
   (docs/sdcard.md). Optional: without it, machboot is installed alone,
   and you install a system later from **Install**.
2. Within 5 seconds of power-on, while tinydfu is listening:

   ```
   dfu-util -a 0 -D machboot-lakritz-install.bin
   ```

   `dfu-util` warns that the file has no DFU suffix; that is expected.
   tinydfu writes the file at 0x040000, its user area, and starts it.
3. The menu opens on **Install machboot**: "machboot is running from
   where dfu-util put it." Press **Enter**, then **Y** to the question
   "Replace tinydfu with machboot now?".
4. A progress bar; about 20 seconds. Keep the board powered.
5. "machboot is installed." The board restarts into machboot, which
   counts down and starts Zeitlos.

Keys come from a USB keyboard or the serial console, as everywhere.

## What it writes

```
before (tinydfu)                      after (machboot)
0x000000  tinydfu, 256 KB             0x000000  machboot: bitstream, firmware, settings (384 KB)
0x040000  the installer image         0x060000  Zeitlos's gateware, from the card
          (written by dfu-util)                 (or nothing: "No system is installed yet.")
0x100000  Zeitlos region              0x100000  Zeitlos region -- untouched
```

The installer image, at 0x040000 (`tools/mkinstaller.py`):

| Offset | Address | What |
|---|---|---|
| +0x000000 | 0x040000 | the installer's bitstream |
| +0x050000 | 0x090000 | the firmware (the same as machboot's) |
| +0x05E000 | 0x09E000 | the installer header: payload offset, length, CRC-32, IDCODE, version |
| +0x060000 | 0x0A0000 | the payload: `machboot-lakritz.bin`, the boot slot for 0x000000 |

It ends before 0x100000 by construction (0x060000 + at most 0x05E000),
so `dfu-util` cannot reach the Zeitlos region either.

The boot ROM finds the firmware at 0x090000 (it looks at 0x050000
first, which on a tinydfu board is part of the old Zeitlos gateware and
never a machboot header), and the firmware knows from that address that
it is in install mode.

## Why the installer is a separate gateware build

The normal machboot gateware cannot write its own boot slot: the flash
controller's fence (rtl/zeitlos/spiflash.v) refuses every erase and
program below 0x060000 except the settings sectors, whatever the
firmware does. That is the point of it, and install mode must not have a
software switch that turns it off.

So the installer is built from the same RTL with `INSTALLER=1`
(`make installer`, `-DMB_INSTALLER`). Its fence is turned around:

| | normal build | installer build |
|---|---|---|
| refused | below 0x060000, except 0x05E000-0x05FFFF | 0x100000 and up |
| allowed | 0x05E000 and up | 0x000000-0x0FFFFF |
| FEATURES bit 6 | 0 | 1 |
| packed with boot address | 0x060000 (the system slot) | 0 (the new machboot) |

The installer can therefore never touch the Zeitlos region, and the
installed machboot can never touch its own boot slot. The firmware only
offers to install when FEATURES bit 6 says it runs on the installer
gateware, from 0x090000, with a valid header. Anything else -- the plain
`machboot-lakritz.bin` written with dfu-util, say -- shows "This image
cannot install itself" and writes nothing.

## The order of the writes, and power cuts

1. The payload's CRC-32 is checked against the header, and its start
   must be a bitstream for this FPGA with machboot's firmware at
   0x050000. Otherwise: "This machboot installer is damaged."
   **Nothing is written.**
2. The boot slot is written **from the top down**, 0x05F000 first,
   0x000000 last, 4 KB at a time. Each sector is compared first and
   written only if it differs, then read back; a sector that reads back
   wrong is written once more.
3. The whole boot slot is checked against the CRC.
4. The installer is made invisible: the first sector of the system slot
   (0x060000, which is now the middle of the installer's bitstream) and
   the installer's firmware header (0x090000) are erased.
5. The settings are written at their new place (0x05E000), with the
   keyboard layout chosen during the install.
6. The system from the card is installed into the system slot, exactly
   as **Install** does it (docs/install.md): Zeitlos if the card has it,
   otherwise the first system for this board.
7. PROGRAMN. The installer was packed with boot address 0, so the FPGA
   loads the new machboot.

**A power cut.** Writing from the top down keeps tinydfu whole for as
long as possible: tinydfu is about 125 KB at the bottom of the flash,
and the last ~2 seconds of step 2 are the only time it is incomplete.

- Before step 2 reaches tinydfu's sectors: tinydfu still starts. Its
  user image at 0x040000 is now damaged, so it does not start anything,
  but it still listens for 5 seconds: write the installer again with
  `dfu-util`, and install again. (Tested: tests/run_host.py,
  `im_cut_early` and `im_cut_again`.)
- During the last ~2 seconds: the board does not start; JTAG
  (`openFPGALoader -f -o 0 machboot-lakritz.bin`) or an MMOD programmer
  brings it back -- the same risk as upgrading tinydfu itself (Zeitlos
  docs/dfu_upgrade.md).
- After step 3: machboot starts. If the system was not written yet,
  Start says so and Install installs it.

**A write that fails** (the flash refuses or reads back wrong twice)
stops with "Installing machboot did not finish" and the address. The
firmware is still running from RAM, so Enter tries again, from where it
stopped (sectors already right are skipped). Do not switch off until it
is done.

## Afterwards: dfu-util and tinydfu

After the install, tinydfu is gone, and machboot answers on the USB-C
port instead: `dfu-util -a 0 -D zeitlos-lakritz_gpio-dfu.bin` works as it
did with tinydfu (docs/usb-device.md). Systems also come from the card
(**Install**). Zeitlos keeps working as before, from 0x060000, once it
has the two changes in docs/zeitlos.md.

## Testing

- `tests/run_host.py`: the PC build, `im_*` cases: the offer, cancel,
  install with and without a card (byte-for-byte checks of the boot
  slot, the system slot, the Zeitlos region, the settings), a power cut
  and a second `dfu-util`, a damaged image, the plain image, an
  installer for another FPGA.
- `make sim`: the installer **gateware** in CXXRTL, on a flash with a
  stand-in tinydfu and the installer at 0x040000: Enter, Y, and the
  install through the real flash controller and its turned-around
  fence, up to PROGRAMN.
