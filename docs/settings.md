# Settings

Kept in the flash, in Zeitlos's key/value log format (`fw/kvlog.c`, from
Zeitlos `sw/os/kvlog.c`, unchanged apart from the include): two 4 KB
sectors at **0x05E000-0x05FFFF**, the end of the boot slot. Records are
appended; a sector is erased only when one fills and the live records are
compacted into the other, whose header is written last -- a power cut at
any point leaves the old or the new settings, never neither (Zeitlos
`docs/kvstore.md`).

## The settings page

| Setting | Values | Key |
|---|---|---|
| Start by itself | the system / no | `boot.auto` |
| Wait | 1, 2, 3, 5, 10 seconds | `boot.wait` |
| Keyboard | US / Deutsch | `boot.kbd` |
| Language | English (German is planned) | -- |

**↑↓** choose, **←→** change, **Esc** saves and goes back to the
sections. Only values that changed are written. "Saved." confirms it.

With "Start by itself" off, the startup screen is skipped and the menu
opens on Start.

## What the installer records

| Key | |
|---|---|
| `sys.name` | "Zeitlos" -- the name the menu shows |
| `sys.ver` | "0.0.8" -- compared with the card's to offer an update |
| `sys.crc` | CRC-32 of the installed bitstream, hex |
| `sys.size` | its size in bytes |

Without these (a system written by dfu-util or JTAG), the name is a
guess: "Zeitlos" if a Zeitlos kernel is at 0x100000, else "System".

## When settings are not kept

In install mode and in the JTAG test build, settings can be changed for
the session but are not written ("Settings are kept once machboot is
installed."), for the same reason installing is not offered there
(docs/install.md).

## The window in the fence

The flash controller refuses writes below 0x060000 -- except to
0x05E000-0x05FFFF (`WIN_START`/`WIN_END`, the one change machboot makes
to Zeitlos's `spiflash.v`). The settings can be written; the bitstream
and the firmware in front of them cannot.
