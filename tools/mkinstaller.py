#!/usr/bin/env python3
"""
Build the install-mode image: what `dfu-util -a 0` writes at 0x040000 on
a board that still has tinydfu (docs/install-mode.md).

  +0x000000  the installer's bitstream (the INSTALLER gateware: the flash
             fence protects the Zeitlos region instead of the boot slot;
             packed with boot address 0)
  +0x050000  the firmware image (the same as the boot slot's)
  +0x05E000  the installer header (fw/selfinst.h, si_header_t)
  +0x060000  the payload: the boot-slot image for 0x000000

The whole file must end before the Zeitlos region (0x100000 - 0x040000 =
768 KB), so that writing it cannot touch Zeitlos's kernel and apps.

Usage: mkinstaller.py installer.bit fw.img boot-slot.bin out.bin [--version 0x000300]
"""
import os, struct, sys, zlib

FW_OFFSET = 0x050000
HEADER = 0x05E000
PAYLOAD = 0x060000
LIMIT = 0x100000 - 0x040000
SETTINGS = 0x05E000          # the payload must leave the settings sectors empty

def idcode_of(bit):
    """VERIFY_ID's operand, as fw/flash.c finds it"""
    p = bit.find(b'\xbd\xb3')
    if p < 0 or p > 4096:
        sys.exit('mkinstaller: no ECP5 preamble in the payload')
    q = bit.find(b'\xe2\x00\x00\x00', p, p + 130)
    return struct.unpack('>I', bit[q + 4:q + 8])[0] if q >= 0 else 0

def build(inst_bit, fw, payload, version=0x000300):
    if len(inst_bit) > FW_OFFSET:
        sys.exit('mkinstaller: the installer bitstream is %d bytes, more than %d' % (len(inst_bit), FW_OFFSET))
    if len(payload) > SETTINGS:
        sys.exit('mkinstaller: the boot-slot image is %d bytes, more than %d' % (len(payload), SETTINGS))
    img = bytearray(b'\xff' * (PAYLOAD + len(payload)))
    if len(img) > LIMIT:
        sys.exit('mkinstaller: %d bytes would reach the Zeitlos region' % len(img))
    img[0:len(inst_bit)] = inst_bit
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import mkslot
    mkslot.place_fw(img, len(inst_bit), fw)	# as in the boot slot: below the header if large
    if len(img) != PAYLOAD + len(payload):
        sys.exit('mkinstaller: the firmware reaches the installer header')
    img[HEADER:HEADER + 32] = struct.pack('<4sIIIIIII', b'MBIN', 1, PAYLOAD, len(payload),
                                          zlib.crc32(payload) & 0xffffffff, idcode_of(payload), version, 0)
    img[PAYLOAD:] = payload
    return bytes(img)

def main():
    a = sys.argv[1:]
    version = 0x000300
    if '--version' in a:
        i = a.index('--version')
        version = int(a[i + 1], 0)
        del a[i:i + 2]
    inst_bit, fw, payload, out = a
    img = build(open(inst_bit, 'rb').read(), open(fw, 'rb').read(), open(payload, 'rb').read(), version)
    open(out, 'wb').write(img)
    print('%s: %d bytes (%.0f%% of the %d KB before the Zeitlos region)' % (
        out, len(img), 100.0 * len(img) / LIMIT, LIMIT // 1024))

if __name__ == '__main__':
    main()
