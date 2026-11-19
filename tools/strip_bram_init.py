#!/usr/bin/env python3
"""
Drop block-RAM initialisation that is all zeros from a nextpnr text
config, before ecppack.

Every initialised DP16KD costs about 2.3 KB of bitstream that ecppack
does not compress. Memories whose contents do
not matter at power-on (the RAM, the text cells) are written as zeros
by nextpnr all the same; leaving them out of the bitstream leaves their
contents undefined after configuration, which the boot ROM and the
firmware already allow for (they clear what they use).

Usage: strip_bram_init.py IN.config OUT.config
"""
import re, sys

def main():
    src, dst = sys.argv[1:3]
    parts = re.split(r'(?m)^(?=\.)', open(src).read())
    out, dropped = [], 0
    for p in parts:
        if p.startswith('.bram_init') and all(int(v, 16) == 0 for v in p.split()[2:]):
            dropped += 1
            continue
        out.append(p)
    open(dst, 'w').write(''.join(out))
    print('%s: %d all-zero block RAM inits left out' % (dst, dropped))

if __name__ == '__main__':
    main()
