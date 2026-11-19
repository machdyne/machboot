# The USB-C port: console and DFU

On a Lakritz, the USB-C socket that powers the board is also a USB
device: plug it into a PC and it shows up as

```
16d0:116d  Machdyne machboot
  interface 0+1  CDC-ACM serial port   "machboot console"
  interface 2    DFU                   "System slot + Zeitlos region"
  interface 3    mass storage          "machboot card reader": the SD card, when shared
```

16d0:116d is the ID tinydfu and Zeitlos's USB console use; nothing
needs a new udev rule.

## The console

```
minicom -D /dev/ttyACM0        (any baud rate)
```

It is the serial console (docs/ui.md, "The serial mirror") on the
USB port: every page as text, and keys typed there drive the menu --
arrows, Enter, Esc, F1-F12 as terminal escape sequences. It works
alongside the PMOD serial port and a USB keyboard.

machboot only sends once a terminal has the port open (DTR). A terminal
that stops reading is dropped after 50 ms of a full buffer, and picked
up again when it opens the port again.

## DFU

```
dfu-util -l
dfu-util -a 0 -D zeitlos-lakritz_gpio-dfu.bin     Zeitlos's release file, as with tinydfu
dfu-util -a 0 -D zeitlos.bit                 or just a bitstream
dfu-util -a 0 -U slot.bin                    read the system slot back (640 KB)
```

Alt 0 takes what tinydfu's alt 0 took. tinydfu wrote it at 0x040000;
machboot maps it onto its own layout:

| File offset | Goes to | |
|---|---|---|
| 0x000000-0x09FFFF | 0x060000-0x0FFFFF | the system slot |
| 0x0A0000-0x0BFFFF | -- | must be empty (0xFF); more than 640 KB of gateware is refused |
| 0x0C0000- | 0x100000- | the Zeitlos region (kernel, core apps) |

So a Zeitlos `-dfu.bin`, which is tinydfu's layout, puts the gateware
in the system slot and the rest where Zeitlos expects it; a plain
bitstream fills only the start of the slot.

**What is checked first:** the first block must be an ECP5 bitstream,
and for this FPGA (its VERIFY_ID). Otherwise dfu-util reports an
error, the screen says why ("not a bitstream", "for another FPGA"),
and **nothing is written**.

**Safe against interruptions**, as an install from the card
(docs/install.md): the slot's first sector is erased before anything
else is written and written last, after the whole file has arrived. A
download that stops halfway -- the cable pulled, dfu-util killed --
leaves "Zeitlos did not finish installing", never half a bitstream that
looks whole. Each block is one 4 KB flash sector (wTransferSize 4096);
sectors that already hold the same bytes are not written, the others
are read back.

**On the screen:** a line above the key line on every page,
"dfu-util: receiving ... 312 KB", then "dfu-util: done." or why not. A
download that starts during the countdown stops it, so the old system
is not started under it. Afterwards the menu stays open; Enter starts
what was written.

**Not offered** in install mode or the JTAG test build (machboot is not
at 0x000000 there): dfu-util gets an error and nothing is written.

The settings record the name -- "Zeitlos" for a file with a Zeitlos
region part; for a bare bitstream, the name of what was installed
before (it is taken to be a new version of it), else "Zeitlos" if a
Zeitlos kernel is at 0x100000, else "System" -- with no version, and the
size and CRC of the slot part.

## The card reader

**Card reader** in the menu, **Enter**: the SD card appears on the PC as
a USB drive ("Machdyne machboot card"), and the PC can copy files to it
and from it -- systems for `\SYS\GATEWARE`, DOS files, anything.
While it is shared, machboot does not touch the card.

It is given back to machboot when

- the PC ejects the drive (START STOP UNIT), or
- Enter on **Stop sharing**, or leaving the page (any way, F2 too).

Then machboot reads the card again, so Install sees what the PC copied.
If the card now has a newer version of the installed system (or any
system, when none is installed), the page's first line is **Install
<name> <version>**: Enter installs it, as Install would. That makes
"drag and drop" two steps: copy the `.bit` into
`\SYS\GATEWARE\<NAME>\` on the drive (`tools/mksys.py` does it),
eject, Enter.
Eject it on the PC before taking it back, as with any USB drive: the
PC may still have writes in its cache.

When the card is not shared, the drive is there but empty ("no
medium"), as a card reader with no card in it. On sharing, the PC is
told the medium changed (UNIT ATTENTION), so it looks again.

What the PC can send: the bulk-only transport (CBW / data / CSW) and
TEST UNIT READY, REQUEST SENSE, INQUIRY, MODE SENSE (6 and 10),
PREVENT/ALLOW MEDIUM REMOVAL, START STOP UNIT, READ CAPACITY (10),
READ FORMAT CAPACITIES, READ (10), WRITE (10), VERIFY (10) and
SYNCHRONIZE CACHE; anything else fails with ILLEGAL REQUEST. Writes go
to the card a sector at a time (SPI mode CMD24), so it is not fast --
about the speed of a slow USB stick.

Not offered while dfu-util is writing, and dfu-util is refused while
the card is shared (they share a buffer).

## How it is built

The hardware is `rtl/mb_usbd.v`: ulixxe's SIE (`rtl/ulixxe/`, MIT,
from Zeitlos's `rtl/ext/usb_cdc`) does the wire -- NRZI, bit
stuffing, CRCs, PIDs, data toggles, ACK/NAK/STALL -- and machboot's
block in front of it gives four endpoints, each a 64-byte OUT buffer
(full / not full; a full one NAKs) and a 64-byte IN buffer (armed / not
armed; not armed NAKs). The register map is in the file's header and
docs/memory-map.md. Everything else -- descriptors, chapter 9, CDC,
DFU -- is firmware (`fw/usbd.c`, about 6 KB), written from the USB 2.0,
CDC 1.2 and DFU 1.1 specifications, polled from the menu's key loop.

The SIE runs from the 48 MHz system clock with 4 samples per bit, as
in Zeitlos. The D+ pull-up is switched by `usb_ufp_pull` (R7): on
16 ms after the firmware attaches, released otherwise.

## Tested

- The PC build (`tests/run_host.py`, `ud_*`): a register-level model
  of the endpoint buffers with a host behind it: enumeration, the
  console both ways, DFU of a bitstream and of a Zeitlos `-dfu.bin`
  (byte for byte, nothing else touched), upload, a file for the 45F, a
  file that is not a bitstream, install mode, a download during the
  countdown; the card reader: no medium until shared, UNIT ATTENTION,
  capacity, READ (10) byte for byte, WRITE (10) there and nowhere else,
  eject from the PC, leaving the page.
- The simulation (`make sim`, `--usb`): the real RTL -- SIE, buffers,
  CPU, firmware -- against a USB host written at the wire from the
  specification (`sim/usbhost.inc`: NRZI, stuffing, CRC5/CRC16,
  retries on NAK): bus reset, enumeration, SET_ADDRESS, configuration,
  DTR, a DFU download into the slot, the console's IN data with its
  toggles, keys typed on the console reaching the menu, and the card
  reader: sharing from the menu, UNIT ATTENTION, then READ (10) of the
  card's first sectors byte for byte.

On a PC, `dfu-util -l` and `lsusb -v -d 16d0:116d` show the device.

## Not yet

- A drag-and-drop drive for gateware (a virtual disk whose files are
  the systems). The card reader and Install do the same in two steps.
- Windows: DFU there needs a WinUSB driver (Zadig), as for tinydfu.
