# machboot -- boot manager for Machdyne FPGA computers
#
#   make                  the Lakritz boot-slot image (build/machboot-lakritz.bin)
#   make sram             a JTAG test build: load into SRAM, nothing needed in flash
#   make sechzig          the SRAM builds for Sergei and Mozart, ML0/ML1/ML2 (docs/sechzig.md)
#   make BOARD=sergei_ml2 sram   ... one of them
#   make BOARD=konfekt    Konfekt: everything Lakritz has (all, sram, installer)
#   make installer        the install-mode image for dfu-util on a board with tinydfu
#   make measure          also build the same menu for a 45F, and report sizes
#   make sim              boot the whole SoC with its firmware in simulation
#   make host             the menu on the PC: screenshots of every page
#   make test             host + sim checks
#   make flash            write the boot-slot image to a Lakritz over JTAG
#
# docs/building.md has the details.

BOARD ?= lakritz

# toolchain: native tools if present, YoWASP (pip) otherwise
YOSYS   ?= $(shell command -v yosys 2>/dev/null || echo yowasp-yosys)
NEXTPNR ?= $(shell command -v nextpnr-ecp5 2>/dev/null || echo yowasp-nextpnr-ecp5)
ECPPACK ?= $(shell command -v ecppack 2>/dev/null || echo yowasp-ecppack)
CROSS   ?= $(shell command -v riscv-none-elf-gcc >/dev/null 2>&1 && echo riscv-none-elf- || echo riscv64-unknown-elf-)
PYTHON  ?= python3

CC      = $(CROSS)gcc
HOSTCC ?= cc
OBJDUMP = $(CROSS)objdump
OBJCOPY = $(CROSS)objcopy
SIZE    = $(CROSS)size

ARCH    = -march=rv32im_zicsr -mabi=ilp32
CFLAGS  = $(ARCH) -Os -flto -g -Wall -Wextra -Wno-unused-parameter -ffreestanding -fno-builtin \
          -fno-tree-loop-distribute-patterns -ffunction-sections -fdata-sections -nostdlib -std=gnu11
LDFLAGS = $(ARCH) -Os -flto -nostdlib -nostartfiles -Wl,--gc-sections -lgcc -Wl,--no-warn-rwx-segments

B = build
MB_VERSION = $(shell sed -n "s/.*MB_VERSION_STR\t*\"\(.*\)\"/\1/p" fw/mb.h)
MB_VERSION_HEX = $(shell sed -n "s/^\#define MB_VERSION\t*\(0x[0-9a-fA-F]*\).*/\1/p" fw/mb.h)
FW_RAM = $(B)/fw-ram.hex

# -- board --------------------------------------------------------------------

ifeq ($(BOARD),lakritz)
  DEVICE  = 25k
  PACKAGE = CABGA256
  LPF     = boards/lakritz.lpf
  TOP     = top_lakritz
  SLOT    = 0x060000
  USBD_RTL = rtl/mb_usbd.v rtl/ulixxe/sie.v rtl/ulixxe/phy_rx.v rtl/ulixxe/phy_tx.v
  DEFINES = -DMB_USB_HOST -DMB_USB_DEV
endif

ifeq ($(BOARD),konfekt)
  DEVICE  = 12k
  PACKAGE = CABGA256
  LPF     = boards/konfekt.lpf
  TOP     = top_konfekt
  SLOT    = 0x060000
  USBD_RTL = rtl/mb_usbd.v rtl/ulixxe/sie.v rtl/ulixxe/phy_rx.v rtl/ulixxe/phy_tx.v
  DEFINES = -DMB_USB_HOST -DMB_USB_DEV
endif

# A Sechzig module in a Sergei or Mozart carrier: SRAM builds only (rtl/top_sechzig.v)
SECHZIG = sergei_ml0 sergei_ml1 sergei_ml2 mozart_ml0 mozart_ml1 mozart_ml2
ifneq ($(filter $(BOARD),$(SECHZIG)),)
  CARRIER = $(word 1,$(subst _, ,$(BOARD)))
  MODULE  = $(subst ml,,$(word 2,$(subst _, ,$(BOARD))))
  DEVICE  = $(if $(filter 0,$(MODULE)),25k,45k)
  PACKAGE = CABGA256
  LPF     = boards/$(CARRIER)_ml$(if $(filter 0,$(MODULE)),1,$(MODULE)).lpf
  TOP     = top_sechzig
  SLOT    = 0x000000
  FW_RAM  = $(B)/fw-sechzig-ram.hex
  USBD_RTL =
  XLINK_RTL = rtl/mb_xlink.v
  DEFINES = -DMB_USB_HOST -DMB_XLINK -DMB_CARRIER=$(if $(filter sergei,$(CARRIER)),2,3) -DMB_MODULE=$(MODULE) \
            $(if $(filter 0,$(MODULE)),,-DMB_DIE_45F) $(if $(filter 2,$(MODULE)),-DMB_DDR3_PARK)
endif

RTL = rtl/$(TOP).v rtl/mb_soc.v rtl/mb_ram.v rtl/mb_text.v rtl/mb_tmds.v rtl/mb_dvi.v \
      rtl/mb_sysctl.v rtl/mb_pll.v \
      rtl/zeitlos/spiflash.v rtl/zeitlos/uart.v rtl/zeitlos/spim.v \
      rtl/cpu/zeitlos32/zeitlos32.v rtl/cpu/zeitlos32/zeitlos32_decode.v \
      rtl/cpu/zeitlos32/zeitlos32_alu.v rtl/cpu/zeitlos32/zeitlos32_lsu.v \
      rtl/cpu/zeitlos32/zeitlos32_muldiv.v \
      rtl/usb/usb_host.v rtl/usb/usb_xact.v rtl/usb/usb_sie.v rtl/usb/usb_port.v \
      rtl/usb/usb_hid_compat.v $(USBD_RTL) $(XLINK_RTL)

FW_PAGES = fw/page_start.c fw/page_update.c fw/page_card.c fw/page_settings.c fw/page_diag.c \
           fw/page_board.c fw/page_instmode.c
FW_SRC = fw/start.S fw/main.c fw/ui.c fw/screen.c fw/console.c fw/flash.c fw/diag.c fw/lib.c \
         fw/usb.c fw/kvlog.c fw/settings.c fw/sd.c fw/fat.c fw/card.c fw/install.c fw/selfinst.c fw/usbd.c \
         $(FW_PAGES)

.PHONY: all sram sechzig installer xlink-test uf2 measure sim host test flash clean sv sv-test release
all: $(B)/machboot-$(BOARD).bin

$(B):
	mkdir -p $(B)

# -- font, boot ROM, firmware ------------------------------------------------------

$(B)/font.hex: tools/mkfont.py tools/font/ascii8x16.mem | $(B)
	$(PYTHON) tools/mkfont.py $@ --unicode $(B)/codepage.py

$(B)/bootrom.elf: bootrom/start.S bootrom/boot.c bootrom/link.ld | $(B)
	$(CC) $(CFLAGS) -T bootrom/link.ld -o $@ bootrom/start.S bootrom/boot.c $(LDFLAGS)
	@$(SIZE) $@

$(B)/bootrom.bin: $(B)/bootrom.elf
	$(OBJCOPY) -O binary $< $@

$(B)/bootrom.hex: $(B)/bootrom.bin tools/bin2hex.py
	$(PYTHON) tools/bin2hex.py $< $@ 256

# Lakritz, Konfekt: the pages are overlays, kept in flash (docs/overlays.md).
# Each overlay's files are compiled on their own (no LTO), so that the
# linker script can place their code and strings by file name.
OVL_NAMES = start update card settings diag board instmode
OVL_FILES_start = fw/page_systems.c
OVL_FILES_update = fw/page_sysinst.c
OVL_FILES_card = fw/page_card.c
OVL_FILES_settings = fw/page_settings.c
OVL_FILES_diag = fw/page_diag.c fw/diag.c
OVL_FILES_board = fw/page_board.c
OVL_FILES_instmode = fw/page_instmode.c
OVL_SRC = $(foreach o,$(OVL_NAMES),$(OVL_FILES_$(o)))
OVL_OBJ = $(foreach o,$(OVL_NAMES),$(foreach f,$(OVL_FILES_$(o)),$(B)/ovl_$(o)_$(notdir $(f:.c=.o))))
# installed, Lakritz and Konfekt have the list of systems with one slot
# (docs/systems.md): the old Start and Install pages are the JTAG build's
FW_SLOT_DEFS = -DMB_OVERLAYS -DMB_SYSTEMS -DMB_SLOT
FW_RESIDENT = $(filter-out $(OVL_SRC) fw/page_start.c fw/page_update.c,$(FW_SRC)) fw/ovl.c fw/systems.c fw/ui_systems.c
OVL_CFLAGS = $(filter-out -flto,$(CFLAGS)) -fno-lto $(FW_SLOT_DEFS)

define ovl_rule
$(B)/ovl_$(1)_$(notdir $(2:.c=.o)): $(2) fw/*.h | $(B)
	$$(CC) $$(OVL_CFLAGS) -c $(2) -o $$@
endef
$(foreach o,$(OVL_NAMES),$(foreach f,$(OVL_FILES_$(o)),$(eval $(call ovl_rule,$(o),$(f)))))

$(B)/fw.elf: $(FW_RESIDENT) $(OVL_OBJ) fw/*.h fw/link_ovl.ld | $(B)
	$(CC) $(CFLAGS) $(FW_SLOT_DEFS) -T fw/link_ovl.ld -o $@ $(FW_RESIDENT) $(OVL_OBJ) $(LDFLAGS)
	@$(SIZE) $@
	$(PYTHON) tools/check_ovl.py $@ $(OBJDUMP)

# the JTAG test build (firmware preloaded in RAM, no flash to load pages
# from): every page resident, as before
$(B)/fw-jtag.elf: $(FW_SRC) fw/*.h fw/link.ld | $(B)
	$(CC) $(CFLAGS) -T fw/link.ld -o $@ $(FW_SRC) $(LDFLAGS)
	@$(SIZE) $@

$(B)/fw-jtag.bin: $(B)/fw-jtag.elf
	$(OBJCOPY) -O binary $< $@

# Sergei and Mozart: no USB device and no install mode; the card reader
# goes through the RP2040 (fw/xlink.c in place of fw/usbd.c)
FW_SECHZIG_SRC = $(filter-out fw/usbd.c fw/selfinst.c fw/install.c,$(FW_SRC)) fw/xlink.c fw/selfinst_none.c fw/install_none.c fw/systems.c \
                 fw/ui_systems.c fw/page_systems.c fw/page_sysinst.c

$(B)/fw-sechzig.elf: $(FW_SECHZIG_SRC) fw/*.h fw/link.ld | $(B)
	$(CC) $(CFLAGS) -DMB_SYSTEMS -T fw/link.ld -o $@ $(FW_SECHZIG_SRC) $(LDFLAGS)
	@$(SIZE) $@

$(B)/fw-sechzig.bin: $(B)/fw-sechzig.elf
	$(OBJCOPY) -O binary $< $@

$(B)/fw-sechzig-ram.hex: $(B)/fw-sechzig.bin tools/bin2hex.py
	head -c 1024 /dev/zero > $(B)/zero1k.bin
	$(PYTHON) tools/bin2hex.py $(B)/zero1k.bin $@ 22528 --at 0x400 $<

$(B)/fw.bin: $(B)/fw.elf
	$(OBJCOPY) -O binary $(foreach o,$(OVL_NAMES),-R .ovl_$(o)) $< $@
	for o in $(OVL_NAMES); do $(OBJCOPY) -O binary -j .ovl_$$o $< $(B)/fw-ovl-$$o.bin; done

$(B)/fw.img: $(B)/fw.bin tools/mkfw.py
	$(PYTHON) tools/mkfw.py $< $@ --version $(MB_VERSION_HEX) --ovl $(foreach o,$(OVL_NAMES),$(B)/fw-ovl-$(o).bin)

# -- gateware ---------------------------------------------------------------------

$(B)/$(BOARD).json: $(RTL) $(B)/bootrom.hex $(B)/font.hex
	$(YOSYS) -q -l $(B)/$(BOARD)-synth.log $(DEFINES) -p \
		"read_verilog $(DEFINES) $(RTL); synth_ecp5 -abc9 -top $(TOP) -json $@"

$(B)/$(BOARD).config: $(B)/$(BOARD).json $(LPF)
	$(NEXTPNR) -q -l $(B)/$(BOARD)-pnr.log --$(DEVICE) --package $(PACKAGE) --speed 6 \
		--lpf $(LPF) --json $< --textcfg $@ --timing-allow-fail

$(B)/$(BOARD).bit: $(B)/$(BOARD).config tools/strip_bram_init.py
	$(PYTHON) tools/strip_bram_init.py $< $(B)/$(BOARD)-packed.config
	$(ECPPACK) --compress --bootaddr $(SLOT) $(B)/$(BOARD)-packed.config --bit $@

$(B)/machboot-$(BOARD).bin: $(B)/$(BOARD).bit $(B)/fw.img tools/mkslot.py
	$(PYTHON) tools/mkslot.py $< $(B)/fw.img $@

# -- the JTAG test build: firmware in RAM, nothing needed in flash ---------------------

$(B)/bootrom-sram.elf: bootrom/start.S bootrom/boot.c bootrom/link.ld | $(B)
	$(CC) $(CFLAGS) -DMB_SRAM_TEST -T bootrom/link.ld -o $@ bootrom/start.S bootrom/boot.c $(LDFLAGS)

$(B)/bootrom-sram.hex: $(B)/bootrom-sram.elf tools/bin2hex.py
	$(OBJCOPY) -O binary $< $(B)/bootrom-sram.bin
	$(PYTHON) tools/bin2hex.py $(B)/bootrom-sram.bin $@ 256

$(B)/fw-ram.hex: $(B)/fw-jtag.bin tools/bin2hex.py
	head -c 1024 /dev/zero > $(B)/zero1k.bin
	$(PYTHON) tools/bin2hex.py $(B)/zero1k.bin $@ 22528 --at 0x400 $<

$(B)/$(BOARD)-sram.json: $(RTL) $(B)/bootrom-sram.hex $(FW_RAM) $(B)/font.hex
	$(YOSYS) -q -l $(B)/$(BOARD)-sram-synth.log $(DEFINES) -DMB_SRAM_TEST -p \
		"read_verilog $(DEFINES) -DMB_SRAM_TEST $(RTL); synth_ecp5 -abc9 -top $(TOP) -json $@"

$(B)/$(BOARD)-sram.config: $(B)/$(BOARD)-sram.json $(LPF)
	$(NEXTPNR) -q -l $(B)/$(BOARD)-sram-pnr.log --$(DEVICE) --package $(PACKAGE) --speed 6 \
		--lpf $(LPF) --json $< --textcfg $@ --timing-allow-fail

$(B)/machboot-$(BOARD)-sram.bit: $(B)/$(BOARD)-sram.config
	$(ECPPACK) --compress --bootaddr $(SLOT) $< --bit $@

sram: $(B)/machboot-$(BOARD)-sram.bit

sechzig:
	for b in $(SECHZIG); do $(MAKE) BOARD=$$b sram || exit 1; done

# -- the installer: written by tinydfu at 0x040000, installs machboot at 0 ------------
# Its gateware has the flash fence turned around (rtl/mb_soc.v, INSTALLER) and is
# packed with boot address 0, so its restart starts the new machboot.
# docs/install-mode.md

$(B)/$(BOARD)-inst.json: $(RTL) $(B)/bootrom.hex $(B)/font.hex
	$(YOSYS) -q -l $(B)/$(BOARD)-inst-synth.log $(DEFINES) -DMB_INSTALLER -p \
		"read_verilog $(DEFINES) -DMB_INSTALLER $(RTL); synth_ecp5 -abc9 -top $(TOP) -json $@"

$(B)/$(BOARD)-inst.config: $(B)/$(BOARD)-inst.json $(LPF)
	$(NEXTPNR) -q -l $(B)/$(BOARD)-inst-pnr.log --$(DEVICE) --package $(PACKAGE) --speed 6 \
		--lpf $(LPF) --json $< --textcfg $@ --timing-allow-fail

$(B)/$(BOARD)-inst.bit: $(B)/$(BOARD)-inst.config tools/strip_bram_init.py
	$(PYTHON) tools/strip_bram_init.py $< $(B)/$(BOARD)-inst-packed.config
	$(ECPPACK) --compress --bootaddr 0 $(B)/$(BOARD)-inst-packed.config --bit $@

$(B)/machboot-$(BOARD)-install.bin: $(B)/$(BOARD)-inst.bit $(B)/fw.img $(B)/machboot-$(BOARD).bin tools/mkinstaller.py
	$(PYTHON) tools/mkinstaller.py $< $(B)/fw.img $(B)/machboot-$(BOARD).bin $@

installer: $(B)/machboot-$(BOARD)-install.bin

# -- make measure: what the menu costs on a 45F -----------------------------------

$(B)/$(BOARD)-45f.json: $(RTL) $(B)/bootrom.hex $(B)/font.hex
	$(YOSYS) -q -l $(B)/$(BOARD)-45f-synth.log $(DEFINES) -DMB_DIE_45F -p \
		"read_verilog $(DEFINES) -DMB_DIE_45F $(RTL); synth_ecp5 -abc9 -top $(TOP) -json $@"

$(B)/$(BOARD)-45f.config: $(B)/$(BOARD)-45f.json $(LPF)
	$(NEXTPNR) -q -l $(B)/$(BOARD)-45f-pnr.log --45k --package $(PACKAGE) --speed 6 \
		--lpf $(LPF) --json $< --textcfg $@ --timing-allow-fail

$(B)/$(BOARD)-45f.bit: $(B)/$(BOARD)-45f.config tools/strip_bram_init.py
	$(PYTHON) tools/strip_bram_init.py $< $(B)/$(BOARD)-45f-packed.config
	$(ECPPACK) --compress --bootaddr $(SLOT) $(B)/$(BOARD)-45f-packed.config --bit $@

measure: $(B)/machboot-$(BOARD).bin $(B)/$(BOARD)-45f.bit
	$(PYTHON) tools/measure.py $(B) $(BOARD)

# -- simulation and the PC build ------------------------------------------------------

sim: $(B)/bootrom.hex $(B)/font.hex $(B)/fw.img $(B)/bootrom-sram.hex $(B)/fw-ram.hex $(B)/fw-sechzig-ram.hex $(B)/machboot-$(BOARD).bin
	$(MAKE) -C sim YOSYS="$(YOSYS)"

host: $(B)/font.hex $(B)/fw.img
	$(MAKE) -C fw/host

# -- the RP2040 supervisor (Sergei, Mozart): docs/rp2040.md -------------------------
# make sv PICO_SDK_PATH=/path/to/pico-sdk   (1.5.x, with lib/tinyusb)

sv:
	cmake -S rp2040 -B $(B)/sv -DPICO_SDK_PATH=$(PICO_SDK_PATH) > $(B)/sv-cmake.log
	$(MAKE) -C $(B)/sv
	$(PYTHON) tools/mkuf2.py $(B)/sv/machboot-sv.uf2 $(B)/machboot-sv.uf2

# the supervisor with the menu for one module: hold BOOT, power on, copy it
# (docs/power-on.md). Needs make sv, and the module's SRAM build (make sechzig).
uf2: $(foreach b,$(SECHZIG),$(B)/machboot-$(b).uf2)

$(B)/machboot-%.uf2: $(B)/machboot-%-sram.bit $(B)/sv/machboot-sv.uf2 tools/mkuf2.py
	$(PYTHON) tools/mkuf2.py $(B)/sv/machboot-sv.uf2 $@ --menu $< \
		--board $$(python3 -c "b='$*';c=2 if b.startswith('sergei') else 3;print(hex(c|int(b[-1])<<8))") \
		--name "machboot $(MB_VERSION) $$(echo $* | sed 's/_ml/ ML/; s/^s/S/; s/^m/M/')"

# the XA-XD link on its own, in CXXRTL (tests/xlink_tb.cc)
CXXRTL_RT = $(shell $(PYTHON) -c "import yowasp_yosys,os;print(os.path.dirname(yowasp_yosys.__file__))" 2>/dev/null)/share/include/backends/cxxrtl/runtime
xlink-test: rtl/mb_xlink.v tests/xlink_tb.cc | $(B)
	$(YOSYS) -q -p "read_verilog rtl/mb_xlink.v; hierarchy -top mb_xlink; proc; flatten; opt_clean; write_cxxrtl -O6 $(B)/xlink_tb_rtl.cc"
	$(CXX) -std=c++14 -O2 -DCXXRTL_NDEBUG -I$(CXXRTL_RT) -I$(B) tests/xlink_tb.cc -o $(B)/xlink_tb
	$(B)/xlink_tb

sv-test:
	mkdir -p $(B)
	$(HOSTCC) -Wall -Wextra -Irp2040/src rp2040/tests/test_djtag.c rp2040/src/djtag.c -o $(B)/test_djtag
	$(B)/test_djtag
	$(HOSTCC) -Wall -Wextra -Wno-unused-parameter -DMB_HOST -Irp2040/src -Ifw rp2040/tests/test_xlink.c \
		rp2040/src/xlink.c rp2040/src/msc.c rp2040/src/cache.c fw/xlink.c -o $(B)/test_xlink
	$(B)/test_xlink
	$(HOSTCC) -Wall -Wextra -Irp2040/src rp2040/tests/test_ecp5.c rp2040/src/ecp5.c -o $(B)/test_ecp5
	$(B)/test_ecp5
	$(HOSTCC) -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -DMB_HOST -Irp2040/src -Ifw rp2040/tests/test_sv.c \
		rp2040/src/sv.c rp2040/src/ecp5.c rp2040/src/msc.c rp2040/src/xlink.c rp2040/src/store.c rp2040/src/cache.c fw/xlink.c -o $(B)/test_sv
	$(B)/test_sv
	$(HOSTCC) -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -DMB_HOST -Irp2040/src -Ifw rp2040/tests/test_systems.c \
		rp2040/src/sv.c rp2040/src/ecp5.c rp2040/src/msc.c rp2040/src/xlink.c rp2040/src/store.c rp2040/src/cache.c \
		fw/xlink.c -o $(B)/test_systems
	$(B)/test_systems

test: host sv-test xlink-test sim

flash: $(B)/machboot-$(BOARD).bin
	openFPGALoader -c dirtyJtag -f -o 0 $<

clean:
	rm -rf $(B)
	$(MAKE) -C sim clean
	$(MAKE) -C fw/host clean

# Everything a release carries, built from scratch for every board, then
# gathered into build/release/ (tools/mkrelease.py). Publish it with
# tools/gh-release.sh. Needs PICO_SDK_PATH for the RP2040 firmware.
release:
	$(MAKE) BOARD=lakritz all installer sram
	$(MAKE) BOARD=konfekt all installer sram
	$(MAKE) sechzig
	$(MAKE) sv uf2
	$(PYTHON) tools/mkrelease.py $(B)
