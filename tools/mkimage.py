#!/usr/bin/env python3
"""
Build a whole-flash image (docs/flash-layout.md):

  0x000000  the machboot boot slot      --boot  machboot-<board>.bin
  0x060000  the system's gateware       --system some.bit   (optional)
  0x100000  the Zeitlos region          --region region.bin (optional)

Usage: mkimage.py OUT.bin --boot B [--system S] [--region R] [--size 2M]
"""
import sys

def arg(name, default=None):
    return sys.argv[sys.argv.index(name) + 1] if name in sys.argv else default

def main():
    out = sys.argv[1]
    size = arg('--size', '2M')
    size = int(size[:-1]) << 20 if size.endswith('M') else int(size, 0)
    img = bytearray(b'\xff' * size)
    for opt, at, limit in [('--boot', 0x000000, 0x060000), ('--system', 0x060000, 0x100000),
                           ('--region', 0x100000, size)]:
        p = arg(opt)
        if not p:
            continue
        d = open(p, 'rb').read()
        if at + len(d) > limit:
            sys.exit('mkimage: %s (%d bytes) does not fit at 0x%06x' % (p, len(d), at))
        img[at:at + len(d)] = d
    open(out, 'wb').write(img)
    print('%s: %d bytes' % (out, size))

if __name__ == '__main__':
    main()
