#!/usr/bin/env python3
"""
Drive the PC build of the firmware (build/host/mbhost) through the
menu, save every screen as PNG and text, and check what they say.

Flash images are synthetic: a boot slot with machboot's firmware image,
and a minimal ECP5 bitstream start (header, preamble, VERIFY_ID) in the
system slot -- exactly the bytes fw/flash.c looks at. If a real build
exists (build/lakritz.bit), one case uses it instead.

Run by `make host`. Output: build/host/*.png, build/host/*.txt.
"""
import os, subprocess, sys, struct
sys.path.insert(0, 'tools')
import render

B = 'build/host'
MB = 2 << 20

def bitstream(idcode, header=True):
    b = b''
    if header:
        b += b'\xff\x00' + b'Part: LFE5U-25F-6CABGA256\x00' + b'Date: 2026\x00' + b'\xff'
    b += b'\xff' * 16 + b'\xbd\xb3'
    b += bytes([0x3B, 0, 0, 0])                       # LSC_RESET_CRC
    b += bytes([0xE2, 0, 0, 0]) + struct.pack('>I', idcode)  # VERIFY_ID
    return b + b'\xff' * 64

def flash(sys_bit=None, zeitlos=True, size=MB, at=0x060000, fw_at=0x050000):
    f = bytearray(b'\xff' * size)
    img = open('build/fw.img', 'rb').read()
    f[fw_at:fw_at + len(img)] = img
    if sys_bit:
        f[at:at + len(sys_bit)] = sys_bit
    if zeitlos:
        f[0x100000:0x100004] = b'\x13\x00\x00\x00'
    return bytes(f)

def run(name, image, script, expect=(), absent=(), card=None, exe='mbhost'):
    fp = os.path.join(B, name + '.flash')
    sp = os.path.join(B, name + '.script')
    open(fp, 'wb').write(image)
    open(sp, 'w').write(script)
    if os.path.exists(fp + '.out'):
        os.remove(fp + '.out')
    cmd = [os.path.join(B, exe), fp, sp, B] + ([card] if card else [])
    out = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
    log = out.stdout
    open(os.path.join(B, name + '.serial.txt'), 'w').write(log)
    ok = out.returncode == 0
    texts = {}
    for line in out.stderr.splitlines():
        if line.startswith('[snap '):
            snap = line[6:-1]
            cells = render.load(os.path.join(B, snap + '.cells'))
            t = render.text(cells)
            texts[snap] = t
            open(os.path.join(B, snap + '.txt'), 'w').write(t + '\n')
            render.png(cells, os.path.join(B, snap + '.png'), 1)
    alltext = '\n'.join(texts.values()) + '\n' + log + '\n' + out.stderr
    for e in expect:
        if e not in alltext:
            print('  FAIL %s: missing %r' % (name, e)); ok = False
    for a in absent:
        if a in alltext:
            print('  FAIL %s: unexpected %r' % (name, a)); ok = False
    print('%s %s (%s)' % ('ok  ' if ok else 'FAIL', name, ', '.join(texts) or 'no screens'))
    if not ok:
        print(out.stderr[-2000:])
    return ok

def flash_out(name):
    p = os.path.join(B, name + '.flash.out')
    return open(p, 'rb').read() if os.path.exists(p) else None

def check(cond, what):
    print('%s %s' % ('ok  ' if cond else 'FAIL', what))
    return cond

T = 'build/test'

def bitfile(idcode, size, seed):
    import random
    r = random.Random(seed)
    b = bitstream(idcode)[:-64]
    return b + bytes(r.getrandbits(8) for _ in range(size - len(b)))

def cards():
    """test cards, built with pyfatfs (tests/mkcard.py)"""
    os.makedirs(T, exist_ok=True)
    files = {'z008': bitfile(0x41111043, 470000, 1), 'z009': bitfile(0x41111043, 471234, 4),
             'm86': bitfile(0x41111043, 460848, 2), 's45': bitfile(0x41112043, 600000, 3),
             'k45': bitfile(0x41112043, 700000, 5)}
    for k, v in files.items():
        open('%s/%s.bit' % (T, k), 'wb').write(v)
    def mk(name, kind, *args):
        path = '%s/%s.img' % (T, name)
        subprocess.run([sys.executable, 'tests/mkcard.py', path, kind] + list(args), check=True,
                       capture_output=True)
        return path
    z = lambda ver, bit: ['SYS/GATEWARE/ZEITLOS/LAKRITZ.BIT=%s/%s.bit' % (T, bit),
                          'SYS/GATEWARE/ZEITLOS/SYSTEM.CFG=@name=Zeitlos;version=%s' % ver]
    m = ['SYS/GATEWARE/MACH86/LAKRITZ.BIT=%s/m86.bit' % T,
         'SYS/GATEWARE/MACH86/SYSTEM.CFG=@# Mach86;name = Mach86;version= 0.1',
         'SYS/GATEWARE/MACH86/SERGEI2.BIT=%s/s45.bit' % T]
    k = ['SYS/GATEWARE/OTHER/SYSTEM.CFG=@name=Kakao;version=0.3', 'SYS/GATEWARE/OTHER/SERGEI2.BIT=%s/k45.bit' % T,
         'SYS/GATEWARE/BLINK.BIT=%s/s45.bit' % T]
    return {
        'c008': mk('c008', 'fat32-mbr', *(z('0.0.8', 'z008') + m + k)),
        'c009': mk('c009', 'fat32-mbr', *(z('0.0.9', 'z009') + m)),
        'c45': mk('c45', 'fat32-mbr', 'SYS/GATEWARE/ZEITLOS/LAKRITZ.BIT=%s/s45.bit' % T,
                  'SYS/GATEWARE/ZEITLOS/SYSTEM.CFG=@name=Zeitlos;version=0.0.8'),
        'c16': mk('c16', 'fat16-mbr', *(z('0.0.8', 'z008'))),
        'c12': mk('c12', 'fat12-flat', 'SYS/GATEWARE/MACH86/SYSTEM.CFG=@name=Mach86;version=0.1',
                  'SYS/GATEWARE/MACH86/LAKRITZ.BIT=%s/s45.bit' % T),
        'cnone': mk('cnone', 'empty-mbr'),
    }

# ---- install mode ----

def boot_slot_image(bit):
    """what tools/mkslot.py builds: the menu's bitstream, the firmware at 0x050000"""
    sys.path.insert(0, 'tools')
    import mkslot
    img = bytearray(bit)
    mkslot.place_fw(img, len(bit), open('build/fw.img', 'rb').read())
    return bytes(img)

def install_mode_cases(c, z008):
    import random
    sys.path.insert(0, 'tools')
    import mkinstaller
    ok = True
    r = random.Random(7)
    fw = open('build/fw.img', 'rb').read()
    menu_bit = bitfile(0x41111043, 230000, 11)              # stands in for build/lakritz.bit
    inst_bit = bitfile(0x41111043, 231000, 12)              # and for build/lakritz-inst.bit
    payload = boot_slot_image(menu_bit)
    inst = mkinstaller.build(inst_bit, fw, payload)
    tinydfu = bitfile(0x41111043, 125000, 13)
    region = bytes(r.getrandbits(8) for _ in range(65536))

    def board_with_tinydfu(installer_image):
        f = bytearray(b'\xff' * MB)
        f[:len(tinydfu)] = tinydfu
        f[0x040000:0x040000 + len(installer_image)] = installer_image
        f[0x100000:0x110000] = region
        return bytes(f)

    want = payload + b'\xff' * (0x05E000 - len(payload))     # the settings sectors are written after
    f0 = board_with_tinydfu(inst)
    IM = 'installer fwbase:090000 '

    ok &= run('im_offer', f0, IM + 'wait:300 snap:im_offer f1 snap:im_offer_details',
              ['running from where dfu-util put it', 'Install machboot', 'replaces tinydfu',
               'Then Zeitlos 0.0.8 is installed from the card.', 'Writes      0x000000-0x05FFFF'], card=c['c008'])
    ok &= run('im_cancel', f0, IM + 'wait:300 enter snap:im_confirm esc wait:50', ['Replace tinydfu with machboot now?'],
              card=c['c008'])
    ok &= check(flash_out('im_cancel') is None, 'install mode, cancelled: nothing was written')

    ok &= run('im_install', f0, IM + "wait:300 enter 'y wait:120000",
              ['machboot is installed.', 'Zeitlos 0.0.8 is installed too.', 'install mode: done, restarting'],
              card=c['c008'])
    fi = flash_out('im_install')
    ok &= check(fi is not None and fi[:0x05E000] == want, 'install mode: 0x000000-0x05DFFF is the boot-slot image, byte for byte')
    ok &= check(fi is not None and b'sys.name' in fi[0x05E000:0x060000] and b'Zeitlos' in fi[0x05E000:0x060000],
                'install mode: the new settings record Zeitlos')
    ok &= check(fi is not None and fi[0x060000:0x060000 + len(z008)] == z008, 'install mode: the system slot holds Zeitlos from the card')
    ok &= check(fi is not None and fi[0x100000:0x110000] == region and fi[0x110000:] == f0[0x110000:],
                'install mode: the Zeitlos region is unchanged')
    if fi:
        ok &= run('im_after', fi, 'wait:1200 snap:im_after', ['Starting Zeitlos in'], card=c['c008'])
        ok &= run('im_after_menu', fi, 'wait:300 f8 f1 wait:50 snap:im_after_menu',
                  ['Zeitlos 0.0.8', 'Installed   470000 bytes'], card=c['c008'])

    # no card: machboot alone; the system slot must not look like a system
    ok &= run('im_nocard', f0, IM + "wait:300 snap:im_offer_nocard enter 'y wait:120000",
              ['There is no system for this board on the card;', 'machboot is installed.'])
    fn = flash_out('im_nocard')
    ok &= check(fn is not None and fn[:0x05E000] == want and fn[0x060000:0x061000] == b'\xff' * 4096
                and fn[0x090000:0x091000] == b'\xff' * 4096,
                'install mode, no card: machboot installed; the installer\'s bitstream and firmware are invisible')
    if fn:
        ok &= run('im_nocard_after', fn, 'wait:300 snap:im_nocard_after', ['No system is installed yet.'])

    # a power cut early on: tinydfu is still whole; dfu-util again, install again
    ok &= run('im_cut_early', f0, IM + "wait:300 enter cut:40 'y wait:120000", [], card=c['c008'])
    fc = flash_out('im_cut_early')
    ok &= check(fc is not None and fc[:0x020000] == f0[:0x020000],
                'install mode, power cut early: tinydfu (0x000000-0x01FFFF) is untouched')
    if fc:
        again = bytearray(fc)
        again[0x040000:0x040000 + len(inst)] = inst          # what dfu-util -a 0 does
        ok &= run('im_cut_again', bytes(again), IM + "wait:300 enter 'y wait:120000", ['machboot is installed.'],
                  card=c['c008'])
        fa = flash_out('im_cut_again')
        ok &= check(fa is not None and fa[:0x05E000] == want and fa[0x060000:0x060000 + len(z008)] == z008,
                    'install mode, after the power cut and dfu-util again: installed, byte for byte')

    # a damaged image: nothing is written
    bad = bytearray(inst)
    bad[0x060000 + 100000] ^= 0x40
    ok &= run('im_damaged', board_with_tinydfu(bytes(bad)), IM + "wait:300 enter 'y wait:5000 snap:im_damaged",
              ['This machboot installer is damaged.', 'Nothing has been changed.'], card=c['c008'])
    ok &= check(flash_out('im_damaged') is None, 'install mode, damaged image: nothing was written')

    # the plain boot-slot image written by dfu-util: it cannot install itself
    ok &= run('im_plain', board_with_tinydfu(payload), 'fwbase:090000 wait:300 snap:im_plain',
              ['This image cannot install itself.'])
    # the installer image, but on a 45F: refused
    ok &= run('im_wrong_fpga', f0, IM + 'idcode:41112043 wait:300 snap:im_wrong_fpga',
              ['This machboot installer is damaged.'])
    return ok

# ---- the USB device (console, DFU) ----

def run_env(env, *a, **k):
    old = {key: os.environ.get(key) for key in env}
    os.environ.update(env)
    try:
        return run(*a, **k)
    finally:
        for key, v in old.items():
            if v is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = v

def usb_device_cases(c, z008, z25):
    import random
    ok = True
    SLOT = 0x060000
    U = 'usbd wait:300 ud:enum ud:wait '
    ok &= run('ud_enum', flash(z25), U + "f8 left 'b right wait:50 snap:ud_board",
              ['[ud: device 16d0:116d, class ef, 18 bytes]', 'if0/02.02.01', 'if1/0a.00.00', 'if2/fe.01.02',
               'dfu(xfer 4096)', 'string 2 "machboot"', 'string 4 "System slot + Zeitlos region"',
               'enumerated, console open', 'USB-C', 'console, DFU (terminal open)'])
    cdc = os.path.join(B, 'ud_console.cdc.txt')
    ok &= run_env({'MB_CDC_OUT': cdc}, 'ud_console', flash(z25), U + 'ud:cdc:^[19~ wait:600 ud:cdc:^[D wait:100 ud:cdc:b wait:100 ud:cdc:^[C wait:300',
                  ['== Board ==', 'console, DFU'])
    got = open(cdc, 'rb').read().decode('latin-1') if os.path.exists(cdc) else ''
    ok &= check('== Start a system ==' in got and '== Board ==' in got and 'Sections:' in got,
                'console: the menu is on the USB port too, and keys typed there drive it')

    ok &= run('ud_dfu', flash(z25), U + 'f8 ud:dfu:%s/z008.bit ud:wait wait:200 snap:ud_dfu_done f1 snap:ud_dfu_start' % T,
              ['[ud: dfu: done, state 2]', 'dfu-util: done. 458 KB into the system slot', 'Installed   470000 bytes'])
    fd = flash_out('ud_dfu')
    ok &= check(fd is not None and fd[SLOT:SLOT + len(z008)] == z008, 'dfu: the system slot holds the file, byte for byte')
    ok &= check(fd is not None and fd[:0x05E000] == flash(z25)[:0x05E000] and fd[0x100000:] == flash(z25)[0x100000:],
                'dfu: nothing outside the slot and the settings changed')
    if fd:
        up = os.path.join(B, 'ud_upload.bin')
        ok &= run('ud_upload', fd, U + 'f8 ud:upload:%s ud:wait wait:50' % up, ['[ud: upload: 161 blocks, done]'])
        u = open(up, 'rb').read() if os.path.exists(up) else b''
        ok &= check(len(u) == 0x0A0000 and u[:len(z008)] == z008, 'dfu upload: the system slot, 640 KB')

    # Zeitlos's -dfu.bin: tinydfu's layout, gateware at 0, the region at 0x0C0000
    r = random.Random(9)
    region = bytes(r.getrandbits(8) for _ in range(70000))
    zdfu = bytearray(b'\xff' * (0x0C0000 + len(region)))
    zdfu[:len(z008)] = z008
    zdfu[0x0C0000:] = region
    zp = os.path.join(T, 'zeitlos-dfu.bin')
    open(zp, 'wb').write(zdfu)
    ok &= run('ud_dfu_zeitlos', flash(z25), U + 'f8 ud:dfu:%s ud:wait wait:200 snap:ud_dfu_zeitlos' % zp,
              ['[ud: dfu: done, state 2]', 'KB Zeitlos region'])
    fz = flash_out('ud_dfu_zeitlos')
    ok &= check(fz is not None and fz[SLOT:SLOT + len(z008)] == z008 and fz[0x100000:0x100000 + len(region)] == region,
                "dfu, Zeitlos's -dfu.bin: the gateware in the slot, the rest at 0x100000")

    ok &= run('ud_dfu_wrong', flash(z25), U + 'f8 ud:dfu:%s/s45.bit ud:wait wait:200 snap:ud_dfu_wrong' % T,
              ['dfu-util: error', 'not written: for another FPGA'])
    ok &= check(flash_out('ud_dfu_wrong') is None, 'dfu, a 45F file: nothing written')
    ok &= run('ud_dfu_notbit', flash(z25), U + 'f8 ud:dfu:tests/run_host.py ud:wait wait:200',
              ['dfu-util: error', 'not a bitstream'])
    ok &= check(flash_out('ud_dfu_notbit') is None, 'dfu, not a bitstream: nothing written')
    ok &= run('ud_dfu_installmode', flash(None, at=0x040000, fw_at=0x090000),
              'fwbase:090000 ' + U + 'ud:dfu:%s/z008.bit ud:wait wait:200' % T,
              ['dfu-util: error', 'install machboot first'])
    ok &= check(flash_out('ud_dfu_installmode') is None, 'dfu in install mode: nothing written')
    # the card reader: the SD card as a USB drive
    cr = os.path.join(B, 'ud_card_read.bin')
    wr = os.path.join(T, 'ud_card_write.bin')
    open(wr, 'wb').write(bytes((i * 7 + 3) & 0xff for i in range(1024)))
    cimg = os.path.join(T, 'ud_card.img')
    import shutil
    shutil.copy(c['c008'], cimg)
    if os.path.exists(cimg + '.out'):
        os.remove(cimg + '.out')
    ok &= run('ud_card_off', flash(z25), U + 'f8 ud:msc:tur ud:msc:read:0:64:%s ud:msc:tur ud:wait wait:50' % (cr + '.off'),
              ['test unit ready: CSW, status 1', 'sense 02/3a/00', 'read: CSW, status 1, residue 0'], card=cimg)
    ok &= run('ud_card', flash(z25), U + "f8 left 'c right wait:50 snap:ud_card_page enter wait:100 "
              'ud:msc:tur ud:msc:tur ud:msc:cap ud:msc:read:0:4:%s ud:msc:write:2048:%s ud:wait wait:400 snap:ud_card_shared '
              'ud:msc:eject ud:wait wait:300 snap:ud_card_ejected' % (cr, wr),
              ['Share the card with the PC', 'sense 06/28/00', 'test unit ready: CSW, status 0',
               'sectors of 512 bytes', 'read: CSW, status 0, residue 0', 'write: CSW, status 0, residue 0',
               'The SD card is on the PC as a USB drive.', 'Read 0.0 MB, written 0.0 MB',
               'ejected on the PC'], card=cimg)
    card0 = open(c['c008'], 'rb').read()
    rd = open(cr, 'rb').read() if os.path.exists(cr) else b''
    ok &= check(rd == card0[:2048], 'card reader: the PC reads the card, byte for byte')
    co = open(cimg + '.out', 'rb').read() if os.path.exists(cimg + '.out') else b''
    ok &= check(co[2048 * 512:2048 * 512 + 1024] == open(wr, 'rb').read() and co[:2048 * 512] == card0[:2048 * 512]
                and co[2050 * 512:] == card0[2050 * 512:], 'card reader: the PC writes the card, there and nowhere else')
    # back from the PC with something to install: one Enter
    ok &= run('ud_card_offer', flash(None), U + "f8 left 'c right enter wait:100 ud:msc:eject ud:wait wait:300 "
              "snap:ud_card_offer enter wait:30000 snap:ud_card_offer_done",
              ['Install Zeitlos 0.0.8', 'new on the card', '► Zeitlos 0.0.8                            installed'], card=c['c008'])
    fo = flash_out('ud_card_offer')
    ok &= check(fo is not None and fo[SLOT:SLOT + len(z008)] == z008, 'card reader, then Install: the slot holds the card\'s file')
    ok &= run('ud_card_leave', flash(z25), U + "f8 left 'c right enter wait:100 left wait:100 ud:msc:tur ud:wait wait:50",
              ['the card is machboot\'s again', 'sense 02/3a/00'], card=cimg)

    ok &= run('ud_dfu_countdown', flash(z25), 'usbd ud:enum ud:dfu:%s/z008.bit ud:wait wait:300' % T,
              ['the menu instead', '[ud: dfu: done, state 2]'], ['Starting Zeitlos ...'])
    return ok

# ---- hubs and flash drives on the USB host ports ----

def usb_host_cases(c, z008, z25):
    ok = True
    SLOT = 0x060000
    stick = c['c008']                       # a FAT32 image with \SYS\GATEWARE: Zeitlos 0.0.8, Mach86, Kakao
    clean = ['[usb model:']                 # no collisions, no toggle errors
    ok &= run('hub_kbd', flash(z25), "usb:hub usb:hub:kbd@2 wait:2500 u:f8 wait:300 left 'b right wait:50 snap:hub_kbd",
              ['usb 1: hub 05e3:0608, 4 ports', 'usb 1.2: keyboard 04c4:2001 (low speed, behind a hub)',
               '== Start a system ==', 'USB 1        hub  05e3:0608', 'USB 1.2    keyboard  04c4:2001  low speed'], clean)
    ok &= run('hub_three', flash(z25), "f8 usb:hub usb:hub:mouse@1 usb:hub:kbd@3 usb:hub:stick@4:%s wait:4000 "
              "left 'b right wait:50 snap:hub_three" % stick,
              ['usb 1.1: 04c4:2001 is not a keyboard, hub or flash drive', 'usb 1.3: keyboard',
               'usb 1.4: flash drive 0781:5581', 'flash drive  0781:5581'], clean)
    ok &= run('hub_replug', flash(z25), "f8 usb:hub usb:hub:kbd@2 wait:2500 usb:hub:unplug@2 wait:500 usb:hub:kbd@2 "
              "wait:2500 u:left wait:100 u:b wait:100 u:right wait:300 snap:hub_replug",
              ['USB 1.2    keyboard  04c4:2001'], clean)
    # the hub goes: its keyboard goes with it; a keyboard plugged in directly then works
    ok &= run('hub_unplug', flash(z25), "f8 usb:hub usb:hub:kbd@2 wait:2500 usb:unplug wait:300 usb:kbd wait:1500 "
              "u:left wait:100 u:b wait:100 u:right wait:300 snap:hub_unplug",
              ['USB 1        keyboard  04c4:2001  low speed'], clean + ['USB 1.2'])
    # a flash drive on the root port, no SD card: Update lists what it has, and installs from it
    ok &= run('stick_list', flash(z25), "usb:stick:%s wait:3000 f8 f2 wait:200 snap:stick_list" % stick,
              ['usb 1: flash drive 0781:5581', 'On the USB drive:', 'Zeitlos 0.0.8  (USB)', 'Mach86'], clean)
    ok &= run('stick_install', flash(z25), "usb:stick:%s wait:3000 f8 f2 wait:200 enter wait:30000 snap:stick_install" % stick,
              ['Zeitlos 0.0.8  (USB)'], clean)
    fs = flash_out('stick_install')
    ok &= check(fs is not None and fs[SLOT:SLOT + len(z008)] == z008, 'from a USB flash drive: the slot holds its file, byte for byte')
    # the SD card and a drive at once: both lists
    ok &= run('stick_and_card', flash(z25), "usb:stick:%s wait:3000 f8 f2 wait:200 snap:stick_and_card" % stick,
              ['On the SD card and the USB drive:', 'Zeitlos 0.0.8  (USB)'], clean, card=c['c009'])
    return ok

# ---- the other boards: Sergei/Mozart ML0-ML2 (SRAM builds), Konfekt ----

def sechzig_flash(sys_bit):
    """a Sechzig module's flash: the system at 0, the Zeitlos region at 0x100000, nothing of machboot's"""
    f = bytearray(b'\xff' * MB)
    f[0:len(sys_bit)] = sys_bit
    f[0x100000:0x100004] = b'\x13\x00\x00\x00'
    return bytes(f)

def board_cases(c, z25):
    ok = True
    X = 'mbhost-sechzig'
    z45 = bitstream(0x41112043)
    S2 = 'sechzig board:0x0202 idcode:41112043 '
    fi = sechzig_flash(z45)
    ok &= run('sergei_ml2_boot', fi, S2 + 'wait:1200 snap:sergei_ml2_startup wait:4000',
              ['Machdyne Sergei ML2', 'Starting Zeitlos in', '[RECONFIG: the FPGA would load 0x000000]'], exe=X)
    ok &= run('sergei_ml2_board', fi, S2 + "f8 left 'b right wait:50 snap:sergei_ml2_board",
              ['Sergei ML2', 'ECP5-45F', 'USB 2'], ['Card reader (later)'], exe=X)
    # the card's SERGEI2.BIT files are listed; nothing is written to the module's flash
    ok &= run('sergei_ml2_update', fi, S2 + 'f8 f2 wait:200 down wait:20 enter wait:2000 snap:sergei_ml2_update',
              ['Zeitlos 0.0.8                   not for this board', 'Mach86', 'Kakao',
               'installing comes with the RP2040'], card=c['c008'], exe=X)
    # a changed setting, then Start: the module flash is the same, byte for byte
    ok &= run('sergei_ml2_settings', fi, S2 + "f8 left 't right wait:50 enter wait:50 snap:sergei_ml2_settings "
              "esc wait:50 left 's right wait:50 enter wait:500", ['the RP2040 keeps them', '[RECONFIG: the FPGA would load 0x000000]'], card=c['c008'], exe=X)
    ok &= check(flash_out('sergei_ml2_settings') is None, 'Sergei ML2: not one write to the module flash')
    ok &= run('mozart_ml0_board', sechzig_flash(z25), "sechzig board:0x0003 idcode:41111043 f8 left 'b right wait:50 snap:mozart_ml0_board",
              ['Mozart ML0', 'ECP5-25F'], exe=X)
    ok &= run('mozart_ml1_start', sechzig_flash(z45), 'sechzig board:0x0103 idcode:41112043 f8 wait:50 snap:mozart_ml1_start',
              ['Machdyne Mozart ML1'], exe=X)
    # Konfekt: one USB host port
    ok &= run('konfekt_board', flash(bitstream(0x21111043)), "board:5 idcode:21111043 ports:1 usb:kbd wait:1500 "
              "u:f8 wait:300 left 'b right wait:50 snap:konfekt_board",
              ['Konfekt', 'USB 1        keyboard'], ['USB 2'])
    return ok

# ---- the card reader on Sergei/Mozart: through the RP2040, over XA-XD ----

def xlink_cases(c):
    ok = True
    X = 'mbhost-sechzig'
    fi = sechzig_flash(bitstream(0x41112043))
    S2 = 'sechzig board:0x0202 idcode:41112043 '
    CARD = "f8 left 'c right wait:50 "
    img = open(c['c008'], 'rb').read()
    # no machboot-sv on the RP2040: the page says so
    ok &= run('xl_waiting', fi, S2 + CARD + 'snap:xl_waiting',
              ['Share the card with the PC', 'The RP2040 needs machboot-sv.uf2'], card=c['c008'], exe=X)
    # before sharing: no medium; shared: sectors, byte for byte
    r0 = os.path.join(B, 'xl_r0.bin'); r1 = os.path.join(B, 'xl_r1.bin')
    for p in (r0, r1):
        if os.path.exists(p): os.remove(p)
    ok &= run('xl_read', fi, S2 + 'xl:on wait:200 ' + CARD + 'xl:read:0:1 xl:wait enter wait:100 xl:hello xl:wait '
              'xl:read:0:3:%s xl:read:2051:2:%s xl:wait wait:300 snap:xl_read' % (r0, r1),
              ['xlink: the RP2040 answers', '[xl: READ: status 1, 0 sectors', '[xl: HELLO: shared 1',
               '[xl: READ: status 0, 3 sectors, 1540 bytes]', 'The SD card is on the PC as a USB drive.',
               'Read 0.0 MB'], card=c['c008'], exe=X)
    ok &= check(os.path.exists(r0) and open(r0, 'rb').read() == img[0:1536], 'xlink READ: sectors 0-2 are the card\'s')
    ok &= check(os.path.exists(r1) and open(r1, 'rb').read() == img[2051 * 512:2053 * 512], 'xlink READ: sectors 2051-2052')
    # WRITE lands on the card
    w = os.path.join(B, 'xl_w.bin')
    data = bytes((i * 7 + 3) & 0xff for i in range(1536))
    open(w, 'wb').write(data)
    if os.path.exists(c['c008'] + '.out'): os.remove(c['c008'] + '.out')
    ok &= run('xl_write', fi, S2 + 'xl:on wait:200 ' + CARD + 'enter wait:100 xl:write:60000:%s xl:wait wait:300 snap:xl_write' % w,
              ['[xl: WRITE: status 0, 3 sectors]', 'written 0.0 MB'], card=c['c008'], exe=X)
    out = open(c['c008'] + '.out', 'rb').read() if os.path.exists(c['c008'] + '.out') else b''
    ok &= check(out[60000 * 512:60003 * 512] == data, 'xlink WRITE: the card holds the three sectors')
    ok &= check(out[:60000 * 512] == img[:60000 * 512], 'xlink WRITE: nothing else changed')
    if os.path.exists(c['c008'] + '.out'): os.remove(c['c008'] + '.out')
    # the PC ejects: the card is machboot's again, and the flags say so
    ok &= run('xl_eject', fi, S2 + 'xl:on wait:200 ' + CARD + 'enter wait:100 xl:eject xl:wait wait:300 xl:hello xl:wait snap:xl_eject',
              ['ejected on the PC', '[xl: HELLO: shared 0', 'Share the card with the PC'], card=c['c008'], exe=X)
    # settings the RP2040 keeps: handed over at start, before the countdown would begin
    ok &= run('xl_settings_in', fi, S2 + "xl:set:boot.auto=0|boot.wait=5|boot.kbd=de| xl:on wait:300 left 't right wait:50 snap:xl_settings_in",
              ['xlink: settings from the RP2040', 'Start by itself                                 no',
               'Wait                                     5 seconds', 'Keyboard                                   Deutsch'],
              ['Starting Zeitlos in'], card=c['c008'], exe=X)
    # changed in the menu: the RP2040 fetches them once, when the page is left
    ok &= run('xl_settings_out', fi, S2 + "xl:on wait:300 f8 left 't right wait:50 right wait:50 esc wait:300 snap:xl_settings_out",
              ['[xl: GET: boot.auto=0|boot.wait=3|boot.kbd=us|sys=m|0|0|Zeitlos||]', 'Saved.'], card=c['c008'], exe=X)
    # Start: the RP2040 is told first (it lets that reset boot the module's flash)
    ok &= run('xl_start', fi, S2 + 'xl:on wait:300 enter wait:2000',
              ['[xl: START: status 0', '[RECONFIG: the FPGA would load 0x000000]'], card=c['c008'], exe=X)
    # no machboot-sv: Start goes ahead without waiting
    ok &= run('xl_start_alone', fi, S2 + 'enter wait:2000', ['[RECONFIG: the FPGA would load 0x000000]'],
              ['[xl: START'], card=c['c008'], exe=X)
    # the RP2040 goes quiet (reset, or its old firmware back): the page notices
    ok &= run('xl_gone', fi, S2 + 'xl:on wait:200 ' + CARD + 'xl:off wait:4000 snap:xl_gone',
              ['The RP2040 needs machboot-sv.uf2'], card=c['c008'], exe=X)
    return ok

# ---- the list of systems, the RP2040's flash as their cache ----

def systems_cases(c):
    ok = True
    X = 'mbhost-sechzig'
    fi = sechzig_flash(bitstream(0x41112043))
    S2 = 'sechzig board:0x0202 idcode:41112043 '
    INST = 'f8 f2 wait:100 '
    m86 = 'xl:put:Mach86:0.1:%s/s45.bit ' % T
    # Install: what Enter writes is said first; then it is in the list, in flash
    ok &= run('sys_install', fi, S2 + 'xl:on wait:300 ' + INST + 'down wait:50 snap:sys_install_hint '
              'enter wait:3000 snap:sys_install_done left up right wait:50 snap:sys_list',
              ['Enter copies Mach86 0.1 from the SD card into flash', "adds it to Start's list",
               '[xl: PUT: Mach86 0.1, ', '[xl: PUT: done', 'Mach86 0.1 is installed.',
               'default, module', 'Enter starts Zeitlos from the module', 'Mach86 0.1'],
              ['To make room'], card=c['c008'], exe=X)
    ok &= check(flash_out('sys_install') is None, 'systems: not one write to the module flash')
    # a cached one, moved to the top, starts over JTAG: no write
    ok &= run('sys_start_cached', fi, S2 + m86 + 'xl:on wait:300 f8 down wait:20 snap:sys_cached_hint f3 wait:50 snap:sys_moved enter wait:4000',
              ['Enter starts Mach86 from flash (nothing is written)', 'default, in flash',
               '[xl: GET: boot.auto=1|boot.wait=3|boot.kbd=us|sys=c|', '[xl: RUN: Mach86 0.1, '],
              ['[xl: PUT'], card=c['c008'], exe=X)
    # not enough room: the hint names what goes; Enter makes room, then copies
    ok &= run('sys_evict', fi, S2 + 'xl:cachesize:0x100000 ' + m86 + 'xl:on wait:300 ' + INST + 'down down wait:50 snap:sys_evict_hint '
              'enter wait:4000 snap:sys_evict_done',
              ['Enter copies Kakao 0.3 from the SD card into flash', 'To make room, Mach86 is removed from flash.',
               '[xl: DROP:', 'erased]', '[xl: PUT: done', 'Kakao 0.3 is installed.'], card=c['c008'], exe=X)
    # Esc while copying: nothing in the cache, nothing in the list
    ok &= run('sys_cancel', fi, S2 + 'xl:on wait:300 ' + INST + 'down down wait:50 enter esc wait:2000 snap:sys_cancel left up right wait:50 snap:sys_cancel_list',
              ['[xl: PUT: cancelled', 'Cancelled: Kakao was not installed.'],
              ['[xl: PUT: done'], card=c['c008'], exe=X)
    ok &= check('Kakao' not in open(os.path.join(B, 'sys_cancel_list.txt')).read(), 'systems: a cancelled one is not in the list')
    # the top one is on the card only: the countdown says it copies first
    ok &= run('sys_countdown_copy', fi, S2 + 'xl:set:boot.auto=1|boot.wait=3|boot.kbd=us|sys=c~1234~700000~Kakao~0.3|sys=m~0~0~Zeitlos~| '
              'xl:on wait:1200 snap:sys_countdown_copy wait:6000',
              ['Copying Kakao from the SD card into flash, then starting it, in', '[xl: PUT: done',
               '[xl: RUN: Kakao 0.3, '], card=c['c008'], exe=X)
    # the RP2040 could not start one: no countdown, the menu says why
    ok &= run('sys_note', fi, S2 + 'xl:note:Mach86_did_not_start_(the_FPGA_rejected_it) xl:on wait:1500 snap:sys_note',
              ['Mach86 did not start (the FPGA rejected it)', 'Start a system'], ['Starting Zeitlos in'], card=c['c008'], exe=X)
    # Remove asks first, then erases it from flash
    ok &= run('sys_remove', fi, S2 + m86 + "xl:on wait:300 f8 down del wait:50 snap:sys_remove_ask 'y wait:300 snap:sys_removed",
              ['Remove Mach86 from the list?', 'It is erased from flash; the card keeps its copy.', '[xl: DROP:', 'Mach86 was removed.'],
              card=c['c008'], exe=X)
    return ok

# ---- Lakritz and Konfekt, the list with one slot ----

def slot_cases(c):
    ok = True
    X = 'mbhost-slot'
    z25 = bitstream(0x41111043)
    fi = flash(z25)
    m86 = open('%s/m86.bit' % T, 'rb').read()
    START = "left 's right wait:50 "
    ADD = 'wait:300 f8 f2 wait:100 down wait:50 enter wait:200 '
    # the installed menu as make sim draws it (tests/check_sim.py compares the pixels)
    ok &= run('slot_menu', fi, 'usbd wait:300 f8 wait:50 snap:slot_start', ['default, in flash'], exe=X)
    # the slot's system is the list's first; starting it writes nothing
    ok &= run('slot_list', fi, 'wait:300 f8 wait:50 snap:slot_list',
              ['Start a system', 'default, in flash', 'Enter starts Zeitlos from flash (nothing is written).'],
              ['Starting Zeitlos ...'], card=c['c008'], exe=X)
    # Install adds to the list; nothing is written to the slot
    ok &= run('slot_add', fi, 'wait:300 f8 f2 wait:100 down wait:50 snap:slot_add_hint enter wait:200 snap:slot_add_done '
              + START + 'down wait:50 snap:slot_add_list',
              ['Enter adds Mach86 0.1 to Start', 'Nothing is written now', 'Mach86 is in Start',
               'on the card', 'Enter copies Mach86 from the SD card into flash', 'It replaces Zeitlos in flash.',
               'It is copied back when it is started again.'], card=c['c008'], exe=X)
    out = flash_out('slot_add')
    ok &= check(out is not None and out[0x060000:0x100000] == fi[0x060000:0x100000], 'slot: Install wrote only the settings')
    # Start on it: copied into the slot, then PROGRAMN
    ok &= run('slot_start_copy', fi, ADD + START + 'down wait:50 enter wait:6000',
              ['systems: copying Mach86', 'into the slot: installed', '[RECONFIG: the FPGA would load 0x060000]'],
              card=c['c008'], exe=X)
    out = flash_out('slot_start_copy')
    ok &= check(out is not None and out[0x060000:0x060000 + len(m86)] == m86 and out[0x100000:0x100004] == fi[0x100000:0x100004],
                'slot: Mach86 in the slot byte for byte, the Zeitlos region untouched')
    # Esc while it compares: nothing written
    ok &= run('slot_cancel', fi, ADD + START + 'down wait:50 enter esc wait:3000 snap:slot_cancel',
              ['Cancelled: nothing was written or started.'], ['RECONFIG'], card=c['c008'], exe=X)
    out = flash_out('slot_cancel')
    ok &= check(out is not None and out[0x060000:0x100000] == fi[0x060000:0x100000], 'slot: cancelled while comparing: the slot as it was')
    # moved to the top: after a restart the countdown says it copies first
    ok &= run('slot_move', fi, ADD + START + 'down wait:50 f3 wait:100 snap:slot_moved',
              ['default, on the card'], card=c['c008'], exe=X)
    moved = flash_out('slot_move')
    if moved:
        ok &= run('slot_countdown', moved, 'wait:1200 snap:slot_countdown wait:8000',
                  ['Copying Mach86 from the SD card into flash, then starting it, in', 'into the slot: installed',
                   '[RECONFIG: the FPGA would load 0x060000]'], card=c['c008'], exe=X)
    else:
        ok &= check(False, 'slot: the moved list was saved')
    # Del: the slot's stays; another goes after Y, with nothing written but the list
    ok &= run('slot_remove', fi, ADD + START + "del wait:50 snap:slot_remove_slot "
              "down del wait:50 snap:slot_remove_ask 'y wait:200 snap:slot_removed",
              ['Zeitlos is in flash: it stays in the list.', 'Remove Mach86 from the list?',
               'Nothing is written; the card keeps its copy.', 'Mach86 was removed.'], card=c['c008'], exe=X)
    # a newer version of the slot's system: Install copies it now
    ok &= run('slot_update', fi, 'wait:300 f8 f2 wait:100 snap:slot_update_hint enter wait:6000 snap:slot_update_done',
              ['Enter copies Zeitlos 0.0.9 from the SD card into flash', 'The Zeitlos region is not touched.',
               'Zeitlos 0.0.9 is installed.'], card=c['c009'], exe=X)
    return ok

def main():
    os.makedirs(B, exist_ok=True)
    ok = True
    z25 = bitstream(0x41111043)

    ok &= run('boot_default', flash(z25), 'wait:1200 snap:startup wait:4000',
              ['Starting Zeitlos in 2', 'RECONFIG'] and ['Starting Zeitlos in', 'M A C H B O O T'])
    ok &= run('enter_starts', flash(z25), 'wait:300 enter wait:100', ['Starting Zeitlos ...'])
    # 'usbd': as the simulated SoC, so start.png and no_system.png match its frames
    ok &= run('menu', flash(z25),
              'usbd wait:300 f8 wait:50 snap:start '
              'f1 snap:start_details f1 '
              'left down snap:sidebar_update right snap:update '
              'left down down snap:settings_side right snap:settings '
              'esc down down snap:diag_side right snap:diagnostics '
              "'a wait:50 snap:diag_done "
              'left down snap:board_side right snap:board f1 snap:board_details '
              'left up up up up up up snap:back_to_start right enter wait:50',
              ['Start a system', 'default', 'Zeitlos starts by itself after 3 seconds.',
               'Built for   ECP5-25F', 'Memory', 'OK', 'Board', 'Lakritz', 'ECP5-25F', '2 MB',
               'IDCODE      41111043', 'This comes in a later version'])
    ok &= run('no_system', flash(None, zeitlos=False), 'usbd wait:300 snap:no_system',
              ['No system is installed yet.'], ['Starting'])
    ok &= run('wrong_die', flash(bitstream(0x41112043)), 'wait:300 snap:wrong_die',
              ['The system in flash is for another FPGA.', 'built for the ECP5-45F.', 'This board has the ECP5-25F.'])
    ok &= run('install_mode', flash(None, at=0x040000, fw_at=0x090000), 'fwbase:090000 wait:300 snap:install_mode',
              ['running from where dfu-util put it'])
    ok &= run('video', flash(z25), "wait:300 f8 left down down down down down right down down down enter wait:50 snap:video_pattern 'x wait:50",
              ['Any key returns.'])
    # ---- the card, installing, settings, USB keyboards ----
    c = cards()
    z008 = open(T + '/z008.bit', 'rb').read()
    SLOT = 0x060000

    ok &= run('card_list', flash(z25), 'wait:300 f8 f2 wait:50 snap:update_list f1 snap:update_details',
              ['On the SD card:', 'Zeitlos 0.0.8', 'Mach86 0.1', 'Kakao 0.3', 'not for this board',
               'partition 1, FAT32', 'ZEITLOS\\LAKRITZ.BIT'], ['BLINK'], card=c['c008'])
    ok &= run('install', flash(z25), 'wait:300 f8 f2 wait:50 enter wait:30000 snap:installed',
              ['Zeitlos 0.0.8 is installed.', 'installed'], card=c['c008'])
    f_inst = flash_out('install')
    ok &= check(f_inst is not None and f_inst[SLOT:SLOT + len(z008)] == z008,
                'install: the slot holds the bitstream, byte for byte')
    if f_inst:
        ok &= run('after_install', f_inst, 'wait:300 f8 wait:50 snap:start_installed f1 snap:start_installed_details',
                  ['Zeitlos 0.0.8', 'Installed   470000 bytes'], card=c['c008'])
        ok &= run('install_same', f_inst, 'wait:300 f8 f2 wait:50 enter wait:30000 snap:install_same',
                  ['Zeitlos 0.0.8 was already installed.'], card=c['c008'])
        ok &= check(flash_out('install_same') is None, 'install_same: nothing was written to the flash')
        ok &= run('newer_on_card', f_inst, 'wait:600 snap:startup_newer f2 wait:50 snap:update_newer',
                  ['A new Zeitlos is on the card.  F2 Install', 'Zeitlos 0.0.9', 'new'], card=c['c009'])
        # a power cut in the middle of an update, then the recovery
        ok &= run('power_cut', f_inst, 'wait:300 f8 f2 wait:50 cut:60 enter wait:30000',
                  [], card=c['c009'])
        f_cut = flash_out('power_cut')
        ok &= check(f_cut is not None and f_cut[SLOT:SLOT + 4] == b'\xff' * 4,
                    'power_cut: the slot\'s first sector is erased (the slot is invalid)')
        if f_cut:
            ok &= run('recover', f_cut, 'wait:300 snap:recover enter wait:30000 snap:recovered',
                      ['Zeitlos did not finish installing.', 'Install Zeitlos again', 'Nothing else was changed.',
                       'Zeitlos 0.0.9 is installed.'], ['Starting'], card=c['c009'])
            z009 = open(T + '/z009.bit', 'rb').read()
            f_rec = flash_out('recover')
            ok &= check(f_rec is not None and f_rec[SLOT:SLOT + len(z009)] == z009,
                        'recover: the slot holds 0.0.9, byte for byte')
    ok &= run('wrong_fpga_file', flash(z25), 'wait:300 f8 f2 wait:50 enter wait:5000 snap:wrong_fpga_file',
              ['Not installed: the file is for another FPGA.'], card=c['c45'])
    ok &= check(flash_out('wrong_fpga_file') is None, 'wrong_fpga_file: nothing was written')
    ok &= run('card_fat16', flash(z25), 'wait:300 f8 f2 wait:50 f1 snap:card_fat16',
              ['Zeitlos 0.0.8', 'partition 2, FAT16'], card=c['c16'])
    ok &= run('card_fat12', flash(z25), 'wait:300 f8 f2 wait:50 f1 snap:card_fat12',
              ['Mach86 0.1', 'partition 0, FAT12'], card=c['c12'])
    ok &= run('card_nogateware', flash(z25), 'wait:300 f8 f2 wait:50 snap:card_nogateware',
              ['This card has no \\SYS\\GATEWARE folder.'], card=c['cnone'])
    ok &= run('no_card', flash(z25), 'wait:300 f8 f2 wait:50 snap:no_card', ['No SD card.'])
    ok &= run('install_mode_update', flash(None, at=0x040000, fw_at=0x090000),
              'fwbase:090000 wait:300 f2 wait:50 snap:install_mode_update',
              ['Install machboot first (Start)'], card=c['c008'])
    ok &= check(flash_out('install_mode_update') is None, 'install_mode_update: nothing was written')

    # settings: change, save, and see them at the next start
    ok &= run('settings_change', flash(z25),
              'wait:300 f8 left down down down right snap:settings_focus left '
              'down right right snap:settings_wait down right snap:settings_kbd up up up wait:10 esc wait:50 '
              'right snap:settings_saved',
              ['no', '10 seconds', 'Deutsch', 'Saved.'])
    f_set = flash_out('settings_change')
    ok &= check(f_set is not None and b'boot.wait' in f_set[0x05E000:0x060000]
                and f_set[:0x05E000] == flash(z25)[:0x05E000],
                'settings: written to the settings sectors only (kvlog), nowhere else')
    if f_set:
        ok &= run('settings_kept', f_set, "wait:300 snap:settings_kept left 't right snap:settings_kept_page",
                  ['Nothing starts by itself (Settings).', '10 seconds', 'Deutsch'], ['Starting Zeitlos in'])

    # USB keyboards
    ok &= run('usb_keyboard', flash(z25),
              'usb:kbd wait:1500 u:f8 wait:200 snap:usb_menu u:left wait:100 u:b wait:100 u:right wait:100 '
              'snap:usb_board u:esc wait:100 u:s wait:100 u:right wait:100 u:enter wait:100',
              ['usb 1: keyboard 04c4:2001 (low speed)', 'keyboard  04c4:2001', 'Starting Zeitlos ...'])
    ok &= run('usb_mouse', flash(z25), "usb:mouse wait:1500 f8 wait:50 left 'b wait:50 snap:usb_mouse",
              ['not used  04c4:2001'])
    ok &= run('diag_sd', flash(z25), "wait:300 f8 left 'd right 'a wait:3000 snap:diag_sd",
              ['SD card', 'OK, '], card=c['c008'])

    ok &= install_mode_cases(c, z008)
    ok &= usb_device_cases(c, z008, z25)
    ok &= usb_host_cases(c, z008, z25)
    ok &= board_cases(c, z25)
    ok &= xlink_cases(c)
    ok &= systems_cases(c)
    ok &= slot_cases(c)

    if os.path.exists('build/lakritz.bit'):
        real = open('build/lakritz.bit', 'rb').read()
        ok &= run('real_bitstream', flash(real), 'wait:300 f8 f1 snap:real_bitstream',
                  ['Built for   ECP5-25F'])
    sys.exit(0 if ok else 1)

if __name__ == '__main__':
    main()
