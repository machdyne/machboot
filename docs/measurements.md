# Measurements (Phase 0)

What the menu costs, so the boot slot can be sized from numbers instead
of guesses. Round 1, 2026-11-17: yosys 0.71 (YoWASP), nextpnr-ecp5
(YoWASP), `synth_ecp5 -abc9`, speed grade 6, one place-and-route run
each. `make measure` regenerates the table.

## Round 6 (2026-11-18)

The supervisor with the power-on loader, the settings store and the
link client: 37 KB of the RP2040's flash. Menus in the UF2s: 370-435 KB
each (the SRAM bitstreams from their preamble). The gateware is
unchanged from round 5b; the Sergei/Mozart firmware grows by the SET/
GET/START messages (`_end` 0x12128 of 0x14800). Lakritz/Konfekt: 524
bytes of RAM left. Not measured: the JTAG load time on the board.

## Round 5a (2026-11-18)

The XA-XD link (two block RAMs) in the Sergei/Mozart builds: 9053-9127
COMB, 55 DP16KD, 52.2-55.9 MHz. Lakritz 9577 COMB, 55.6-56.0 MHz;
Konfekt 9240 COMB, 53.1-58.0 MHz. Every build meets 48 MHz
(docs/boards.md has the table). Firmware RAM: Lakritz/Konfekt `_end`
0x146C0 (320 bytes left); the Sergei/Mozart firmware 0x113A4 (13 KB
left: no USB device, no install mode). The card reader through the
link: about 290 KB/s reading, in the gate-level simulation.

## Round 4 (2026-11-18)

USB hubs and flash drives on the host ports (firmware only), and 8 KB
more RAM for them.

| | Round 3 | Round 4 |
|---|---|---|
| LUT (COMB), 25F | 9339 | 9603 (39%) |
| FF | 3710 | 3711 |
| DP16KD | 51 / 56 | **55 / 56** (RAM 40 -> 44) |
| bitstream, compressed | 250,207 bytes | 258,554 bytes (79% of 320 KB) |
| system clock, routed | 55.7-59.2 MHz | **52.6-55.1 MHz** over the three builds (48 needed) |
| firmware image | 51,428 bytes | 60,416 bytes: no longer fits after the header, so placed at 0x041000, below it |
| firmware RAM end (`_end`) | about 0x11B60 (of 0x12800) | 0x14668 (of 0x14800): 408 bytes left |
| installer image | 772,324 bytes | 720,928 bytes (92% of 768 KB) |
| RP2040 supervisor (`make sv`) | -- | machboot-sv.uf2, 49,664 bytes (24.8 KB of code) |

More boards (docs/boards.md has the table): Konfekt 9256 COMB, 55
DP16KD, 55.4 MHz; the six Sergei/Mozart SRAM builds 8554-8796 COMB, 53
DP16KD, 53.1-56.6 MHz. Every build meets 48 MHz.

One block RAM left (on the 25F and 12F), 408 bytes of RAM, and 4.6 MHz of timing margin.
The next feature that needs RAM needs a different idea (code loaded
from flash on demand), not another block RAM.

## Round 3 (2026-11-17, overnight)

The USB device (ulixxe's SIE and machboot's endpoint buffers) and 16 KB
more RAM.

| | Round 2 | Round 3 |
|---|---|---|
| LUT (COMB), 25F | 8264 | 9339 (38%) |
| FF | 3341 | 3710 |
| DP16KD | 41 / 56 | **51 / 56** (RAM 32 -> 40, USB buffers 2) |
| bitstream, compressed | 232,290 bytes | **250,207 bytes** (76% of 320 KB) |
| system clock, routed | 67.2 MHz | 55.7-59.2 MHz over the three builds (48 needed) |
| firmware image | 36,512 bytes | 51,428 bytes (of 56 KB) |
| firmware RAM end (`_end`) | 0xCBB8 (of 0xE000) | about 0x11B60 (of 0x12800) |
| installer image (`make installer`) | -- | 772,324 bytes (of the 768 KB before the Zeitlos region) |
| install mode, simulated | -- | 97 sector erases, 1,125 page programs, 11.4 s (the flash model is instant: add ~5 s of real erase time) |
| USB enumeration + 9 KB DFU + console + card reader, simulated | -- | about 200 ms of simulated time |

Both remaining budgets are now tight: 5 block RAMs, and about 3 KB of
the firmware's RAM. Next features must pay for themselves (docs/building.md).

## Round 2 (2026-11-17)

The SD card's SPI master added; nothing else in the gateware changed.

| | Round 1 | Round 2 |
|---|---|---|
| LUT (COMB), 25F | 7985 | 8264 (34%) |
| FF | 3183 | 3341 |
| DP16KD | 41 / 56 | 41 / 56 |
| bitstream, compressed | 226,937 bytes | **232,290 bytes** (71% of the 320 KB before the firmware) |
| system clock, routed | 57-67 MHz | 67.2 MHz (48 needed) |
| firmware image | 13,452 bytes | 36,512 bytes (of 48 KB) |
| firmware RAM end (`_end`) | 0x9A90 | 0xCBB8 (limit 0xE000) |
| boot, power-on to the first serial line (simulated) | 106 ms | 282 ms: a larger firmware to copy and check, and the card |

The firmware's RAM is now the tightest budget (docs/building.md).

## The menu, round 1

The round-1 gateware: zeitlos32 (RV32IM), 64 KB RAM, 1 KB boot ROM,
80x30 text mode with its font, DVI out, system registers, SPI flash,
UART, and Zeitlos's two-port USB host.

| Die | LUT (COMB) | FF | DP16KD | Bitstream (compressed) | Empty-die floor | Menu itself |
|---|---|---|---|---|---|---|
| 25F (Lakritz) | 7985 / 24288 (33%) | 3183 | 41 / 56 | **226,937 bytes (221 KB)** | ~99 KB | ~122 KB |
| 45F (Sergei, Schoko, Noir) | 7993 / 43848 (18%) | 3183 | 41 / 108 | **293,435 bytes (286 KB)** | ~162 KB | ~124 KB |

The "empty-die floor" is the size of a near-empty compressed bitstream
(Zeitlos `docs/zboot.md`, the jumploader table): every frame costs bytes
whatever it holds.

| Variant (25F) | LUT (COMB) | Bitstream |
|---|---|---|
| without the USB host | 5185 | 184,974 bytes |
| with the USB host | 7985 | 226,937 bytes |
| **the USB host costs** | **~2,800** | **~41 KB** |

Clocks, routed (all pass): system 48 MHz reaches 57-67 MHz depending on
the run; pixel 25.2 MHz reaches 87-102 MHz; the 126 MHz TMDS shift clock
reaches 300-360 MHz.

## What this decides

- **256 KB would not be enough on a 45F even today** (286 KB), and would
  leave a 25F only ~35 KB for SD, the USB device and anything else.
  The boot slot is **384 KB**: the bitstream gets 320 KB of it (to
  0x050000), the firmware and settings the rest.
- On a 25F the menu has **~99 KB** of bitstream left for the rest of the
  plan (SD controller, USB device, a little more). On a 45F a 320 KB
  bitstream space would leave ~34 KB: that is one more reason the
  Sergei/Mozart menu lives in the RP2040 firmware, where it is not
  squeezed (proposal section 3.3). Schoko and Noir (45F, stand-alone)
  will need a feature-trimmed menu or a 4 MB MMOD; decide when they are
  next.

## Initialised block RAM is expensive

The first round-1 build was **311,624 bytes**. The reason: every DP16KD
with initial contents costs about **2.3 KB of bitstream, uncompressed**
-- `ecppack --compress` does not shrink block-RAM initialisation, even
when it is all zeros. Measured: removing just the six all-zero
`.bram_init` sections from the config saved 13.9 KB.

The RAM's initial contents (the boot ROM, 1 KB) were spread across all 32
of its block RAMs, because yosys stripes a 32-bit RAM over them. So:

1. The boot ROM is now its own 256-word block RAM (`rtl/mb_ram.v`); the
   RAM has no initial contents.
2. The text RAM has no initial contents; the boot ROM clears it.
3. `tools/strip_bram_init.py` leaves every all-zero block-RAM init out
   of the bitstream (nextpnr writes them anyway).

Result: **226,937 bytes, -84 KB.** Only the boot ROM (1 DP16KD) and the
font (2) are initialised. This is also why the firmware lives in flash
next to the bitstream rather than in it (docs/boot.md).

## Firmware

| | Bytes |
|---|---|
| boot ROM | 630 (of 1,024) |
| firmware image | 13,452 (of 48 KB at 0x050000-0x05BFFF) |
| firmware RAM (code + data + bss) | ~39.6 KB of 56 KB below the stack |

Boot time (simulated): power-on to the startup screen's first line on
the serial console is **105 ms** -- 3,413 flash reads to copy the
firmware, then the CRC.

## Still to measure (needs boards)

On Sergei ML2, Phase 3: whether the RP2040 can load the FPGA's SRAM over
JTAG while it holds SYS_RST_N low; how long a load takes; JTAG-to-SPI
access to the module flash; the XA-XD SPI rate. On Lakritz: whether the
flash read speed matches the simulation (the Diagnostics page shows it).
