# The firmware built for the PC (hw_host.c instead of the hardware),
# driven by the scripts in tests/, its screens rendered to PNG.
CC = cc
CFLAGS = -O1 -g -Wall -Wno-unused-parameter -DMB_HOST -std=gnu11 -Dmain=main_fw -I..
SRC = ../main.c ../ui.c ../screen.c ../console.c ../flash.c ../diag.c ../lib.c \
      ../usb.c ../kvlog.c ../settings.c ../sd.c ../fat.c ../card.c ../install.c ../selfinst.c ../usbd.c \
      ../page_start.c ../page_update.c ../page_card.c ../page_settings.c ../page_diag.c ../page_board.c ../page_instmode.c
B = ../../build/host

.PHONY: all clean
SRC_SECHZIG = $(filter-out ../usbd.c ../selfinst.c ../install.c,$(SRC)) ../xlink.c ../selfinst_none.c ../install_none.c ../systems.c \
      ../ui_systems.c ../page_systems.c ../page_sysinst.c

all: $(B)/mbhost $(B)/mbhost-sechzig $(B)/mbhost-slot
	cd ../.. && python3 tests/run_host.py

$(B)/mbhost: hw_host.c usbh_model.inc usbd_host.c xlink_host.c ../../rp2040/src/cache.c ../../rp2040/src/cache.h $(SRC) ../*.h ../../tests/sdmodel.c ../../tests/sdmodel.h
	mkdir -p $(B)
	$(CC) $(CFLAGS) -c hw_host.c -o $(B)/hw_host.o -Umain
	$(CC) $(CFLAGS) -c usbd_host.c -o $(B)/usbd_host.o -Umain
	$(CC) $(CFLAGS) -c ../../tests/sdmodel.c -o $(B)/sdmodel.o
	$(CC) $(CFLAGS) -c xlink_host.c -o $(B)/xlink_host.o
	$(CC) $(CFLAGS) -c ../../rp2040/src/cache.c -o $(B)/cache.o
	$(CC) $(CFLAGS) $(SRC) $(B)/hw_host.o $(B)/usbd_host.o $(B)/xlink_host.o $(B)/cache.o $(B)/sdmodel.o -o $@

# Sergei and Mozart: the firmware with fw/xlink.c in place of fw/usbd.c
$(B)/mbhost-sechzig: $(B)/mbhost $(SRC_SECHZIG) ../*.h
	$(CC) $(CFLAGS) -DMB_SYSTEMS $(SRC_SECHZIG) $(B)/hw_host.o $(B)/usbd_host.o $(B)/xlink_host.o $(B)/cache.o $(B)/sdmodel.o -o $@

clean:
	rm -rf $(B)

# Lakritz and Konfekt as installed: the list of systems with one slot
# (docs/systems.md); mbhost is the JTAG test build's pages
SRC_SLOT = $(filter-out ../page_start.c ../page_update.c,$(SRC)) ../systems.c ../ui_systems.c ../page_systems.c ../page_sysinst.c
$(B)/mbhost-slot: $(B)/mbhost $(SRC_SLOT) ../*.h
	$(CC) $(CFLAGS) -DMB_SYSTEMS -DMB_SLOT $(SRC_SLOT) $(B)/hw_host.o $(B)/usbd_host.o $(B)/xlink_host.o $(B)/cache.o $(B)/sdmodel.o -o $@
