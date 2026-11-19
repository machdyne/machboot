# The list of systems

machboot keeps a list of the systems you use. **Start** shows them in
your order. The top one is the default: it starts by itself after the
wait, if Settings says so. **Install** adds systems from the SD card or a
USB drive.

The two kinds of board keep the systems differently:

- **Sergei and Mozart** keep several in the RP2040's flash. Each is
  copied there once, and switching between them writes nothing. This
  page mostly describes them.
- **Lakritz and Konfekt** have one system slot, and the Zeitlos region
  is always reserved. Switching to another system copies it into the
  slot. See "Lakritz and Konfekt: one slot" below.

## What you see

```
Start a system
──────────────
► Mach86 r31                       default, in flash
  Zeitlos 0.0.7                               module
  Kakao 0.3                              on the card

Enter starts Mach86 from flash (nothing is written).

The top one starts by itself after 3 seconds.

↑↓ Choose   Enter Start   F3 F4 Move   Del Remove   ← Menu
```

| Where it says | What Enter does |
|---|---|
| **module** | The module's own system, in the module's flash. Enter starts it with PROGRAMN, as before. machboot does not change the module's flash, so this entry cannot be removed. |
| **in flash** | It is in the RP2040's flash. The RP2040 loads it into the FPGA over JTAG, in about a second. Nothing is written. |
| **on the card** / **on the USB drive** | It is not in flash (yet, or any more). Enter copies it from the card into flash first, with a progress bar, then starts it. Esc cancels the copy. |
| **not found** | Not in flash and not on the card. Put the card with it in, or remove it from the list. |

**F3** moves the chosen system up and **F4** moves it down. Moving one to
the top makes it the default. **Del** removes one after asking: "Remove
Mach86 from the list? It is erased from flash; the card keeps its copy."

### Writes to flash are announced

Anything that will write flash says so on the page before you press
Enter, and says how long it takes:

- Start, on a system that is not in flash: "Enter copies Kakao from the
  SD card into flash (about 7 s), then starts it."
- Install: "Enter copies Kakao 0.3 from the SD card into flash (about
  7 s), and adds it to Start's list." For a newer version of one already
  there: "... in place of 0.2."
- When room has to be made: "To make room, Mach86 is removed from
  flash. It is copied back when it is started again."
- The countdown, when the default is not in flash: "Copying Kakao from
  the SD card into flash, then starting it, in 3 ..."
- Remove: "It is erased from flash; the card keeps its copy."

Moving systems in the list and changing settings are not announced.
They are small settings writes in the RP2040's flash (rp2040/src/store.c).

The times assume about 100 KB/s from card to flash (`RC_RATE`,
fw/rcache.h), an estimate until it is measured.

### When a system does not start

If the RP2040 cannot load a system (for example, the FPGA rejects it),
it loads the menu again and tells machboot why. Start then shows it in
red at the top, for example "Mach86 did not start (the FPGA rejected
it)". In that case nothing starts by itself, so one broken system does
not cause a boot loop.

## DDR3 (Sergei and Mozart ML2)

A system with DDR3 (Mach86) trains it when it starts. It has to find the
DDR3 as just after power-on, but on these boards the FPGA is never
switched off between systems. So the menu holds ML2's DDR3 in reset
the whole time it runs (RESET# and CKE low). Before a system from the
cache goes in, the supervisor first sets every pin to a safe state
(openFPGALoader's PRELOAD of ones), then erases the FPGA and leaves it
blank for 500 ms. At the end of the load it waits until the FPGA
reports it is done, and about 20 ms more, before releasing it. The
system's own power-up sequence follows from there.

## Where things are

- **The cache** is the RP2040's W25Q32 from 0x110000 to the end, about
  2.9 MB (docs/power-on.md has the whole layout). Each system takes one
  piece of whole 4 KB sectors: a 256-byte header ("MBSY", length, CRC-32,
  IDCODE, name, version), then the bitstream from the dummy bytes before
  its preamble. The .bit file's comment header is left behind, as
  openFPGALoader does. A system is known by the bitstream's CRC-32.
- **The header is written last**, after the bitstream has been written
  and read back against its CRC. A copy that was cancelled, or cut by a
  power cut, has no header. It is free space, and the list never shows
  it.
- **Making room.** A copy goes into the first gap big enough for it. If
  there is none, systems are removed from flash, starting from the
  bottom of the list, until a gap opens. Before those, anything in flash
  that the list does not know about goes first, and so does the older
  copy of the same system. The page names what will be removed before
  you press Enter.
- **The list** is kept with the settings, one line per system, in the
  order shown:
  `sys=c|<crc>|<len>|<name>|<version>` for a system in the cache, and
  `sys=m|0|0|<name>|<version>` for the module's own. If the list is
  lost, the systems still in flash are added back at the bottom.
- **Checked before every start.** The copy's CRC is checked again before
  it is loaded. A damaged one is not loaded: "Mach86 did not start (its
  copy in flash is damaged: remove it and install it again)".

## Lakritz and Konfekt: one slot

The flash is the MMOD's, which can be replaced, so writing it is
acceptable. Writes are still kept to a minimum, and each one is
announced first.

```
Start a system
──────────────
  Zeitlos 0.0.8                     default, in flash
► Mach86 0.1                              on the card

Enter copies Mach86 from the SD card into flash
(about 9 s), then starts it.
It replaces Zeitlos in flash.
It is copied back when it is started again.
```

- **in flash**: the system in the slot (0x060000-0x0FFFFF). Enter starts
  it with PROGRAMN and writes nothing. It cannot be removed from the
  list, because it is what the board boots.
- **on the card**: Enter copies it into the slot, then starts it. The
  copy is fw/install.c's, as before. It compares first, writes only the
  sectors that differ, and invalidates the slot before writing and
  validates it last, so a power cut never leaves something that looks
  whole. While it compares (the first 30 % of the bar), Esc stops it
  with nothing written. Once it writes, it cannot stop, and the screen
  says "Please do not switch off".
- **Install** only adds a system to the list: "Enter adds Mach86 0.1 to
  Start's list. Nothing is written now: it is copied into flash when it
  is started." Two cases copy at once, with "Enter copies ... into
  flash" said first: a newer version of the system in the slot, and the
  first system into an empty slot.
- **Remove** takes a system off the list only, and writes nothing else.
- **The Zeitlos region (0x100000, 1 MB) is always reserved.** Nothing
  machboot copies goes there, whatever is in the list.
- The list is the `sys.list` setting (names, in order; fw/settings.c),
  written only when it changes. A system that was in the slot before the
  list existed is put at its top.
- Times assume about 60 KB/s for a full copy, an estimate until it is
  measured.
  A copy of a system mostly the same as what is in the slot is quicker.
- The JTAG test build keeps the old Start and Install pages (it cannot
  write the slot). The list is in the installed build only, and its
  pages are overlays (docs/overlays.md).


## The Zeitlos region

Zeitlos reads its kernel from the module's flash at 0x100000 (the
"region"). On Sergei and Mozart that region stays where it is: the
module's flash is not machboot's, and the cache is in the RP2040's
flash, so the two never compete for room. Zeitlos started from the cache
uses the region already on the module. Installing a Zeitlos into the
cache does not write the region. A module without a region needs it
written into the module's flash some other way, for example with
openFPGALoader.

On Lakritz and Konfekt the region shares the flash with the system
slot, and it is always reserved: nothing machboot copies goes there,
even when Zeitlos is not in the list.

## How it works

machboot raises FLAGS bit 3 on the link, and the RP2040 asks what it
wants (REQ). LIST and DROP are answered at once. For a PUT, the RP2040
pulls the bitstream in 1536-byte CHUNKs and writes each one to its flash
as it comes in. It erases ahead in 64 KB blocks where they line up,
reads the whole copy back, and then writes the header. For a RUN, the
RP2040 answers first and then loads the system over JTAG. That ends
machboot. A later reset of the module, or a reboot of the system, brings
the menu back as before. docs/xlink.md has the messages.

While a request runs, the RP2040 does nothing else on the link. Its card
reader tells the PC there is no medium, as before the request, and USB
keeps going. openFPGALoader still wins JTAG: a PC that wants JTAG while a
system is being loaded gets it, and the load stops.

## Tested

- `make sv-test`, test_systems: the RP2040's own msc.c, cache.c, sv.c
  and ecp5.c against machboot's own fw/xlink.c, with an ECP5 model on
  JTAG. It covers LIST; PUT byte for byte (the .bit header left behind;
  IDCODE, name and version); a PUT cancelled a third of the way; no
  room; a 25F bitstream refused on a 45F; DROP; RUN loading every bit;
  and a broken system bringing the menu back, with machboot told why.
  cache.c alone: a copy cut by a power cut is invisible and its room is
  free; every erase is whole sectors and every program lands on erased
  flash. The store keeps a 700-byte text through 41 saves and a restart.
- `make host`, the sys_* cases (screens in build/host): install with the
  flash-write line, start a cached system (moved to the top) over JTAG,
  eviction named and done, Esc cancel, the countdown copying first, the
  note after a failed start, remove with its question. None of them
  writes the module's flash.
- Lakritz and Konfekt, `make host`, the slot_* cases (the installed
  build's code as a PC program): the list and its first entry; Install adding with only
  the settings written; Start copying a system byte for byte into the slot
  with the Zeitlos region untouched, then PROGRAMN; Esc while comparing
  leaving the slot as it was; a system moved to the top making the next
  countdown copy it first; Remove; an update of the slot's system copied
  at once.
