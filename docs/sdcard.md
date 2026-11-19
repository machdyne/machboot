# The SD card

machboot reads the SD card to find systems to install. machboot itself
never writes to the card; the PC can, through the card reader
(docs/usb-device.md), which writes the sectors the PC sends and nothing
else.

## Hardware

Zeitlos's SPI master (`rtl/zeitlos/spim.v`) at `0x5000_0000`: DATA stalls
while a transfer runs, so `write; read` is a whole exchange, and CTRL
bit 1 makes a transfer four bytes (used for the 512-byte data blocks).
The clock starts at 400 kHz for initialisation and runs at 24 MHz after
it, as Zeitlos runs the same controller: SD's
SPI mode goes to 25 MHz. A card that misbehaves at 24 MHz: `DIV_FAST` 1
in `fw/sd.c`. Lakritz: SCK F2, MOSI K1, SS K2, MISO F3.

## The driver (`fw/sd.c`)

Written from the SD Physical Layer Simplified Specification, chapter 7
(SPI mode): 74+ clocks with CS high; CMD0; CMD8 (a v2 card echoes 0xAA, a
v1 card says "illegal command"); ACMD41 until ready, with HCS for v2;
CMD58 for the CCS bit (block addressing); CMD16 512 for byte-addressed
cards; CMD9 for the capacity; CMD17 to read a sector; CMD24 to write one
(the card reader only: start token, 512 bytes, the data response, busy). No CRC checks, no
writing.

## FAT (`fw/fat.c`)

Written from Microsoft's FAT specification (fatgen103). FAT12, FAT16 and
FAT32, with or without a partition table (MBR types 0x01, 0x04, 0x06,
0x0B, 0x0C, 0x0E). Read only, short names only: long-name entries are
skipped, and every FAT driver writes a short name beside a long one.

**A shared card.** The card may also hold DOS for Mach86 -- Zeitlos and
the DOS boot files on the first partition, CD images on the second. Every
FAT partition is searched for `\SYS\GATEWARE`, in order; the first one
that has it is used.

## What machboot looks for

```
\SYS\GATEWARE\ZEITLOS\SYSTEM.CFG     name=Zeitlos
                                     version=0.0.8
\SYS\GATEWARE\ZEITLOS\LAKRITZ.BIT    the bitstream for this board
\SYS\GATEWARE\ZEITLOS\SERGEI2.BIT    ... and for others
\SYS\GATEWARE\MACH86\SYSTEM.CFG
\SYS\GATEWARE\MACH86\LAKRITZ.BIT
\SYS\GATEWARE\BLINK.BIT              a loose file (for "Run a file", not available yet)
```

- A **system** is a folder with a `SYSTEM.CFG`. Lines are `key=value`;
  `#` starts a comment; spaces around `=` are allowed. `name` and
  `version` are used today; anything else is ignored. A folder without a
  `SYSTEM.CFG` is ignored.
- **Bitstream names** are per board, 8.3 so DOS can see them: `LAKRITZ`,
  `SERGEI2`, `MOZART2`, `OBST`, `KONFEKT`, `MINZE`, `SCHOKO`, `NOIR`,
  `KLINGE`, with `.BIT`. A system without one for this board is listed
  as "not for this board".
- A bitstream may be a `.bit` file (with its header) or a raw `.bin`
  renamed to `.BIT`: either way it starts like an ECP5 bitstream, and
  that is what is checked (docs/install.md).

The card is read once at power-on, and again with **F5** on the Install
page (after changing cards).

`tools/mksys.py` puts a system on a mounted card (or into a folder to
copy):

```
python3 tools/mksys.py /media/$USER/SDCARD zeitlos-lakritz_gpio-gateware.bit --name Zeitlos --version 0.0.8
```

## Diagnostics

"SD card" reads the first 1 MB and reports the speed.
