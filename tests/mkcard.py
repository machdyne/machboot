#!/usr/bin/env python3
"""
Build SD card images for the tests, with pyfatfs -- an independent FAT
implementation, so fw/fat.c is checked against someone else's idea of
FAT, not its own.

  mkcard.py OUT.img KIND [system files...]

KIND:
  fat32-mbr   MBR; partition 1 FAT32 "ZEITLOS" with \\SYS\\GATEWARE; partition 2 FAT16 "DOS"
  fat16-mbr   MBR; partition 1 FAT16 "DOS" (no gateware), partition 2 FAT16 with \\SYS\\GATEWARE
  fat12-flat  no partition table: one FAT12 volume with \\SYS\\GATEWARE
  empty-mbr   MBR; one FAT32 partition without \\SYS\\GATEWARE

The gateware tree is given as arguments: dest=src pairs, for example
  SYS/GATEWARE/ZEITLOS/LAKRITZ.BIT=build/test/z.bit  SYS/GATEWARE/ZEITLOS/SYSTEM.CFG=@name=Zeitlos;version=0.0.8
("@" means literal text, ";" a newline).
"""
import os, struct, sys, tempfile
from pyfatfs.PyFat import PyFat
from pyfatfs.PyFatFS import PyFatFS

MB = 1 << 20

def make_fat(path, fat_type, size, label, files):
    with open(path, 'wb') as f:
        f.truncate(size)
    pf = PyFat()
    pf.mkfs(path, fat_type=fat_type, size=size, label=label)
    pf.close()
    fs = PyFatFS(path)
    for dest, data in files:
        d = os.path.dirname(dest)
        if d:
            fs.makedirs('/' + d, recreate=True)
        fs.writebytes('/' + dest, data)
    fs.close()

def mbr(parts):
    """parts: [(type, lba, sectors)]"""
    b = bytearray(512)
    for i, (t, lba, n) in enumerate(parts):
        e = 446 + 16 * i
        b[e + 4] = t
        b[e + 8:e + 12] = struct.pack('<I', lba)
        b[e + 12:e + 16] = struct.pack('<I', n)
    b[510] = 0x55
    b[511] = 0xAA
    return bytes(b)

def parse_files(args):
    out = []
    for a in args:
        dest, src = a.split('=', 1)
        data = src[1:].replace(';', '\n').encode() if src.startswith('@') else open(src, 'rb').read()
        out.append((dest, data))
    return out

def dos_files():
    return [('AUTOEXEC.BAT', b'@echo off\r\n'), ('GAMES/A Long Game Name.EXE', b'MZ' + bytes(300)),
            ('ISOS/readme.txt', b'disc images go here\r\n')]

def main():
    out, kind = sys.argv[1], sys.argv[2]
    files = parse_files(sys.argv[3:])
    tmp = tempfile.mkdtemp()
    p1, p2 = os.path.join(tmp, 'p1'), os.path.join(tmp, 'p2')
    if kind == 'fat32-mbr':
        s1, s2 = 40 * MB, 8 * MB
        make_fat(p1, PyFat.FAT_TYPE_FAT32, s1, 'ZEITLOS', dos_files()[:1] + files)
        make_fat(p2, PyFat.FAT_TYPE_FAT16, s2, 'DOS', dos_files())
        img = mbr([(0x0C, 2048, s1 // 512), (0x06, 2048 + s1 // 512, s2 // 512)])
        img += bytes(2048 * 512 - 512) + open(p1, 'rb').read() + open(p2, 'rb').read()
    elif kind == 'fat16-mbr':
        s1, s2 = 8 * MB, 16 * MB
        make_fat(p1, PyFat.FAT_TYPE_FAT16, s1, 'DOS', dos_files())
        make_fat(p2, PyFat.FAT_TYPE_FAT16, s2, 'ZEITLOS', files)
        img = mbr([(0x06, 63, s1 // 512), (0x0E, 63 + s1 // 512, s2 // 512)])
        img += bytes(63 * 512 - 512) + open(p1, 'rb').read() + open(p2, 'rb').read()
    elif kind == 'fat12-flat':
        s1 = 2 * MB
        make_fat(p1, PyFat.FAT_TYPE_FAT12, s1, 'SMALL', files)
        img = open(p1, 'rb').read()
    elif kind == 'empty-mbr':
        s1 = 40 * MB
        make_fat(p1, PyFat.FAT_TYPE_FAT32, s1, 'NOTHING', dos_files())
        img = mbr([(0x0C, 2048, s1 // 512)]) + bytes(2048 * 512 - 512) + open(p1, 'rb').read()
    else:
        sys.exit('unknown kind ' + kind)
    # round up to a whole MB, as a card would be
    img += bytes((-len(img)) % MB)
    open(out, 'wb').write(img)
    print('%s: %s, %d MB' % (out, kind, len(img) // MB))

if __name__ == '__main__':
    main()
