#!/usr/bin/env python3
"""
What the menu costs. Reads the place-and-route logs and the
bitstreams of the 25F and 45F builds and prints a table of their size
and timing (docs/building.md has the current numbers).

Usage: measure.py BUILDDIR BOARD
"""
import os, re, sys

def util(log, what):
    m = re.findall(r'%s:\s+(\d+)/\s*(\d+)' % what, open(log).read())
    return '%s/%s' % m[-1] if m else '-'

def fmax(log):
    out = {}
    for name, f in re.findall(r"Max frequency for clock\s+'([^']+)': ([\d.]+) MHz", open(log).read()):
        out[name] = f
    return out

def main():
    b, board = sys.argv[1:3]
    rows = [('25F', '%s/%s-pnr.log' % (b, board), '%s/%s.bit' % (b, board), 99 * 1024),
            ('45F', '%s/%s-45f-pnr.log' % (b, board), '%s/%s-45f.bit' % (b, board), 162 * 1024)]
    print('| Die | LUT (COMB) | FF | DP16KD | bitstream | of which empty-die minimum | menu |')
    print('|---|---|---|---|---|---|---|')
    for die, log, bit, empty in rows:
        if not os.path.exists(log) or not os.path.exists(bit):
            continue
        size = os.path.getsize(bit)
        print('| %s | %s | %s | %s | %d bytes (%d KB) | ~%d KB | ~%d KB |' % (
            die, util(log, 'TRELLIS_COMB'), util(log, 'TRELLIS_FF'), util(log, 'DP16KD'),
            size, size // 1024, empty // 1024, (size - empty) // 1024))
    for die, log, bit, empty in rows:
        if os.path.exists(log):
            print('\n%s fmax: %s' % (die, ', '.join('%s %s MHz' % (k, v) for k, v in fmax(log).items())))

if __name__ == '__main__':
    main()
