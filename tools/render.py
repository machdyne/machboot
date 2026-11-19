#!/usr/bin/env python3
"""
Render a screen dump (80 x 30 cells, 16-bit little-endian, as
fw/host/hw_host.c and sim/ write them) to a PNG with the real font,
and/or to text.

Usage: render.py CELLS [--png OUT.png] [--scale 2] [--text]
"""
import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'build'))
from codepage import CP, GLYPHS

PAL = [(0x00,0x00,0x00),(0x00,0x00,0xAA),(0x00,0xAA,0x00),(0x00,0xAA,0xAA),
       (0xAA,0x00,0x00),(0xAA,0x00,0xAA),(0xAA,0x55,0x00),(0xAA,0xAA,0xAA),
       (0x55,0x55,0x55),(0x55,0x55,0xFF),(0x55,0xFF,0x55),(0x55,0xFF,0xFF),
       (0xFF,0x55,0x55),(0xFF,0x55,0xFF),(0xFF,0xFF,0x55),(0xFF,0xFF,0xFF)]

def load(path):
    d = open(path, 'rb').read()
    return [d[i] | d[i + 1] << 8 for i in range(0, 2 * 80 * 30, 2)]

def text(cells):
    out = []
    for y in range(30):
        row = ''
        for x in range(80):
            c = cells[y * 80 + x] & 0xff
            row += CP.get(c, chr(c) if 0x20 <= c < 0x7f else ' ')
        out.append(row.rstrip())
    return '\n'.join(out)

def png(cells, out, scale=1):
    from PIL import Image
    img = Image.new('RGB', (640, 480))
    px = img.load()
    for y in range(30):
        for x in range(80):
            v = cells[y * 80 + x]
            ch, fg, bg = v & 0xff, (v >> 8) & 15, (v >> 12) & 15
            g = GLYPHS[ch]
            for r in range(16):
                bits = g[r]
                for c in range(8):
                    px[x * 8 + c, y * 16 + r] = PAL[fg] if bits >> (7 - c) & 1 else PAL[bg]
    if scale != 1:
        img = img.resize((640 * scale, 480 * scale), Image.NEAREST)
    img.save(out)

def main():
    cells = load(sys.argv[1])
    scale = int(sys.argv[sys.argv.index('--scale') + 1]) if '--scale' in sys.argv else 1
    if '--png' in sys.argv:
        png(cells, sys.argv[sys.argv.index('--png') + 1], scale)
    if '--text' in sys.argv:
        print(text(cells))

if __name__ == '__main__':
    main()
