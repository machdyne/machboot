# Building

## Toolchain

| Tool | Used for | Notes |
|---|---|---|
| yosys | synthesis, and the simulation model (CXXRTL) | native, or `pip install yowasp-yosys` |
| nextpnr-ecp5, ecppack | place and route, bitstream | native, or `pip install yowasp-nextpnr-ecp5` |
| RISC-V GCC | boot ROM and firmware | any rv32im-capable one: `riscv-none-elf-` (xPack) or `riscv64-unknown-elf-` |
| Python 3 + Pillow | font, images, tests, PNGs | |
| pyfatfs | test SD card images | `pip install pyfatfs`; tests only |
| g++, cc | the simulation, the PC build of the firmware | |
| openFPGALoader / dfu-util | putting it on a board | |

The Makefile finds native tools first, YoWASP otherwise; override with
`YOSYS=`, `NEXTPNR=`, `ECPPACK=`, `CROSS=`. machboot 0.4 was built with
yosys and nextpnr-ecp5 from YoWASP, xPack riscv-none-elf-gcc 14.2.0,
xPack arm-none-eabi-gcc 14.2.1 and pico-sdk 1.5.1 (`PICO_SDK_PATH=`).

## Targets

| | |
|---|---|
| `make` | `build/machboot-lakritz.bin`: the boot-slot image (bitstream at 0, firmware at 0x050000) |
| `make sram` | `build/machboot-lakritz-sram.bit`: the JTAG test build, firmware in RAM |
| `make installer` | `build/machboot-lakritz-install.bin`: install mode, for `dfu-util -a 0` on a board with tinydfu (docs/install-mode.md) |
| `make host` | the PC build of the firmware; runs `tests/run_host.py`; screens in `build/host/` |
| `make sim` | the whole-system simulation; runs `tests/check_sim.py`; frames in `build/sim/` |
| `make test` | host, sv-test (the RP2040's protocol code), xlink-test (the link RTL), then sim |
| `make xlink-test` | `rtl/mb_xlink.v` alone in CXXRTL against a real-time SPI master (tests/xlink_tb.cc) |
| `make measure` | also the 45F build; prints each build's size and timing |
| `make flash` | writes the boot-slot image at 0 over JTAG (dirtyJtag) -- try `make sram` first |
| `make BOARD=konfekt` (`all sram installer`) | the same three for Konfekt: `build/machboot-konfekt*.{bin,bit}` |
| `make BOARD=sergei_ml2 sram` | the SRAM build for one Sergei/Mozart: `sergei_ml0..2`, `mozart_ml0..2` |
| `make sechzig` | all six of those: `build/machboot-{sergei,mozart}_ml{0,1,2}-sram.bit` |
| `make sv` | the RP2040's supervisor, `build/machboot-sv.uf2` (needs PICO_SDK_PATH) |
| `make uf2` | after `make sv` and `make sechzig`: `build/machboot-{sergei,mozart}_ml{0,1,2}.uf2`, the supervisor with each module's menu (tools/mkuf2.py) |

The boards and what each build does: docs/boards.md.

Three firmware images come from `fw/`:

- `build/fw.img`, for Lakritz and Konfekt in flash: its pages are
  overlays (docs/overlays.md). `tools/check_ovl.py` checks the link.
- `fw-ram.hex`, the JTAG test build for Lakritz and Konfekt
  (`build/fw-jtag.elf`): every page resident.
- `build/fw-sechzig-ram.hex`, for Sergei and Mozart: `fw/xlink.c` (the
  card reader through the RP2040) in place of `fw/usbd.c`, the list of
  systems (docs/systems.md), and no install mode.

## What gets built

```
build/font.hex                 font ROM (tools/mkfont.py)
build/bootrom.hex              boot ROM, 256 words (bootrom/)
build/fw.img                   firmware with its header (tools/mkfw.py)
build/lakritz.json/.config     synthesis / place-and-route
build/lakritz-packed.config    the same without all-zero block-RAM inits (tools/strip_bram_init.py)
build/lakritz.bit              compressed, boot address 0x060000
build/machboot-lakritz.bin     the boot slot (tools/mkslot.py)
build/lakritz-inst.bit         the installer gateware, boot address 0 (make installer)
build/machboot-lakritz-install.bin  the install-mode image (tools/mkinstaller.py)
```

Logs: `build/lakritz-synth.log`, `build/lakritz-pnr.log` (utilisation
and routed clock speeds at the end).

## Coding rules

Verilog: plain Verilog-2005, no SystemVerilog, no declarations inside
blocks, every signal driven from one place. C: freestanding, no libc
(`fw/lib.c` has what is needed), `-Os -flto` (link-time optimisation
saves about 2.5 KB; `memcpy` and friends are marked `used` so it keeps
them for the calls the compiler generates itself).

**The firmware's RAM is the limit to watch.** 88 KB, of which 1 KB is
the boot ROM, 6 KB the stack, and the rest code, data and bss
(`fw/link.ld` fails the build if they pass 0x14800). 55 of the 25F's 56
block RAMs are in use. On Lakritz and Konfekt the menu's pages are
overlays, loaded from flash (docs/overlays.md), which leaves about
2.9 KB spare in the installed build. The JTAG test build keeps every
page in RAM and has about 230 bytes spare.

What each build uses (nextpnr-ecp5, speed grade 6; the system clock
needs 48 MHz):

| Build | FPGA | LUTs | Block RAMs | System clock |
|---|---|---|---|---|
| Lakritz | 25F | 9,577 (39%) | 55 of 56 | 56.0 MHz |
| Konfekt | 12F | 9,240 | 55 of 56 | 56.6 MHz |
| Sergei/Mozart ML0 | 25F | 9,124-9,127 | 55 of 56 | 52.2-54.2 MHz |
| Sergei/Mozart ML1, ML2 | 45F | 9,053-9,117 | 55 of 108 | 53.3-58.5 MHz |

The boot-slot image is 344,996 bytes, the installer 738,212 of its 768
KB, and the firmware's resident part 53,404 bytes.

The firmware image in flash: its header is always at 0x050000. An image
that fits in the 56 KB after the header (0x050020-0x05DFFF) follows it;
a larger one goes below the header, 4 KB
aligned and above the bitstream, and the header's word 7 says where
(docs/boot.md; `tools/mkslot.py` places it and checks the bitstream
does not overlap).

## The installer

`make installer` builds `build/machboot-lakritz-install.bin`, the image
for `dfu-util -a 0` on a board with tinydfu (docs/install-mode.md): a
third gateware build with `-DMB_INSTALLER` (the flash fence turned
around, packed with boot address 0), the firmware, and the normal
boot-slot image as its payload (`tools/mkinstaller.py`).
