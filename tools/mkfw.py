#!/usr/bin/env python3
"""
Wrap the firmware binary in the 32-byte header the boot ROM checks.

  0  magic "MBFW"           16  entry address
  4  register-map version   20  CRC-32 of the payload
  8  payload length         24  version (major << 16 | minor << 8 | patch)
 12  load address           28  where the payload is (tools/mkslot.py sets it)

With --ovl, the overlays (the menu's pages on Lakritz and Konfekt,
docs/overlays.md) follow the payload, after a table fw/ovl.c reads:

  "MBOV", count, the payload's CRC-32, 0,
  per overlay: offset (from "MBOV"), length, CRC-32, 0

The boot ROM loads only the payload; the CRC ties the overlays to it.

Usage: mkfw.py fw.bin fw.img [--load 0x400] [--version 0x000100] [--ovl a.bin b.bin ...]
"""
import struct, sys, zlib

def pad4(b):
    return b + b'\0' * (-len(b) % 4)

def ovl_blob(files, fw_crc):
    parts = [pad4(open(f, 'rb').read()) for f in files]
    off = 16 + 16 * len(parts)
    table = struct.pack('<4sIII', b'MBOV', len(parts), fw_crc, 0)
    data = b''
    for p in parts:
        table += struct.pack('<IIII', off + len(data), len(p), zlib.crc32(p) & 0xffffffff, 0)
        data += p
    return table + data

def main():
    a = sys.argv[1:]
    ovl = []
    if '--ovl' in a:
        i = a.index('--ovl')
        ovl = a[i + 1:]
        del a[i:]
    src, dst = a[0], a[1]
    load = 0x400
    version = 0x000100
    if '--load' in a:
        load = int(a[a.index('--load') + 1], 0)
    if '--version' in a:
        version = int(a[a.index('--version') + 1], 0)
    payload = pad4(open(src, 'rb').read())
    crc = zlib.crc32(payload) & 0xffffffff
    hdr = struct.pack('<4sIIIIIII', b'MBFW', 2, len(payload), load, load, crc, version, 0)
    tail = ovl_blob(ovl, crc) if ovl else b''
    open(dst, 'wb').write(hdr + payload + tail)
    print('%s: %d bytes of firmware, CRC %08x%s' % (dst, len(payload), crc,
          ', %d overlays, %d bytes (largest %d)' % (len(ovl), len(tail), max(
              len(open(f, 'rb').read()) for f in ovl)) if ovl else ''))

if __name__ == '__main__':
    main()
