#!/usr/bin/env python3
"""
Builds the menu's 8x16 font ROM (build/font.hex) and its code page.

Code page: printable ASCII at 0x20-0x7E, and the graphics the menu
draws (lines, boxes, shades, arrows, a few marks) at the positions the
IBM PC's code page 437 gives them, so the byte values are familiar.
docs/font.md lists every glyph.

- ASCII 0x20-0x7F comes from tools/font/ascii8x16.mem, the 8x16 font in
  the Zeitlos tree (sw/data/font/font16.mem): 96 glyphs, each 16 lines
  of 8 space-separated bits, top row first.
- Every other glyph here is drawn from geometry below, so there is
  nothing to copy and nothing to license.

Output: 4096 lines of two hex digits, glyph * 16 + row, bit 7 leftmost.
Usage: mkfont.py OUT.hex [--unicode OUT.py]
"""
import sys

W, H = 8, 16
font = [[0] * H for _ in range(256)]

def load_ascii(path):
    vals = [int(v) for v in open(path).read().split()]
    assert len(vals) == 96 * H * W, len(vals)
    for g in range(96):
        for r in range(H):
            bits = vals[(g * H + r) * W:(g * H + r + 1) * W]
            b = 0
            for x in range(W):
                b |= bits[x] << (7 - x)
            font[0x20 + g][r] = b

def px(g, x, y):
    if 0 <= x < W and 0 <= y < H:
        font[g][y] |= 1 << (7 - x)

def hline(g, y, x0, x1):
    for x in range(x0, x1 + 1):
        px(g, x, y)

def vline(g, x, y0, y1):
    for y in range(y0, y1 + 1):
        px(g, x, y)

# single lines: centre column 3, centre row 7
CX, CY = 3, 7
def single(g, up, down, left, right):
    if up: vline(g, CX, 0, CY)
    if down: vline(g, CX, CY, H - 1)
    if left: hline(g, CY, 0, CX)
    if right: hline(g, CY, CX, W - 1)

# double lines: columns 2 and 4, rows 6 and 8
def dbl_h(g): hline(g, 6, 0, 7); hline(g, 8, 0, 7)
def dbl_v(g): vline(g, 2, 0, 15); vline(g, 4, 0, 15)

UNI = {}

def build():
    # box drawing, single
    for code, u, d, l, r, ch in [
        (0xC4, 0, 0, 1, 1, '─'), (0xB3, 1, 1, 0, 0, '│'),
        (0xDA, 0, 1, 0, 1, '┌'), (0xBF, 0, 1, 1, 0, '┐'),
        (0xC0, 1, 0, 0, 1, '└'), (0xD9, 1, 0, 1, 0, '┘'),
        (0xC3, 1, 1, 0, 1, '├'), (0xB4, 1, 1, 1, 0, '┤'),
        (0xC2, 0, 1, 1, 1, '┬'), (0xC1, 1, 0, 1, 1, '┴'),
        (0xC5, 1, 1, 1, 1, '┼')]:
        single(code, u, d, l, r); UNI[code] = ch
    # box drawing, double
    dbl_h(0xCD); UNI[0xCD] = '═'
    dbl_v(0xBA); UNI[0xBA] = '║'
    g = 0xC9; hline(g, 6, 2, 7); vline(g, 2, 6, 15); hline(g, 8, 4, 7); vline(g, 4, 8, 15); UNI[g] = '╔'
    g = 0xBB; hline(g, 6, 0, 4); vline(g, 4, 6, 15); hline(g, 8, 0, 2); vline(g, 2, 8, 15); UNI[g] = '╗'
    g = 0xC8; vline(g, 2, 0, 8); hline(g, 8, 2, 7); vline(g, 4, 0, 6); hline(g, 6, 4, 7); UNI[g] = '╚'
    g = 0xBC; vline(g, 4, 0, 8); hline(g, 8, 0, 4); vline(g, 2, 0, 6); hline(g, 6, 0, 2); UNI[g] = '╝'
    # blocks and shades
    for y in range(H):
        for x in range(W):
            if (x % 2 == 0) and (y % 2 == 0) and ((x // 2 + y // 2) % 2 == 0): px(0xB0, x, y)
            if (x + y) % 2 == 0: px(0xB1, x, y)
            if not ((x % 2 == 0) and (y % 2 == 0) and ((x // 2 + y // 2) % 2 == 0)): px(0xB2, x, y)
            px(0xDB, x, y)
            if y >= 8: px(0xDC, x, y)
            if y < 8: px(0xDF, x, y)
    UNI.update({0xB0: '░', 0xB1: '▒', 0xB2: '▓', 0xDB: '█', 0xDC: '▄', 0xDF: '▀'})
    # triangles
    for r, d in zip(range(4, 12), [1, 2, 3, 4, 4, 3, 2, 1]):
        hline(0x10, r, 1, min(7, d * 2 - 1 + 1))
        hline(0x11, r, max(0, 7 - (d * 2 - 1 + 1)), 6)
    UNI[0x10] = '►'; UNI[0x11] = '◄'
    # arrows
    vline(0x18, 3, 4, 12); px(0x18, 3, 4); hline(0x18, 5, 2, 4); hline(0x18, 6, 1, 5); UNI[0x18] = '↑'
    vline(0x19, 3, 3, 11); hline(0x19, 10, 1, 5); hline(0x19, 11, 2, 4); px(0x19, 3, 12); UNI[0x19] = '↓'
    hline(0x1A, 8, 1, 6); px(0x1A, 7, 8); vline(0x1A, 6, 7, 9); vline(0x1A, 5, 6, 10); UNI[0x1A] = '→'
    hline(0x1B, 8, 1, 6); px(0x1B, 0, 8); vline(0x1B, 1, 7, 9); vline(0x1B, 2, 6, 10); UNI[0x1B] = '←'
    # marks
    for r, (a, b) in zip(range(5, 11), [(2, 5), (1, 6), (1, 6), (1, 6), (1, 6), (2, 5)]):
        hline(0x07, r, a, b)
    UNI[0x07] = '●'
    for r in range(5, 11): hline(0xFE, r, 1, 6)
    UNI[0xFE] = '■'
    hline(0xFA, 7, 3, 4); hline(0xFA, 8, 3, 4); UNI[0xFA] = '·'
    for x, ys in [(1, [8]), (2, [9]), (3, [10]), (4, [9]), (5, [7, 8]), (6, [5, 6]), (7, [3, 4])]:
        for y in ys: px(0xFB, x, y)
    UNI[0xFB] = '√'
    # German: umlauts from the base letters plus two dots, and sharp s
    for code, base, ch in [(0x84, 'a', 'ä'), (0x94, 'o', 'ö'), (0x81, 'u', 'ü'),
                           (0x8E, 'A', 'Ä'), (0x99, 'O', 'Ö'), (0x9A, 'U', 'Ü')]:
        font[code] = list(font[ord(base)])
        top = next(r for r in range(H) if font[code][r])
        dot_row = max(0, top - 2)
        font[code][dot_row] |= (1 << (7 - 2)) | (1 << (7 - 5))
        UNI[code] = ch
    g = 0xE1
    vline(g, 1, 3, 13); hline(g, 2, 2, 4); px(g, 5, 3); px(g, 5, 4); px(g, 4, 5); px(g, 3, 6)
    px(g, 4, 7); px(g, 5, 8); vline(g, 6, 9, 11); px(g, 5, 12); hline(g, 13, 3, 4)
    UNI[g] = 'ß'

def main():
    load_ascii(sys.argv[0].rsplit('/', 1)[0] + '/font/ascii8x16.mem')
    build()
    with open(sys.argv[1], 'w') as f:
        for g in range(256):
            for r in range(H):
                f.write('%02x\n' % font[g][r])
    if '--unicode' in sys.argv:
        out = sys.argv[sys.argv.index('--unicode') + 1]
        with open(out, 'w') as f:
            f.write('# generated by tools/mkfont.py: machboot code page -> Unicode\n')
            f.write('CP = %r\n' % UNI)
            f.write('GLYPHS = %r\n' % [list(font[g]) for g in range(256)])

if __name__ == '__main__':
    main()
