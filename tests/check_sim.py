#!/usr/bin/env python3
"""
Check the simulation's screens against the PC build's.

The simulation decodes its frames from the TMDS lanes (sim/sim_main.cc);
the PC build renders the same firmware's text cells with the same font
(tools/render.py). If the hardware path -- text RAM, font ROM, pipeline,
palette, TMDS encoder, serialiser -- is right, the pictures are the
same, pixel for pixel.

  sim_menu.ppm     vs  build/host/slot_start.png      (whole screen)
  sim_startup.ppm  vs  build/host/startup.png    (all but the countdown rows)
  sim_sram_menu.ppm vs build/host/no_system.png  (the JTAG test build, blank flash)
"""
import sys
from PIL import Image

def load(p):
    return Image.open(p).convert('RGB')

def diff(a, b, skip_rows=()):
    pa, pb = a.load(), b.load()
    n = 0
    for y in range(480):
        if y // 16 in skip_rows:
            continue
        for x in range(640):
            if pa[x, y] != pb[x, y]:
                n += 1
    return n

def main():
    ok = True
    for sim, host, skip in [('build/sim/sim_menu.ppm', 'build/host/slot_start.png', ()),
                            ('build/sim/sim_startup.ppm', 'build/host/startup.png', (14, 16)),
                            ('build/sim/sim_sram_menu.ppm', 'build/host/no_system.png', ())]:
        try:
            a = load(sim)
        except FileNotFoundError:
            print('FAIL %s missing' % sim); ok = False
            continue
        a.save(sim.replace('.ppm', '.png'))
        try:
            b = load(host)
        except FileNotFoundError:
            print('skip %s: run make host first' % host)
            continue
        n = diff(a, b, skip)
        print('%s %s vs %s: %d pixels differ' % ('ok  ' if n == 0 else 'FAIL', sim, host, n))
        ok &= n == 0
    sys.exit(0 if ok else 1)

if __name__ == '__main__':
    main()
