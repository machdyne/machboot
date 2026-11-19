# Boards

One firmware (`build/fw.img`) for every board; the gateware tells it
which board it runs on (`SYS_BOARD`, docs/memory-map.md), how many USB
host ports there are, and where the system it starts lives (`SYS_SLOT`).

| Board | `make` | FPGA | What you get |
|---|---|---|---|
| Lakritz | `BOARD=lakritz` (default): `all sram installer` | LFE5U-25F | machboot in the boot slot (installed once, from tinydfu); the list of systems with one system slot; USB-C (console, DFU, card reader); 2 USB host ports |
| Konfekt | `BOARD=konfekt`: `all sram installer` | LFE5U-12F | the same as Lakritz, with 1 USB host port and the console on USB-C only |
| Sergei ML0 / ML1 / ML2 | `make sechzig uf2` (all six), or `BOARD=sergei_ml2 sram` | 25F / 45F / 45F | machboot loaded by the RP2040 at power-on; the list of systems kept in the RP2040's flash; the card reader through the RP2040; 2 USB host ports |
| Mozart ML0 / ML1 / ML2 | as Sergei | 25F / 45F / 45F | the same |

`BOARD` in the gateware: the carrier in bits 7:0 (1 Lakritz, 2 Sergei,
3 Mozart, 5 Konfekt), the Sechzig module in bits 15:8 (0, 1, 2). The
menu shows "Sergei ML2"; Install looks for `SERGEI2.BIT`, `MOZART0.BIT`,
`KONFEKT.BIT`, `LAKRITZ.BIT` in each system's folder.

## Konfekt

A Lakritz without the PMOD, on the 12F (`rtl/top_konfekt.v`). The same
flash layout, the same tinydfu (so the same install mode,
docs/install-mode.md, with `machboot-konfekt-install.bin`), the same
USB-C device. What differs: one USB-A socket (the controller reports one
port, and the Board page shows one), and no serial pads: the console is
the USB-C port's CDC-ACM (`/dev/ttyACM0`). Pins from Zeitlos's
`boards/konfekt_v0.lpf`, which was checked ball by ball against the
schematic.

## Sergei and Mozart (Sechzig ML0, ML1, ML2)

`rtl/top_sechzig.v`, one top for all six; pins from Zeitlos's
`boards/{sergei,mozart}_ml{1,2}.lpf`. ML0 is the ML1 module on a 25F
with the same balls, so it uses the ML1 file (Zeitlos docs/boards.md).

**The module's flash stays as it is**: its own system at 0, the Zeitlos
region at 0x100000. machboot never writes it. Instead the menu is
loaded into the FPGA's SRAM:

- by the RP2040, at power-on and after every reset, from its own flash:
  copy `machboot-sergei_ml2.uf2` (and so on, one per module) in BOOTSEL
  mode -- **docs/power-on.md**;
- or by you, for a try: `openFPGALoader -c dirtyJtag machboot-sergei_ml2-sram.bit`.

Starting the module's own system pulls PROGRAMN: the FPGA reconfigures
from the module's flash at 0, exactly as at power-on.

The console: machboot's UART0 is the module's UART_TX/UART_RX, which
the RP2040 bridges to USB: B3/A2 on ML2, **B4/C4 on ML0 and ML1** (on
those modules B3/A2 are XC/XD, the link to the RP2040).

- **Systems** other than the module's own are kept in the RP2040's flash
  and loaded over JTAG: docs/systems.md.
- **Settings** are kept in the RP2040's flash too, over the link:
  docs/power-on.md.
- **The card reader** goes through the RP2040 and the XA-XD link:
  docs/xlink.md.
- **The module's flash** is locked against machboot in hardware
  (`FLASH_LOCK_FROM 0`).
- **ML2's DDR3**, which machboot does not use, is held in reset while
  the menu runs, so the next system finds it as just after power-on.

The serial console is the module's UART0, which the RP2040 passes to the
USB-C port: `minicom -D /dev/ttyACM0 -b 1000000` (machboot's console
runs at 1 Mbaud; the bridge takes its rate from the terminal).

Build sizes and timing for every board: docs/building.md.
