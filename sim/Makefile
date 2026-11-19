# Whole-system simulation with CXXRTL: boot ROM, firmware from a
# modelled flash, the menu over a modelled serial port, the screen
# decoded from the TMDS lanes. Run from the top: make sim.
YOSYS ?= yowasp-yosys
CXX ?= g++
CXXRTL_INC ?= $(shell python3 -c "import yowasp_yosys,os;print(os.path.join(os.path.dirname(yowasp_yosys.__file__),'share/include/backends/cxxrtl/runtime'))" 2>/dev/null || echo $$(yosys-config --datdir)/include/backends/cxxrtl/runtime)
B = ../build/sim
# 80 KB of RAM is not a power of two: CXXRTL checks every read port
# address, also those the RAM ignores (rtl/mb_ram.v, in_mem)
SIMFLAGS = -DCXXRTL_NDEBUG

RTL = sim/sim_top.v sim/ecp5_sim.v rtl/mb_soc.v rtl/mb_ram.v rtl/mb_text.v rtl/mb_tmds.v rtl/mb_dvi.v \
      rtl/mb_sysctl.v rtl/zeitlos/spiflash.v rtl/zeitlos/uart.v rtl/zeitlos/spim.v \
      rtl/cpu/zeitlos32/zeitlos32.v rtl/cpu/zeitlos32/zeitlos32_decode.v \
      rtl/cpu/zeitlos32/zeitlos32_alu.v rtl/cpu/zeitlos32/zeitlos32_lsu.v \
      rtl/cpu/zeitlos32/zeitlos32_muldiv.v \
      rtl/mb_usbd.v rtl/ulixxe/sie.v rtl/ulixxe/phy_rx.v rtl/ulixxe/phy_tx.v

.PHONY: all clean
all: $(B)/sim $(B)/flash.bin $(B)/sim-sram $(B)/blank.bin $(B)/card.img $(B)/sim-inst $(B)/inst-flash.bin $(B)/sim-xl $(B)/sechzig-flash.bin
	cd .. && build/sim/sim build/sim/flash.bin build/sim
	cd .. && build/sim/sim-sram build/sim/blank.bin build/sim --sram
	cd .. && build/sim/sim build/sim/flash.bin build/sim --install build/sim/card.img build/sim/small.bit
	cd .. && build/sim/sim build/sim/flash.bin build/sim --usb build/sim/small.bit build/sim/card.img
	cd .. && build/sim/sim-inst build/sim/inst-flash.bin build/sim --selfinstall build/sim/card.img build/sim/small.bit build/machboot-lakritz.bin
	cd .. && build/sim/sim-xl build/sim/sechzig-flash.bin build/sim --xlink build/sim/card.img
	cd .. && python3 tests/check_sim.py

# a FAT12 card with a small Zeitlos "bitstream" for this FPGA (the menu
# checks only its start: header, preamble, VERIFY_ID)
$(B)/card.img: ../tests/mkcard.py
	mkdir -p $(B)
	cd .. && python3 -c "import sys,struct;sys.path.insert(0,'tests');import run_host as r;open('build/sim/small.bit','wb').write(r.bitfile(0x41111043,9000,7))"
	cd .. && python3 tests/mkcard.py build/sim/card.img fat12-flat SYS/GATEWARE/ZEITLOS/LAKRITZ.BIT=build/sim/small.bit "SYS/GATEWARE/ZEITLOS/SYSTEM.CFG=@name=Zeitlos;version=0.0.9"

$(B)/sim-sram.cc: $(addprefix ../,$(RTL)) ../build/bootrom-sram.hex ../build/fw-ram.hex ../build/font.hex
	mkdir -p $(B)
	cd .. && $(YOSYS) -q -l build/sim/yosys-sram.log -p "read_verilog -DMB_USB_DEV -DMB_SRAM_TEST $(RTL); hierarchy -top sim_top; proc; flatten; tribuf -formal; opt_clean; write_cxxrtl -O6 build/sim/sim-sram.cc"

$(B)/sim-sram: sim_main.cc usbhost.inc $(B)/sim-sram.cc
	sed 's/#include "sim.cc"/#include "sim-sram.cc"/' sim_main.cc > $(B)/sim_main_sram.cc
	$(CXX) -std=c++14 -O3 $(SIMFLAGS) -I. -I$(CXXRTL_INC) -I$(B) -I../tests $(B)/sim_main_sram.cc ../tests/sdmodel.c -o $@

# install mode: the installer gateware (fence turned around), on a flash
# with a stand-in tinydfu at 0 and the installer image at 0x040000
$(B)/sim-inst.cc: $(addprefix ../,$(RTL)) ../build/bootrom.hex ../build/font.hex
	mkdir -p $(B)
	cd .. && $(YOSYS) -q -l build/sim/yosys-inst.log -p "read_verilog -DMB_USB_DEV -DMB_INSTALLER $(RTL); hierarchy -top sim_top; proc; flatten; tribuf -formal; opt_clean; write_cxxrtl -O6 build/sim/sim-inst.cc"

$(B)/sim-inst: sim_main.cc usbhost.inc $(B)/sim-inst.cc ../tests/sdmodel.c ../tests/sdmodel.h
	sed 's/#include "sim.cc"/#include "sim-inst.cc"/' sim_main.cc > $(B)/sim_main_inst.cc
	$(CXX) -std=c++14 -O3 $(SIMFLAGS) -I. -I$(CXXRTL_INC) -I$(B) -I../tests $(B)/sim_main_inst.cc ../tests/sdmodel.c -o $@

$(B)/inst-flash.bin: ../build/machboot-lakritz.bin ../build/fw.img ../tools/mkinstaller.py ../tests/mkinstflash.py
	mkdir -p $(B)
	cd .. && python3 tests/mkinstflash.py build/sim/inst-flash.bin build/fw.img build/machboot-lakritz.bin

# Sergei ML2 (rtl/top_sechzig.v's parameters), the Sechzig firmware in RAM,
# the XA-XD link driven by the RP2040's own code (sim/xlink.inc)
$(B)/sim-xl.cc: $(addprefix ../,$(RTL)) ../rtl/mb_xlink.v ../build/bootrom-sram.hex ../build/fw-sechzig-ram.hex ../build/font.hex
	mkdir -p $(B)
	cd .. && $(YOSYS) -q -l build/sim/yosys-xl.log -p "read_verilog -DMB_XLINK -DMB_SIM_SECHZIG $(RTL) rtl/mb_xlink.v; hierarchy -top sim_top; proc; flatten; tribuf -formal; opt_clean; write_cxxrtl -O6 build/sim/sim-xl.cc"

$(B)/sim-xl: sim_main.cc usbhost.inc xlink.inc $(B)/sim-xl.cc ../rp2040/src/xlink.c ../rp2040/src/msc.c ../rp2040/src/cache.c
	sed 's/#include "sim.cc"/#include "sim-xl.cc"/' sim_main.cc > $(B)/sim_main_xl.cc
	$(CC) -O2 -c -I../rp2040/src ../rp2040/src/xlink.c -o $(B)/rp_xlink.o
	$(CC) -O2 -c -I../rp2040/src ../rp2040/src/msc.c -o $(B)/rp_msc.o
	$(CC) -O2 -c -I../rp2040/src ../rp2040/src/cache.c -o $(B)/rp_cache.o
	$(CXX) -std=c++14 -O3 $(SIMFLAGS) -DMB_SIM_XLINK -I. -I$(CXXRTL_INC) -I$(B) -I../tests -I../rp2040/src $(B)/sim_main_xl.cc ../tests/sdmodel.c $(B)/rp_xlink.o $(B)/rp_msc.o $(B)/rp_cache.o -o $@

# a Sechzig module's flash: a system for the 45F at 0 (what Start boots)
$(B)/sechzig-flash.bin: ../tests/run_host.py
	mkdir -p $(B)
	cd .. && python3 -c "import sys;sys.path.insert(0,'tests');import run_host as r;b=r.bitfile(0x41112043,9000,7);f=bytearray(b'\\xff'*(2<<20));f[:len(b)]=b;open('build/sim/sechzig-flash.bin','wb').write(f)"

$(B)/blank.bin:
	mkdir -p $(B)
	python3 -c "open('$(B)/blank.bin','wb').write(b'\\xff' * (2 << 20))"

$(B)/sim.cc: $(addprefix ../,$(RTL)) ../build/bootrom.hex ../build/font.hex
	mkdir -p $(B)
	cd .. && $(YOSYS) -q -l build/sim/yosys.log -p "read_verilog -DMB_USB_DEV $(RTL); hierarchy -top sim_top; proc; flatten; tribuf -formal; opt_clean; write_cxxrtl -O6 build/sim/sim.cc"

$(B)/sim: sim_main.cc usbhost.inc $(B)/sim.cc ../tests/sdmodel.c ../tests/sdmodel.h
	$(CXX) -std=c++14 -O3 $(SIMFLAGS) -I. -I$(CXXRTL_INC) -I$(B) -I../tests sim_main.cc ../tests/sdmodel.c -o $@

$(B)/flash.bin: ../build/machboot-lakritz.bin ../build/lakritz.bit
	mkdir -p $(B)
	printf '\x13\x00\x00\x00' > $(B)/region.bin
	cd .. && python3 tools/mkimage.py build/sim/flash.bin --boot build/machboot-lakritz.bin --system build/lakritz.bit --region build/sim/region.bin

clean:
	rm -rf $(B)
