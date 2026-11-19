#!/usr/bin/env python3
"""
Assemble a boot-slot image: the menu's bitstream at 0, the firmware
image at FW_OFFSET (0x050000). docs/flash-layout.md.

The result is what goes at flash 0x000000 (installed), and it is also
what tinydfu's user alt can write at 0x040000 (install mode): the boot
ROM looks for the firmware at both offsets.

Usage: mkslot.py menu.bit fw.img out.bin
"""
import sys

FW_OFFSET = 0x050000
SLOT_SIZE = 0x060000
SETTINGS = 0x05E000          # the last 8 KB: settings (kvlog, docs/settings.md)

import struct

def place_fw(img, bit_len, f):
    """Put the firmware image f (header + payload + overlays, tools/mkfw.py)
    into img, a boot slot whose bitstream is bit_len bytes. The header
    always goes at FW_OFFSET, where the boot ROM looks. The payload and
    the overlays follow it if they fit below the settings. Otherwise the
    payload goes below the header, in the room the bitstream leaves, 4 KB
    aligned, and the header's word 7 says where (the boot ROM reads it;
    the CRC covers only the payload); the overlays then follow the header
    (fw/ovl.c looks there when word 7 is set).
    Returns the payload's offset in img."""
    hdr = bytearray(f[:32])
    plen = struct.unpack_from('<I', hdr, 8)[0]
    payload, ovl = f[32:32 + plen], f[32 + plen:]
    if FW_OFFSET + 32 + len(payload) + len(ovl) <= SETTINGS:
        src = FW_OFFSET + 32
        struct.pack_into('<i', hdr, 28, 0)
        ovl_at = src + len(payload)
    else:
        src = (FW_OFFSET - len(payload)) & ~0xfff
        if src < bit_len:
            sys.exit('mkslot: the bitstream (%d bytes) and the firmware (%d bytes) do not fit '
                     'below 0x%06x' % (bit_len, len(payload), FW_OFFSET))
        if FW_OFFSET + 32 + len(ovl) > SETTINGS:
            sys.exit('mkslot: the overlays (%d bytes) do not fit between the header and the settings' % len(ovl))
        struct.pack_into('<i', hdr, 28, src - FW_OFFSET)
        ovl_at = FW_OFFSET + 32
    end = max(FW_OFFSET + 32, src + len(payload), ovl_at + len(ovl))
    if len(img) < end:
        img.extend(b'\xff' * (end - len(img)))
    img[FW_OFFSET:FW_OFFSET + 32] = hdr
    img[src:src + len(payload)] = payload
    img[ovl_at:ovl_at + len(ovl)] = ovl
    return src

def main():
    bit, fw, out = sys.argv[1:4]
    b = open(bit, 'rb').read()
    f = open(fw, 'rb').read()
    if len(b) > FW_OFFSET:
        sys.exit('mkslot: bitstream is %d bytes, more than the %d before the firmware' % (len(b), FW_OFFSET))
    img = bytearray(b'\xff' * len(b))
    img[0:len(b)] = b
    src = place_fw(img, len(b), f)
    open(out, 'wb').write(img)
    print('%s: bitstream %d bytes (%.0f%% of %d KB), firmware %d bytes at 0x%06x, image %d bytes' % (
        out, len(b), 100.0 * len(b) / FW_OFFSET, FW_OFFSET // 1024, len(f) - 32, src, len(img)))

if __name__ == '__main__':
    main()
