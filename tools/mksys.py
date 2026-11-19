#!/usr/bin/env python3
"""
Put a system on an SD card the way machboot looks for it
(docs/sdcard.md):

  <card>/SYS/GATEWARE/<NAME>/SYSTEM.CFG     name=..., version=...
  <card>/SYS/GATEWARE/<NAME>/<BOARD>.BIT    the bitstream

<card> is where the card is mounted (or any folder, to copy later).
The folder name is the system name, upper case, cut to 8 characters,
so DOS sees it too.

Usage:
  mksys.py CARD BITSTREAM --name Zeitlos --version 0.0.8 [--board lakritz]

Example:
  python3 tools/mksys.py /media/$USER/SDCARD zeitlos-lakritz_gpio-gateware.bit --name Zeitlos --version 0.0.8
"""
import argparse, os, re, shutil, sys

BOARDS = {'lakritz': 'LAKRITZ', 'sergei2': 'SERGEI2', 'sergei_ml2': 'SERGEI2', 'mozart2': 'MOZART2',
          'mozart_ml2': 'MOZART2', 'obst': 'OBST', 'konfekt': 'KONFEKT', 'minze': 'MINZE',
          'schoko': 'SCHOKO', 'noir': 'NOIR', 'klinge': 'KLINGE'}

def looks_like_bitstream(b):
    """the check machboot makes (fw/flash.c): the preamble in the first 4 KB"""
    return b.find(b'\xbd\xb3', 0, 4096) >= 0

def main():
    ap = argparse.ArgumentParser(description='put a system on an SD card for machboot')
    ap.add_argument('card')
    ap.add_argument('bitstream')
    ap.add_argument('--name', required=True)
    ap.add_argument('--version', default='')
    ap.add_argument('--board', default='lakritz', choices=sorted(BOARDS))
    a = ap.parse_args()

    data = open(a.bitstream, 'rb').read()
    if not looks_like_bitstream(data):
        sys.exit('mksys: %s does not start like an ECP5 bitstream' % a.bitstream)
    folder = re.sub(r'[^A-Z0-9_]', '', a.name.upper())[:8]
    if not folder:
        sys.exit('mksys: the name needs a letter or a digit')
    d = os.path.join(a.card, 'SYS', 'GATEWARE', folder)
    os.makedirs(d, exist_ok=True)
    bit = os.path.join(d, BOARDS[a.board] + '.BIT')
    shutil.copyfile(a.bitstream, bit)
    with open(os.path.join(d, 'SYSTEM.CFG'), 'w', newline='\r\n') as f:
        f.write('# written by machboot tools/mksys.py\n')
        f.write('name=%s\n' % a.name)
        if a.version:
            f.write('version=%s\n' % a.version)
    print('%s: %s %s, %d bytes as %s' % (d, a.name, a.version, len(data), os.path.basename(bit)))

if __name__ == '__main__':
    main()
