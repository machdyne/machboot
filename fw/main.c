/*
 * machboot
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The firmware's entry: the boot ROM (bootrom/boot.c) has copied it
 * into RAM and passes the flash address it found it at.
 */

#include "mb.h"
#include "hw.h"
#include "lib.h"
#include "screen.h"
#include "console.h"
#include "settings.h"
#include "usb.h"
#include "card.h"
#include "selfinst.h"
#include "usbd.h"
#ifdef MB_SYSTEMS
#include "systems.h"
#endif

mb_t mb;

/* BOARD: the carrier in bits 7:0, the Sechzig module (ML0/1/2) in 15:8 */
const char *board_name(uint32_t board)
{
	static char s[16];
	uint32_t m = (board >> 8) & 0xff;
	if ((board & 0xff) == 2 || (board & 0xff) == 3) {
		mb_snprintf(s, sizeof(s), "%s ML%u", (board & 0xff) == 2 ? "Sergei" : "Mozart", m);
		return s;
	}
	switch (board & 0xff) {
	case 1: return "Lakritz";
	case 2: return "Sergei";
	case 3: return "Mozart";
	case 4: return "Obst";
	case 5: return "Konfekt";
	case 6: return "Minze";
	case 7: return "Schoko";
	case 8: return "Noir";
	case 9: return "Klinge";
	default: return "board";
	}
}

void mb_probe_system(void)
{
	flash_slot(mb.slot, &mb.sys);

	/*
	 * Running from tinydfu's user area, the system slot lies inside
	 * this very image (0x040000-0x09FFFF): whatever is there is not a
	 * system, whatever it looks like.
	 */
	int inside_self = mb.install_mode &&
		mb.slot >= TINYDFU_USER && mb.slot < TINYDFU_USER + BOOT_SLOT_SIZE;

	mb.sys_ok = mb.sys.valid && !inside_self &&
		(mb.sys.idcode == 0 || mb.sys.idcode == mb.idcode);

	uint32_t k = flash_rd32(ZEITLOS_REGION);
	mb.zeitlos_region = (k != 0xffffffffu && k != 0);

	/* the name: what the last install recorded, else a guess */
	memset(mb.sysname, 0, sizeof(mb.sysname));
	if (cfg.sys_name[0])
		memcpy(mb.sysname, cfg.sys_name, strlen(cfg.sys_name));
	else if (mb.zeitlos_region)
		memcpy(mb.sysname, "Zeitlos", 7);
	else
		memcpy(mb.sysname, "System", 6);
}

void mb_probe(uint32_t fw_base)
{
	memset(&mb, 0, sizeof(mb));
	mb.board = sys_rd(SYS_BOARD);
	mb.hwver = sys_rd(SYS_HWVER);
	mb.idcode = sys_rd(SYS_IDCODE);
	mb.features = sys_rd(SYS_FEATURES);
	mb.slot = sys_rd(SYS_SLOT);
	mb.fw_base = fw_base;
	mb.install_mode = (fw_base == TINYDFU_USER + FW_OFFSET);

	flash_identify(&mb.flash);

	/* settings are written only when machboot is installed at 0 */
	settings_load(fw_base == FW_OFFSET);
	kbd_layout = cfg.kbd;
	selfinst_probe();

	mb_probe_system();
	mb.usb = (mb.features & FEAT_USBH) && usb_init();
	usbd_init();
	mb_probe_system();		/* Sergei: the settings came from the RP2040 just now */
	card_scan();
#ifdef MB_SYSTEMS
	if (systems_on())
		systems_refresh();
#endif
}

int main(uint32_t fw_base)
{
	con_init();

	if (sys_rd(SYS_MAGIC) != SYS_MAGIC_VAL || sys_rd(SYS_HWVER) != MB_HWVER) {
		con_puts("machboot: this firmware does not match the gateware\n");
		for (;;)
			;
	}

	mb_probe(fw_base);

	int r = ui_startup();
	if (r == UI_BOOT)
		ui_boot();
	ui_menu(r);
	return 0;
}
