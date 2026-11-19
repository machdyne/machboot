#!/usr/bin/env python3
"""
machboot -- the RP2040's UF2 files (docs/power-on.md).

    mkuf2.py FIRMWARE.uf2 OUT.uf2                       the supervisor alone
    mkuf2.py FIRMWARE.uf2 OUT.uf2 --menu MENU.bit --board 0x202 --name "..."

FIRMWARE.uf2 is what pico-sdk builds (build/sv/machboot-sv.uf2). The
output adds, in the same file, either:

  - the menu: a 256-byte header ("MBMN", version 1, IDCODE, length,
    CRC-32, BOARD, name) at 0x10080000, the bitstream after it (from the
    dummy bytes before its preamble: the .bit's comment header is not
    configuration data); or
  - (no --menu) a blank page over that header, so a supervisor alone
    loads nothing at power-on even if an earlier UF2 left a menu there.

The RP2040's boot ROM writes each 256-byte block where it says, but it
erases by block NUMBER: the first block it gets of each run of 16
(block_no / 16) erases the 4 KB sector that block lands in
(pico-bootrom-rp2040, virtual_disk.c, _write_uf2_page). So the blocks
here go out sector by sector -- 16 per 4 KB sector, every page present
(0xFF where nothing is), each sector's at block numbers 16k..16k+15 --
and check_layout() runs that rule on the result before writing it.
(Numbered straight on from the firmware's, the menu's sectors were
erased again after their first pages: an earlier mkuf2 left no menu.)
It never touches the settings (0x10100000) or the system cache (0x10110000 on).
The IDCODE is read from the bitstream's VERIFY_ID command, so a menu
built for one die is never stored as another's.
"""
import argparse, struct, sys, zlib

UF2_MAGIC0, UF2_MAGIC1, UF2_MAGIC_END = 0x0A324655, 0x9E5D5157, 0x0AB16F30
FLAG_FAMILY = 0x00002000
RP2040_FAMILY = 0xE48BFF56
MENU_ADDR = 0x10080000
MENU_HDR = 256
MENU_MAX = 0x10100000 - MENU_ADDR


def read_uf2(path):
    data = open(path, 'rb').read()
    blocks = []
    for i in range(0, len(data), 512):
        b = data[i:i + 512]
        m0, m1, flags, addr, size, no, num, fam = struct.unpack('<8I', b[:32])
        if m0 != UF2_MAGIC0 or m1 != UF2_MAGIC1 or struct.unpack('<I', b[508:512])[0] != UF2_MAGIC_END:
            sys.exit('%s: not a UF2 file' % path)
        if fam != RP2040_FAMILY or size != 256:
            sys.exit('%s: not an RP2040 UF2 with 256-byte blocks' % path)
        blocks.append((addr, b[32:32 + 256]))
    return blocks


def by_sector(blocks):
    """the same contents as whole 4 KB sectors, 16 blocks each, in address order"""
    pages = {}
    for addr, payload in blocks:
        pages[addr] = payload           # a later block for the same page wins
    out = []
    for sector in sorted({a & ~0xfff for a in pages}):
        for p in range(16):
            out.append((sector + p * 256, pages.get(sector + p * 256, b'\xff' * 256)))
    return out


def check_layout(blocks, want):
    """the boot ROM's rule, run on the blocks: what flash holds afterwards must be want"""
    flash, cleared = {}, set()
    for no, (addr, payload) in enumerate(blocks):
        group = no * 256 // 4096
        if group not in cleared:
            cleared.add(group)
            sector = addr & ~0xfff
            for p in range(16):
                flash[sector + p * 256] = b'\xff' * 256
        old = flash.get(addr, b'\xff' * 256)     # a page never erased: whatever was there, ANDed
        flash[addr] = bytes(x & y for x, y in zip(old, payload))
    for addr, payload in want.items():
        if flash.get(addr) != payload:
            sys.exit('UF2 layout: the page at 0x%08x would not hold what it should' % addr)


def write_uf2(path, blocks):
    out = bytearray()
    for n, (addr, payload) in enumerate(blocks):
        hdr = struct.pack('<8I', UF2_MAGIC0, UF2_MAGIC1, FLAG_FAMILY, addr, 256, n, len(blocks), RP2040_FAMILY)
        out += hdr + payload.ljust(476, b'\x00') + struct.pack('<I', UF2_MAGIC_END)
    open(path, 'wb').write(out)


def idcode_of(bit):
    """the IDCODE in the bitstream's VERIFY_ID (0xE2, three zero bytes, IDCODE big-endian)"""
    i = bit.find(b'\xe2\x00\x00\x00')
    if i < 0 or i + 8 > len(bit):
        sys.exit('no VERIFY_ID in the bitstream')
    return struct.unpack('>I', bit[i + 4:i + 8])[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('firmware')
    ap.add_argument('out')
    ap.add_argument('--menu')
    ap.add_argument('--board', type=lambda s: int(s, 0), default=0)
    ap.add_argument('--name', default='machboot')
    a = ap.parse_args()

    blocks = read_uf2(a.firmware)
    top = max(addr for addr, _ in blocks) + 256
    if top > MENU_ADDR:
        sys.exit('the firmware reaches 0x%08x, past the menu at 0x%08x' % (top, MENU_ADDR))

    if a.menu:
        bit = open(a.menu, 'rb').read()
        # from the dummy bytes before the preamble, as openFPGALoader sends a
        # .bit over JTAG: the comment header ("Part: ...") stays behind
        i = bit.find(b'\xbd\xb3')
        if i < 3 or bit[i - 3:i] != b'\xff\xff\xff':
            sys.exit('%s: no preamble (FF FF FF BD B3)' % a.menu)
        bit = bit[i - 3:]
        if len(bit) > MENU_MAX - MENU_HDR:
            sys.exit('the menu is %d bytes; there is room for %d' % (len(bit), MENU_MAX - MENU_HDR))
        idc = idcode_of(bit)
        name = a.name.encode()[:39]
        hdr = struct.pack('<4s5I40s', b'MBMN', 1, idc, len(bit), zlib.crc32(bit) & 0xffffffff, a.board, name)
        image = hdr.ljust(MENU_HDR, b'\xff') + bit
        image += b'\xff' * (-len(image) % 256)
        for off in range(0, len(image), 256):
            blocks.append((MENU_ADDR + off, image[off:off + 256]))
        print('%s: firmware %d KB, menu %d bytes for IDCODE 0x%08x (%s) at 0x%08x' %
              (a.out, (top - 0x10000000) // 1024, len(bit), idc, a.name, MENU_ADDR))
    else:
        blocks.append((MENU_ADDR, b'\xff' * 256))
        print('%s: firmware %d KB, no menu (its header cleared)' % (a.out, (top - 0x10000000) // 1024))
    want = {addr: payload for addr, payload in blocks}
    blocks = by_sector(blocks)
    check_layout(blocks, want)
    write_uf2(a.out, blocks)


if __name__ == '__main__':
    main()
