/*
 * machboot
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * What the firmware knows about the machine it runs on.
 */

#ifndef MB_MB_H
#define MB_MB_H

#include <stdint.h>
#include "flash.h"

#define MB_VERSION_STR	"0.4"
#define MB_VERSION	0x000400

/* boot-slot layout (docs/flash-layout.md) */
#define BOOT_SLOT	0x000000u
#define BOOT_SLOT_SIZE	0x060000u
#define FW_OFFSET	0x050000u	/* firmware blob, inside the boot slot */
#define SYSTEM_SLOT	0x060000u
#define ZEITLOS_REGION	0x100000u
#define TINYDFU_USER	0x040000u	/* where tinydfu's user alt writes */
#define SETTINGS_AT	0x05E000u	/* the settings sectors, inside the boot slot */

typedef struct {
	uint32_t board;
	uint32_t hwver;
	uint32_t idcode;
	uint32_t features;
	uint32_t slot;		/* system slot this bitstream boots */
	uint32_t fw_base;	/* where the boot ROM found the firmware */
	int install_mode;	/* running from tinydfu's user area */

	flash_info_t flash;
	slot_info_t sys;
	int sys_ok;		/* a bitstream for this FPGA is in the slot */
	int zeitlos_region;	/* a Zeitlos kernel is at 0x100000 */
	char sysname[24];

	int usb;		/* the USB host is in the gateware */
} mb_t;

extern mb_t mb;

void mb_probe(uint32_t fw_base);
void mb_probe_system(void);	/* the system slot again, e.g. after an install */
const char *board_name(uint32_t board);

/* ui.c */
enum { UI_BOOT, UI_MENU, UI_UPDATE };
int ui_startup(void);		/* UI_BOOT, or where to open the menu */
void ui_menu(int where);
void ui_boot(void);		/* does not return if it works */

/* diag.c */
int diag_memory_run(void (*progress)(int permille));
int diag_flash_run(uint32_t *kbps);
int diag_sd_run(uint32_t *kbps);

#endif
