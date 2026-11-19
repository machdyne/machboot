# The interface

One startup screen; everything else is one menu in one frame. The
firmware's own screens, rendered, are in `build/host/*.png` after
`make host`.

## Startup

```
                                M A C H B O O T
                                Machdyne Lakritz

                           Starting Zeitlos in 3 ...
                            ████████████████░░░░░░░░

                              Enter Start now   F8 Menu
```

- **Enter** starts now; **F8**, **Esc**, **Del** or **m** opens the menu.
- If the card has a newer version of the installed system, one more
  line says so: "A new Zeitlos is on the card.  F2 Install". **F2** opens
  the menu on Install.
- If the default system is not in flash yet, the line says it is copied
  first: "Copying Mach86 from the SD card into flash, then starting it,
  in 3 ...".
- With no system to start (an empty list, a system that is not on the
  card, one that failed to start last time, install mode), or with
  "Start by itself" off, there is no countdown: the menu opens on Start.
- The wait is a setting (1-10 s, 3 by default).
- Keys work from a USB keyboard (docs/usb.md) and the serial console.

## The menu

```
 MACHBOOT                                                    Machdyne Lakritz
                         Start a system
  Start                  ──────────────
  Install                ► Zeitlos                      default, in flash
  Card reader              Mach86 r32                         on the card
  Settings
                         Enter starts Zeitlos from flash (nothing is
                         written).
  ──────────────────
  Run a file
  Diagnostics
  Board
 ↑↓ Choose   Enter Start   ← Menu   F1 Details
```

- Sections on the left, the page on the right, the page's keys on the
  bottom line.
- **← / Esc / Backspace** move to the sections; **↑↓** choose one;
  **→ / Enter** go back into the page. With the sections focused, a
  letter jumps: **s**tart, **i**nstall, **c**ard reader, se**t**tings,
  **r**un a file, **d**iagnostics, **b**oard.
- **F1** shows the technical side of a page (addresses, IDCODEs, the
  JEDEC ID) and hides it again.
- Sections not available on a board (Run a file, for now) are grey and
  say so.

### Pages

| Page | |
|---|---|
| Start | the list of systems, in your order; the top one is the default. Each says where it is: "in flash", "module" (Sergei, Mozart: the module's own), "on the card", or "not found". **Enter** starts the chosen one, copying it into flash first if it must (the page says so, and how long, before you press Enter; docs/systems.md). **F3/F4** move it up or down, **Del** removes it (after Y). If a system failed to start, the reason is shown in red at the top. In install mode, the page is **Install machboot** (docs/install-mode.md): Enter, then Y. F1: where it is kept, its size and CRC |
| Install | the systems on the card or a USB flash drive, each marked "installed", "new", "in the list" or "not for this board"; **Enter** installs: what it will write is said first (docs/systems.md); **F5** reads the card again. F1: the file, its size |
| Card reader | **Enter** shares the SD card with the PC as a USB drive; Enter again, leaving the page, or ejecting it on the PC gives it back (docs/usb-device.md). Read and written totals while shared |
| Settings | Start by itself, Wait, Keyboard, Language; **←→** change, **Esc** saves (docs/settings.md) |
| Diagnostics | Memory (march test of the menu's free RAM), Flash (ID, 64 KB read twice, speed), SD card (1 MB read, speed), Video (colour bars and every glyph). **A** runs memory, flash and SD card. |
| Board | board, FPGA, flash, video, system, SD card, USB 1 and 2, the USB-C port (console, DFU, whether a terminal is open), version. F1: IDCODE, JEDEC ID, where the firmware was found, the boot address, hardware version and features |

**F2** anywhere in the menu goes to Install.

**dfu-util** (docs/usb-device.md) shows on every page, one line above
the keys: "dfu-util: receiving ... 312 KB", then "dfu-util: done." or
why it was refused.

### How a system is named

From its `SYSTEM.CFG` when it was installed. A system written some other
way (dfu-util, JTAG) is named by a guess: "Zeitlos" if a Zeitlos kernel
is at 0x100000, otherwise "System".

## The serial console

Same pages, as text, at 1 Mbaud 8N1 on the UART (Lakritz: PMOD A pins
2/3, as Zeitlos's UART0), and on the USB-C port's serial port
(`/dev/ttyACM0`, docs/usb-device.md) once a terminal has it open. Each page is printed after every change, with
the section letters when the sections have the focus:

```
Sections:  s Start  i Install  c Card reader  t Settings ...
== Start a system ==
 > Zeitlos                            default, in flash

   Mach86 r32                               on the card

 Enter starts Zeitlos from flash (nothing is written).

 The top one starts by itself after 3 seconds.
[ ^v Choose   Enter Start   F3 F4 Move   Del Remove   < Menu]
```

Keys: the usual terminal sequences (minicom, picocom, screen, PuTTY):
arrows, Enter, Esc, F1-F12 (`ESC O P` or `ESC [ 11 ~` style), Delete,
and letters.

## Colours

| | Foreground | Background |
|---|---|---|
| title bar | black; "MACHBOOT" blue | grey |
| sections | black (grey if not available) | grey |
| current section | yellow (white with ► when focused) | blue |
| page | white; notes grey; details dark grey | blue |
| selected item | blue, its tag red | grey |
| problem line | yellow | red |
| key line | key red, label black | grey |
| startup | white; board dark grey; bar light green on dark grey | black |
