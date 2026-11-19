/*
 * machboot
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The Board page.
 * Part of the menu (fw/ui.c, fw/ui_int.h); on Lakritz and Konfekt an
 * overlay, loaded from flash when the page is shown (fw/ovl.h).
 */

#include "ui_int.h"

void page_board(void)
{
	static const keyhint_t k[] = { { G_LEFT, "Menu" }, { "F1", "Details" } };
	char buf[64];
	int y = 5;
	page_items = 0;

	kv_at(y, "Board", board_name(mb.board)); y += 2;
	kv_at(y, "FPGA", die_name(mb.idcode)); y += 2;
	if (mb.flash.size)
		mb_snprintf(buf, sizeof(buf), "%u MB", mb.flash.size >> 20);
	else
		mb_snprintf(buf, sizeof(buf), "unknown");
	kv_at(y, "Flash", buf); y += 2;
	kv_at(y, "Video", "640x480"); y += 2;
	kv_at(y, "System", mb.sys_ok ? mb.sysname : "none"); y += 2;
	if (card.sd_ok && sd.sectors)
		mb_snprintf(buf, sizeof(buf), "%u MB, %d partition%s", sd.sectors / 2048, card.vols, card.vols == 1 ? "" : "s");
	else
		mb_snprintf(buf, sizeof(buf), card.sd_ok ? "present" : "none");
	kv_at(y, "SD card", buf); y += 2;
	if (!mb.usb) {
		kv_at(y, "USB", "no USB host in this gateware"); y += 1;
	}
	usb_dev_info_t di;
	for (int n = 0; mb.usb && usb_dev_info(n, &di); n++) {
		static const char *const kinds[] = { "-", "starting ...", "keyboard", "not used", "hub",
		                                     "not answering", "flash drive", "no room for it" };
		char kl[12];
		if (di.depth)
			mb_snprintf(kl, sizeof(kl), "  USB %d.%d", di.root + 1, di.hub_port);
		else
			mb_snprintf(kl, sizeof(kl), "USB %d", di.root + 1);
		if (di.kind == USB_NONE || di.kind == USB_BUSY || di.kind == USB_FAILED)
			mb_snprintf(buf, sizeof(buf), "%s", kinds[di.kind]);
		else if (di.kind == USB_DRIVE && di.sectors)
			mb_snprintf(buf, sizeof(buf), "%s  %04x:%04x  %u MB", kinds[di.kind], di.vid, di.pid, di.sectors / 2048);
		else
			mb_snprintf(buf, sizeof(buf), "%s  %04x:%04x%s", kinds[di.kind], di.vid, di.pid,
			            di.lowspeed ? "  low speed" : "");
		kv_at(y, kl, buf); y += 1;
	}
	if (usbd.present) {
		mb_snprintf(buf, sizeof(buf), "%s%s", usbd.configured ? "console, DFU" : "not connected",
		            usbd.cdc_open ? " (terminal open)" : "");
		kv_at(y, "USB-C", buf); y += 1;
	}
	y += 1;
	mb_snprintf(buf, sizeof(buf), "%s", MB_VERSION_STR);
	kv_at(y, "machboot", buf); y += 2;

	if (details) {
		y++;
		mb_snprintf(buf, sizeof(buf), "IDCODE      %08x", mb.idcode);
		text_at(y++, buf, C_DGREY);
		mb_snprintf(buf, sizeof(buf), "JEDEC ID    %06x", mb.flash.jedec);
		text_at(y++, buf, C_DGREY);
		if (mb.fw_base == 0)
			mb_snprintf(buf, sizeof(buf), "Firmware    in the bitstream (JTAG test build)");
		else
			mb_snprintf(buf, sizeof(buf), "Firmware    found at 0x%06x%s", mb.fw_base, mb.install_mode ? " (via tinydfu)" : "");
		text_at(y++, buf, C_DGREY);
		mb_snprintf(buf, sizeof(buf), "Boots       0x%06x (PROGRAMN)", mb.slot);
		text_at(y++, buf, C_DGREY);
		mb_snprintf(buf, sizeof(buf), "Hardware    v%u, features %02x", mb.hwver, mb.features);
		text_at(y++, buf, C_DGREY);
	}
	draw_keys(k, 2);
}

/* ------------------------------------------------------------------------- */
/* menu loop                                                                 */
/* ------------------------------------------------------------------------- */

/* dfu-util at work, or just done: one line above the keys, on every page */
