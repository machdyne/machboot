/*
 * machboot
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The Start page: the installed system (Lakritz, Konfekt; fw/ui_systems.inc has
 * the list of systems of Sergei and Mozart).
 * Part of the menu (fw/ui.c, fw/ui_int.h); on Lakritz and Konfekt an
 * overlay, loaded from flash when the page is shown (fw/ovl.h).
 */

#include "ui_int.h"

void page_start(void)
{
	static const keyhint_t k[] = {
		{ G_UP G_DOWN, "Choose" }, { "Enter", "Start" }, { G_LEFT, "Menu" }, { "F1", "Details" },
	};
	char buf[64];

	if (!mb.sys_ok) {
		static const keyhint_t k2[] = { { G_LEFT, "Menu" }, { "F1", "Details" } };
		static const keyhint_t k3[] = { { "Enter", "Do it" }, { G_LEFT, "Menu" }, { "F1", "Details" } };
		card_entry_t *again = card_copy_of_installed();
		page_items = 0;
		if (cfg.sys_name[0] && !mb.sys.valid) {
			scr_fill(CX - 1, 5, CW + 2, 1, C_YELLOW, C_RED, ' ');
			mb_snprintf(buf, sizeof(buf), "%s did not finish installing.", cfg.sys_name);
			scr_put(CX, 5, buf, C_YELLOW, C_RED);
			if (again && can_install()) {
				mb_snprintf(buf, sizeof(buf), "Install %s again", cfg.sys_name);
				const char *labels[1] = { buf };
				const char *rights[1] = { "from card" };
				page_items = 1;
				int y = draw_items(7, labels, rights, 1, sel, 1);
				text_at(y + 1, "Nothing else was changed.", C_GREY);
				draw_keys(k3, 3);
			} else {
				text_at(7, "Nothing else was changed. Put the SD card with", C_GREY);
				mb_snprintf(buf, sizeof(buf), "%s on it in, and choose Install.", cfg.sys_name);
				text_at(8, buf, C_GREY);
				draw_keys(k2, 2);
			}
			return;
		} else if (mb.sys.valid) {
			scr_fill(CX - 1, 5, CW + 2, 1, C_YELLOW, C_RED, ' ');
			scr_put(CX, 5, "The system in flash is for another FPGA.", C_YELLOW, C_RED);
			mb_snprintf(buf, sizeof(buf), "It was built for the %s.", die_name(mb.sys.idcode));
			text_at(7, buf, C_WHITE);
			mb_snprintf(buf, sizeof(buf), "This board has the %s.", die_name(mb.idcode));
			text_at(8, buf, C_WHITE);
		} else {
			text_at(5, "No system is installed yet.", C_WHITE);
			if (card.n && can_install())
				text_at(7, "Install one from the SD card: choose Install.", C_GREY);
			else
				text_at(7, "Put one on the SD card under \\SYS\\GATEWARE.", C_GREY);
		}
		if (details) {
			mb_snprintf(buf, sizeof(buf), "System slot 0x%06x: %s", mb.slot,
			            mb.sys.valid ? "bitstream" : "no bitstream");
			text_at(12, buf, C_DGREY);
		}
		draw_keys(k2, 2);
		return;
	}

	char name[48];
	if (cfg.sys_ver[0] && cfg.sys_name[0])
		mb_snprintf(name, sizeof(name), "%s %s", mb.sysname, cfg.sys_ver);
	else
		mb_snprintf(name, sizeof(name), "%s", mb.sysname);
	const char *labels[1] = { name };
	const char *rights[1] = { cfg.autostart ? "default" : "" };
	page_items = 1;
	int y = draw_items(5, labels, rights, 1, sel, 1);
	if (cfg.autostart)
		mb_snprintf(buf, sizeof(buf), "%s starts by itself after %d seconds.", mb.sysname, cfg.wait_s);
	else
		mb_snprintf(buf, sizeof(buf), "Nothing starts by itself (Settings).");
	text_at(y + 1, buf, C_GREY);

	if (details) {
		y += 4;
		mb_snprintf(buf, sizeof(buf), "Slot        0x%06x%s", mb.slot, mb.sys.has_header ? " (.bit header)" : "");
		text_at(y++, buf, C_DGREY);
		mb_snprintf(buf, sizeof(buf), "Built for   %s (%08x)", die_name(mb.sys.idcode), mb.sys.idcode);
		text_at(y++, buf, C_DGREY);
		if (mb.sys.part[0]) {
			mb_snprintf(buf, sizeof(buf), "Part        %s", mb.sys.part);
			text_at(y++, buf, C_DGREY);
		}
		if (cfg.sys_size) {
			mb_snprintf(buf, sizeof(buf), "Installed   %u bytes, CRC-32 %08x", cfg.sys_size, cfg.sys_crc);
			text_at(y++, buf, C_DGREY);
		}
		mb_snprintf(buf, sizeof(buf), "Zeitlos     %s", mb.zeitlos_region ? "kernel at 0x100000" : "no kernel at 0x100000");
		text_at(y++, buf, C_DGREY);
	}
	draw_keys(k, 4);
}

