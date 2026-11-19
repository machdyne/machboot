# Font and code page

The menu's font is 8 x 16 pixels, 256 glyphs, in the gateware's font ROM
(`rtl/mb_text.v`, built by `tools/mkfont.py` into `build/font.hex`).

- **0x20-0x7E**: ASCII, from the 8x16 font in the Zeitlos tree
  (`sw/data/font/font16.mem`, copied as `tools/font/ascii8x16.mem`).
- **The graphics**: drawn by `tools/mkfont.py` from geometry (lines,
  shades, arrows, marks), at the positions IBM's code page 437 gives
  them, so the byte values are familiar. Nothing is copied for these.
- **German**: the umlauts are the base letters plus two dots, ß is drawn,
  at their code page 437 positions -- ready for the German language
  setting.

Everything else is blank. `fw/screen.h` names the graphics (`G_HLINE`,
`G_TRI_R`, ...); `scr_row_text()` turns them into ASCII for the serial
mirror.

| Code | Glyph |
|---|---|
| `0x07` | ● |
| `0x10` | ► |
| `0x11` | ◄ |
| `0x18` | ↑ |
| `0x19` | ↓ |
| `0x1A` | → |
| `0x1B` | ← |
| `0x81` | ü |
| `0x84` | ä |
| `0x8E` | Ä |
| `0x94` | ö |
| `0x99` | Ö |
| `0x9A` | Ü |
| `0xB0` | ░ |
| `0xB1` | ▒ |
| `0xB2` | ▓ |
| `0xB3` | │ |
| `0xB4` | ┤ |
| `0xBA` | ║ |
| `0xBB` | ╗ |
| `0xBC` | ╝ |
| `0xBF` | ┐ |
| `0xC0` | └ |
| `0xC1` | ┴ |
| `0xC2` | ┬ |
| `0xC3` | ├ |
| `0xC4` | ─ |
| `0xC5` | ┼ |
| `0xC8` | ╚ |
| `0xC9` | ╔ |
| `0xCD` | ═ |
| `0xD9` | ┘ |
| `0xDA` | ┌ |
| `0xDB` | █ |
| `0xDC` | ▄ |
| `0xDF` | ▀ |
| `0xE1` | ß |
| `0xFA` | · |
| `0xFB` | √ |
| `0xFE` | ■ |

The video pattern on the Diagnostics page shows every glyph above.

## Colours

The 16 colours of the classic PC text modes, in `rtl/mb_text.v`:

| | | | |
|---|---|---|---|
| 0 black 000000 | 4 red AA0000 | 8 dark grey 555555 | C light red FF5555 |
| 1 blue 0000AA | 5 magenta AA00AA | 9 light blue 5555FF | D light magenta FF55FF |
| 2 green 00AA00 | 6 brown AA5500 | A light green 55FF55 | E yellow FFFF55 |
| 3 cyan 00AAAA | 7 grey AAAAAA | B light cyan 55FFFF | F white FFFFFF |
