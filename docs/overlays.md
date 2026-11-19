# Overlays: the menu's pages, loaded from flash (Lakritz, Konfekt)

The menu's RAM is 88 KB for code, data and stack together
(docs/memory-map.md), and on the 25F there is one block RAM to spare. So
on Lakritz and Konfekt the menu's pages are kept in flash and copied
into RAM only while they are shown, one page at a time.

## What is where

| | |
|---|---|
| resident (always in RAM) | the frame, the sidebar, the key loop, the startup screen and countdown, Start's boot path, installing (fw/install.c), the settings store, the card and FAT, USB host and device, the SD card, the console |
| overlays (one at a time, in a 3.5 KB window) | Start (the list, fw/page_systems.c), Install (fw/page_sysinst.c), Card reader, Settings, Diagnostics (with fw/diag.c), Board, install mode's page |

Each page is its own file, `fw/page_*.c`, with what the pages share in
`fw/ui_int.h`. An overlay's code and strings live in the overlay, but
its variables are resident, so a page keeps its state (Diagnostics'
results, the Settings message) when another page has been shown in
between.

The pages come to about 17 KB, the largest about 3.5 KB, so the overlays
save about 13 KB of RAM. fw/selfinst.c stays resident because
`selfinst_probe()` runs at start. The installed build's Start and Install
pages are the list of systems' (docs/systems.md); the single-system
pages (fw/page_start.c, fw/page_update.c) are only in the JTAG test
build.

## How

- **Building.** `make` compiles each overlay's files without LTO, as
  `build/ovl_<name>_*.o`. `fw/link_ovl.ld` places their `.text` and
  `.rodata` by file name into an `OVERLAY` with one window, right after
  `.data`. `NOCROSSREFS` makes the link fail if one page calls another.
  objcopy writes the payload without the overlays, then each overlay on
  its own. `tools/mkfw.py --ovl` appends a table and the overlays to
  `fw.img`.
- **In flash** (`tools/mkslot.py`): the table and overlays follow the
  payload, or follow the header when the payload had to go below it. The
  table is `"MBOV"`, a count, the payload's CRC-32, then per overlay its
  offset, length and CRC-32.
- **Loading** (`fw/ovl.c`, `ovl_use(id)`). If that page is already in the
  window, nothing happens. Otherwise its bytes are copied through the
  flash read window and checked against its CRC. The table must carry
  this payload's CRC, so pages from another build are never run.
  Resident code calls into a page only after `ovl_use()`: in redraw, and
  in the page's keys (Settings, Diagnostics, install mode). An overlay
  may call anything resident. A copy takes a few milliseconds, once per
  change of page.
- **If a page cannot be loaded** (damaged flash, or a firmware without
  its pages), the page says why and suggests installing machboot again.
  The rest of the menu, the countdown and Start keep working.

## Builds without overlays

- **The JTAG test build** (`make sram`, docs/testing.md) has its
  firmware preloaded in RAM and nothing in flash to load pages from. It
  is built with every page resident, from `build/fw-jtag.elf`, and has
  the single-system Start and Install pages. It is nearly full, so
  anything new on Lakritz has to stay out of that build or be small.
- **Sergei and Mozart** (SRAM builds, firmware preloaded): no overlays.
  They have about 2.7 KB of RAM spare.
- **The PC build** (`make host`): every page linked in. `ovl_use()` is
  always 1 there.

## Tested

`make sim` boots the real `machboot-lakritz.bin` (overlays in flash) and
the installer image in the gate-level simulation. It walks the pages,
installs from a card and runs install mode, all through `ovl_use()`.
`make host` covers the pages themselves.
