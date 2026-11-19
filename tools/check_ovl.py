#!/usr/bin/env python3
"""
Check the overlay build (docs/overlays.md): resident code may call into
the overlay window only from the functions that first make sure the page
is there (ovl_use()): the menu's redraw, its page keys and its key loop.
Anything else calling into the window -- a function of an overlay's file
called by resident code, as selfinst_probe() once was -- would run
whatever page happens to be loaded.

Usage: check_ovl.py fw.elf objdump
"""
import re, subprocess, sys

ALLOWED = ('redraw', 'page_key', 'ui_menu')

def main():
    elf, objdump = sys.argv[1], sys.argv[2]
    nm = subprocess.run([objdump.replace('objdump', 'nm'), elf], capture_output=True, text=True, check=True).stdout
    sym = {l.split()[2]: int(l.split()[0], 16) for l in nm.splitlines() if len(l.split()) == 3}
    lo, hi = sym['__ovl_start'], sym['__ovl_start'] + sym['__ovl_size']
    dis = subprocess.run([objdump, '-d', '-j', '.text', elf], capture_output=True, text=True, check=True).stdout
    func, bad, ok = None, [], 0
    for line in dis.splitlines():
        m = re.match(r'^[0-9a-f]+ <([^>]+)>:', line)
        if m:
            func = m.group(1)
            continue
        m = re.search(r'\b(jal|j)\s+(?:\w+,)?([0-9a-f]+) <', line)
        if m and lo <= int(m.group(2), 16) < hi:
            base = func.split('.')[0]
            if base in ALLOWED:
                ok += 1
            else:
                bad.append('%s calls 0x%s, in the overlay window' % (func, m.group(2)))
    for b in bad:
        print('check_ovl: ' + b)
    if bad:
        sys.exit(1)
    print('check_ovl: %d calls into the overlay window, all from %s' % (ok, ', '.join(ALLOWED)))

if __name__ == '__main__':
    main()
