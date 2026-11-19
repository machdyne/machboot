# Boot

## On a stand-alone board (Lakritz)

```
power on
  -> FPGA loads 0x000000           the menu's bitstream (boot address 0x060000)
  -> CPU resets to 0               the boot ROM (bootrom/, 630 bytes, in its own block RAM)
     -> clears the text RAM, sets up the UART
     -> looks for the firmware at 0x050000, then 0x090000
     -> copies it to RAM at 0x400, checks its CRC-32, jumps to it with a0 = where it found it
  -> firmware: probes the board, flash, system slot
     -> startup screen, 3 s countdown           Enter: start now   F8 / Esc / m: menu
     -> start: PROGRAMN
  -> FPGA loads 0x060000           the system
  -> the system pulls PROGRAMN (Zeitlos `reboot`)
  -> FPGA loads 0x000000 again (the system's boot address is 0): the menu
```

Power-on always loads 0x000000, so whatever a system does, a power cycle
comes back to the menu.

## The firmware header

32 bytes, little-endian, written by `tools/mkfw.py`, checked by
`bootrom/boot.c`:

| Offset | | |
|---|---|---|
| 0 | magic | "MBFW" (0x5746424D) |
| 4 | hwver | the register-map version it needs (rtl/mb_sysctl.v HWVER); 2 |
| 8 | length | payload bytes |
| 12 | load | 0x400 |
| 16 | entry | 0x400 |
| 20 | crc | CRC-32 (IEEE, as zlib) of the payload |
| 24 | version | major << 16 / minor << 8 / patch |
| 28 | at | 0: the payload follows the header. Otherwise where it is, in bytes from the header (negative: below it) |

The header is always at 0x050000 (0x090000 in install mode). A
firmware too big for the 56 KB above the header goes below it,
4 KB aligned, in the room the bitstream leaves (`tools/mkslot.py`
places it and checks it does not meet the bitstream); word 7 says where.
The CRC covers only the payload, so the placement does not change it.
On Lakritz and Konfekt the menu's pages (overlays) come after the
payload, or right after the header when the payload is below it. The
boot ROM does not load them; the firmware does, one page at a time
(docs/overlays.md).

The boot ROM refuses a header whose hwver is not 2, whose image would
not fit below 0x14800 (88 KB of RAM; 6 KB are left for the stack), or whose CRC is
wrong. If neither place has a good one it says so -- on the serial
console, and white on red on the screen -- and blinks the LED.

**Why the firmware is not baked into the bitstream.** It could be (as
BRAM contents), but then every firmware change means a new bitstream,
and initialised block RAM is expensive in bitstream size (it does not
compress). This way the firmware can be
updated on its own, and the gateware's only initialised memories are
the 1 KB boot ROM and the 4 KB font.

## Install mode

The machboot image is the same file whether it is at 0x000000 or where
tinydfu's user alt writes it, 0x040000 (`dfu-util -a 0 -D
machboot-lakritz.bin`): the boot ROM finds the firmware at 0x050000 in
the first case and at 0x090000 in the second, and passes which to the
firmware.

Running from 0x040000 (`mb.install_mode`), the system slot 0x060000 is
inside machboot's own image, so the firmware never treats it as a
system: its Start page is **Install machboot**, which copies machboot's
own image down to 0x000000, block by block (docs/install-mode.md).

## Starting the system

`ui_boot()` writes "ZRBT" (0x5A524254) to `RECONFIG`
([memory-map.md](memory-map.md)); PROGRAMN is pulled and the FPGA
reconfigures from the menu bitstream's boot address. If half a second
later the CPU is still running, the screen says "The FPGA did not
restart (PROGRAMN)".

The firmware only starts a slot that holds an ECP5 bitstream whose
VERIFY_ID matches this FPGA (`fw/flash.c`); otherwise Start explains
what is wrong.
