# Memory map

The menu's SoC (`rtl/mb_soc.v`): one zeitlos32 (RV32IM, no interrupts
used) on a Wishbone classic bus. The top four address bits select the
device; anything unmapped is acknowledged at once and reads 0.

| Address | Device | |
|---|---|---|
| `0x0000_0000` | RAM, 88 KB (to 0x15FFF) | 0x000-0x3FF read from the boot ROM (writes ignored); the firmware runs from 0x400; on Lakritz and Konfekt a window after its data holds the page being shown (docs/overlays.md); stack at the top (0x16000, 6 KB) |
| `0x1000_0000` | SPI flash, read window | flash offset = low 24 bits; byte, half and word reads |
| `0x1F00_0000` | SPI flash registers | Zeitlos `docs/spiflash.md`: MAGIC, STATUS, ID (JEDEC), LOCK, ADDR, ARM, CMD, LEN, page buffer at +0x100; machboot adds LOCK_FROM (read at +0x18). Writes refused below 0x060000 except the settings window 0x05E000-0x05FFFF; in the installer build (docs/install-mode.md) refused from 0x100000 up instead |
| `0x2000_0000` | text cells | write-only, one word per cell, 2400 cells (80 x 30) |
| `0x3000_0000` | system registers | below |
| `0x4000_0000` | USB host | Zeitlos `docs/usb_host.md`; compat blocks at +0x000/+0x020, control at +0x100, poll slots at +0x200, packet buffer at +0x1000 (docs/usb.md) |
| `0x5000_0000` | SD card SPI master | Zeitlos `rtl/spim.v`: DATA, STATUS, CTRL (bit 0 CS, bit 1 32-bit, bits 15:8 divider), MAGIC "SPI1" (docs/sdcard.md) |
| `0x6000_0000` | USB device | `rtl/mb_usbd.v`, below; FEATURES bit 2 |
| `0x7000_0000` | the XA-XD link (Sergei, Mozart) | `rtl/mb_xlink.v`, docs/xlink.md; FEATURES bit 4 |
| `0xF000_0000` | UART | 16550 layout, registers at word stride (Zeitlos `rtl/uart.v`) |

## Text cells

```
bits 7:0    character (docs/font.md)
bits 11:8   foreground colour
bits 15:12  background colour
```

Cell n is row n / 80, column n % 80. The text RAM has no initial
contents; the boot ROM clears it.

## System registers (`rtl/mb_sysctl.v`)

| Offset | Name | |
|---|---|---|
| 0x00 | MAGIC | r: 0x4D424F54 ("MBOT") |
| 0x04 | HWVER | r: register-map version: 2. The boot ROM and the firmware stop if they do not know it. |
| 0x08 | MS | r: milliseconds since reset |
| 0x0C | CYCLES | r: 48 MHz cycles, low 32 bits |
| 0x10 | LED | rw: bit 0 |
| 0x14 | RECONFIG | w: 0x5A524254 ("ZRBT") as one word store pulls PROGRAMN; r: {0x5A52, 15'b0, available} |
| 0x18 | BOARD | r: board number, below |
| 0x1C | FEATURES | r: bit 0 video, 1 USB host, 2 USB device, 3 SD, 4 RP2040 link, 5 PROGRAMN, 6 installer build (docs/install-mode.md) |
| 0x20 | IDCODE | r: the IDCODE of the FPGA this bitstream was built for |
| 0x24 | FRAMES | r: video frames since reset |
| 0x28 | SLOT | r: the system slot this bitstream boots (its ecppack boot address) |

The RECONFIG key is the same as Zeitlos's `socctl` RECONFIG register.

## USB device (`rtl/mb_usbd.v`, docs/usb-device.md)

| Offset | Name | |
|---|---|---|
| 0x000 | MAGIC | r: 0x44425355 ("USBD") |
| 0x004 | CTRL | rw: bit 0 attach (pull-up and receiver on), bits 14:8 device address |
| 0x008 | EVENTS | r: bit 0 a bus reset happened (it also cleared the address and every endpoint); w: 1 clears |
| 0x00C | FRAME | r: the last SOF's frame number |
| 0x010 | EPTYPE | rw: bits 3:0 IN bulk, 7:4 OUT bulk, 11:8 IN interrupt (endpoint 0 is control) |
| 0x014 | TOGGLE | w: bits 3:0 IN, 7:4 OUT: reset the data toggle to DATA0 |
| 0x040 + 16n | OUT n | r: bit 0 full, bit 1 SETUP, bits 14:8 length; w: empty it |
| 0x044 + 16n | IN n | r: bit 8 armed; w: bits 6:0 length, bit 8 arm |
| 0x048 + 16n | STALL n | rw: bit 0 |
| 0x400 + 0x80n | OUT buffer n | 64 bytes, read |
| 0x440 + 0x80n | IN buffer n | 64 bytes, write |

## The XA-XD link (`rtl/mb_xlink.v`, docs/xlink.md)

| Offset | | |
|---|---|---|
| 0x0000 | MAGIC | r: "XLNK" |
| 0x0004 | RX | r: bit 0 full, 26:16 length, 15:8 RXSEQ; w: bit 0 = 1 empties it |
| 0x0008 | TX | r: bit 0 armed; w: bits 10:0 length (0 = 2048), bit 31 arm |
| 0x000C | FLAGS | rw: bits 7:0, sent in every STATUS frame |
| 0x0010 | FRAMES | r: frames seen |
| 0x1000 | receive buffer | 2 KB, read |
| 0x1800 | reply buffer | 2 KB, write (byte lanes) |

## Boards

Bits 7:0 the board (the carrier); bits 15:8 the Sechzig module on Sergei
and Mozart (0, 1, 2: ML0, ML1, ML2). docs/boards.md.

| BOARD | |
|---|---|
| 1 | Lakritz |
| 2 | Sergei (0x002, 0x102, 0x202: ML0, ML1, ML2) |
| 3 | Mozart (0x003, 0x103, 0x203) |
| 4 | Obst |
| 5 | Konfekt |
| 6 | Minze |
| 7 | Schoko |
| 8 | Noir |
| 9 | Klinge |

## Clocks

| Clock | | |
|---|---|---|
| system | 48 MHz | the board oscillator, directly |
| pixel | 25.2 MHz | PLL (`rtl/mb_pll.v`, made with ecppll) |
| TMDS shift | 126 MHz | same PLL, 5 x pixel |
