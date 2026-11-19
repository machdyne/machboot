# Sergei and Mozart: the menu at power-on

On Lakritz and Konfekt machboot lives at the start of the FPGA's own
flash. On Sergei and Mozart the module's flash stays the installed
system's, as it is without machboot, and the menu lives in the RP2040's
flash instead. The RP2040 loads it into the FPGA's SRAM over JTAG
(rp2040/, docs/rp2040.md).

## Installing

```
hold BOOT, power on       -> a drive called RPI-RP2
copy machboot-sergei_ml2.uf2 (or -sergei_ml0/_ml1, -mozart_ml0/1/2) to it
power off and on
```

Each of these files carries the supervisor and the menu for **one**
module. The RP2040 cannot tell an ML1 from an ML2: both have a 45F with
the same IDCODE, but different pins. So use the file for your module.
A file for a 25F (ML0) is refused on a 45F and the other way round,
before anything is loaded; the module's flash then boots as before.

`machboot-sv.uf2` is the supervisor alone: JTAG, the serial bridge, the
card reader, no menu. It also blanks the menu header, so a menu from an
earlier file is not loaded any more. The stock `dirtyJtag.uf2` goes back
to the board as shipped. Settings and the system cache survive all of
these: no UF2 writes where they are.

The RP2040's flash (W25Q32, 4 MB):

| Address | Holds |
|---|---|
| 0x000000 | the supervisor's firmware |
| 0x080000 | the menu: a 256-byte header, then the bitstream (512 KB) |
| 0x100000 | the settings and the list of systems, 64 KB (rp2040/src/store.c) |
| 0x110000 to the end | the system cache, about 2.9 MB on a 4 MB chip (rp2040/src/cache.c, docs/systems.md); the end is the chip's own size, read from its JEDEC ID at start |


## What happens

```
power on   RP2040: clock out (GP23), then IDCODE over JTAG; the FPGA has
           begun loading the module's flash -- ISC_ENABLE stops that
           -> the menu into SRAM (about 430 KB over JTAG)
           -> machboot runs: the RP2040 hands it the settings it keeps,
              before the countdown
countdown  -> Start: machboot tells the RP2040 (START), then pulls PROGRAMN
           -> the FPGA boots the module's flash (Zeitlos, Kakao, ...);
              the RP2040 lets that reset through and leaves XA-XD alone
reboot     the system's own reboot, or the reset button: SYS_RST_N goes
           low -> the menu again
```

| Event | The RP2040 |
|---|---|
| power on, or the RP2040's reset | loads the menu |
| a UF2 copied (the boot ROM restarts it by the watchdog) | does **not** load: the FPGA keeps what it has; power-cycle for the menu |
| SYS_RST_N low after a START | nothing: the system boots |
| SYS_RST_N low otherwise (reset button, `reboot`) | loads the menu once SYS_RST_N is high again |
| a PC using JTAG (openFPGALoader) | never loads; a load in progress stops (the PC wins); a reset during the PC's session is the PC's |

SYS_RST_N goes from the carrier through 51 Ohm to the FPGA's PROGRAMN
(R9) and to ball M8, which machboot (and Zeitlos) pull to reconfigure
(modules' PCBs, R6 on ML1, R9 on ML2). The RP2040 watches it on GP20:
the IO bank latches the falling edge, so even a short pulse is seen.
The RP2040 never drives it.

## Loading over JTAG (rp2040/src/ecp5.c)

The ECP5 SRAM sequence (Lattice TN1260; openFPGALoader runs the same):

```
IDCODE (0xE0)                must be the menu's
ISC_ENABLE (0xC6), 0x00      SRAM; also stops a flash load in progress
ISC_ERASE (0x0E), 0x01       the SRAM
LSC_INIT_ADDRESS (0x46)
LSC_BITSTREAM_BURST (0x7A)   the bitstream, each byte MSB first, 4 KB at a time
                             (USB serviced between; a PC's JTAG stops it)
LSC_READ_STATUS (0x3C)       bits 26:23 must be 0
ISC_DISABLE (0x26)           the FPGA wakes up with the menu
```

A load that fails on the FPGA's side (at power-on it may still be busy
loading its own flash) is tried again every 300 ms for about 5 s. The
module's system may start meanwhile; the menu replaces it.

**The supervisor's log.** Open `/dev/ttyACM0` (`minicom -D /dev/ttyACM0
-b 1000000`): `[sv ...]` lines from power-on come first -- the reset
reasons, whether a menu is stored and whole, each load and its result
with the IDCODE read and the time it took -- then machboot's console.
A serial break (minicom: Ctrl-A F) loads the menu at once. `lsusb -v -d
1209:c0ca` shows the supervisor's version in iProduct and bcdDevice.

The JTAG pins are driven from the CPU at 6 MHz, as for the PC's
dirtyJTAG commands. On a Sergei ML2 the menu (434 KB) loads in about
1.1 s, and Mach86 (804 KB) from the cache in about 2.6 s.

## Settings (rp2040/src/store.c)

The Settings page works as on Lakritz. Here the RP2040 keeps the
settings, in its own flash: a log of 1 KB records in 64 KB at
0x100000, with the list of systems in them too. Each change is one record written; a sector is erased only when
the log comes round to it. A record carries a CRC, so the newest intact
record survives a power cut mid-write. Text, `boot.auto=1`,
`boot.wait=3`, `boot.kbd=us`, one per line.

Over the link (docs/xlink.md): when machboot first answers, the RP2040
sends **SET** with what it keeps; machboot waits up to 400 ms for it at
start, so the countdown already follows it. A countdown that is running
when settings arrive later (when the PC loaded the menu) follows them
too. When you leave the Settings page with a change, machboot raises
FLAGS bit 2; the RP2040 **GET**s the text and writes it if it differs.

## Tested

- `make sv-test`, `test_ecp5`: the loader against a model ECP5
  configuration engine (ISC_EN, busy, erase, burst rebuilt into bytes,
  status, DONE): byte for byte, another FPGA refused untouched, the
  PC taking JTAG mid-load, a rejected bitstream.
- `test_sv`: all of it at once -- sv.c, ecp5.c, msc.c, xlink.c, store.c
  against the model module running machboot's own fw/xlink.c: power-on
  load, settings before the countdown, a change kept (and not rewritten
  when unchanged), surviving a power cycle, Start letting its reset
  through, 10 s of the system without a single XA-XD frame, a reboot
  back to the menu, a reset during a PC's session left alone, a few
  probes after a PC's session and then quiet, a load stopped for the PC,
  a menu for another die refused, no menu, a damaged menu (CRC), a
  refused ISC_ENABLE tried again.
- `make sim`, `sim-xl` (gate level): settings handed over at boot and a
  boot.auto=0 obeyed (no countdown); a change on the Settings page
  fetched by the RP2040; Start: START before PROGRAMN.
- `make host`: `xl_settings_in`, `xl_settings_out`, `xl_start`,
  `xl_start_alone`.

On a Sergei ML2 these have also been tried by hand: the RP2040's flash
writes, the reset-edge latch, the reset reasons and JTAG on the real
pins.
