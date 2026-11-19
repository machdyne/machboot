# Flash layout

All addresses are 64 KB aligned (an ECP5 boot address is `addr[23:16]`,
Lattice TN1260 / Zeitlos `docs/zboot.md` section 2).

## Lakritz and other stand-alone boards, 2 MB

```
0x000000  boot slot, 384 KB
  0x000000    the menu's bitstream (221 KB today on a 25F; room to 320 KB)
  0x050000    the menu's firmware: 32-byte header (fw/flash.h), then the image; a larger image
              goes just below the header, after the bitstream (docs/boot.md). The menu's
              pages (overlays, docs/overlays.md) follow the image, or the header when the
              image is below it
  0x05E000    settings, 8 KB (kvlog, docs/settings.md)
0x060000  system slot, 640 KB      the installed system's gateware
0x100000  Zeitlos region, 1 MB     kernel, core apps, key/value store; machboot never touches it
```

The menu's bitstream is packed with **boot address 0x060000**
(`ecppack --bootaddr`), so pulling PROGRAMN from the menu loads the
system slot. A system packed with boot address 0 (Zeitlos is) comes back
to the menu when it pulls PROGRAMN (`reboot`).

**Why 384 KB.** A 25F bitstream is about 582 KB uncompressed whatever
the design, so the 640 KB system slot holds any 25F design that can
exist -- Zeitlos (481 KB compressed today) can grow without the slot
ever being the limit. Taking more for the boot slot would break that.

**The write fence.** The SPI flash controller (`rtl/zeitlos/spiflash.v`)
refuses any erase or program below `LOCK_END`, which machboot sets to
0x060000 -- except the settings window, 0x05E000-0x05FFFF. Nothing the
firmware does can write the menu's bitstream or firmware. (Installing
machboot itself is done by a different gateware build, the installer,
whose fence protects the Zeitlos region instead: [install-mode.md](install-mode.md).)

## 4 MB and larger (planned)

```
0x000000  boot slot                  as above
0x060000  Zeitlos gateware
0x100000  Zeitlos region, 1 MB
0x200000  jumploader, 320 KB         the same address and format (ZJUMP1) as Zeitlos JUMP=1
0x250000  slots ...                  Mach86, scratch slots for "run from card"
```

The first 2 MB are identical on every size, so a 2 MB image works on a
4 MB MMOD unchanged.

## Sergei / Mozart

The module's flash keeps its own layout (its system at 0, the Zeitlos
region at 0x100000) and machboot never writes it. The menu and the
systems in the list are in the RP2040's flash and are loaded into the
FPGA over JTAG (docs/power-on.md has that flash's layout).

## Whole-flash images

`tools/mkimage.py` builds one:

```
python3 tools/mkimage.py out.bin --boot build/machboot-lakritz.bin \
        --system zeitlos-gateware.bit --region zeitlos-region.bin --size 2M
```

## Zeitlos under machboot

Zeitlos assumed its gateware starts at 0x040000 (the tinydfu layout).
Two places needed to learn machboot's layout before machboot is
installed for real: the kernel's write lock, and `zfpga flash`'s free
space. Both are in `zeitlos-patches/`; [zeitlos.md](zeitlos.md) has the
details.

## Install mode

On a board with tinydfu, `dfu-util -a 0` writes the installer image at
0x040000-0x0FA000, and the installer moves machboot to 0x000000
([install-mode.md](install-mode.md)).
