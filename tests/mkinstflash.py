#!/usr/bin/env python3
"""
A flash image for simulating install mode: a stand-in tinydfu at 0, the
installer image (tools/mkinstaller.py) at 0x040000, a Zeitlos region
pattern at 0x100000. The installer's own bitstream is a stand-in too:
the simulation runs the installer's gateware directly, so only its
firmware and payload are read from flash.

Usage: mkinstflash.py out.bin fw.img boot-slot.bin
"""
import sys, random
sys.path.insert(0, 'tools'); sys.path.insert(0, 'tests')
import mkinstaller, run_host

out, fw, payload = sys.argv[1:4]
r = random.Random(5)
inst = mkinstaller.build(run_host.bitfile(0x41111043, 231000, 12), open(fw, 'rb').read(), open(payload, 'rb').read())
f = bytearray(b'\xff' * (2 << 20))
tinydfu = run_host.bitfile(0x41111043, 125000, 13)
f[:len(tinydfu)] = tinydfu
f[0x040000:0x040000 + len(inst)] = inst
f[0x100000:0x110000] = bytes(r.getrandbits(8) for _ in range(65536))
open(out, 'wb').write(f)
print('%s: installer image %d bytes at 0x040000' % (out, len(inst)))
