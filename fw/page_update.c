/*
 * machboot
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The Install page: the systems on the card, into the system slot (docs/install.md).
 * Part of the menu (fw/ui.c, fw/ui_int.h); on Lakritz and Konfekt an
 * overlay, loaded from flash when the page is shown (fw/ovl.h).
 */

#include "ui_int.h"

static const char *entry_status(const card_entry_t *ce)
{
	if (!ce->has_bit)
		return "not for this board";
	if (cfg.sys_name[0] && !strcmp_ci(ce->name, cfg.sys_name)) {
		if (!mb.sys_ok)
			return "install again";
		if (!strcmp_ci(ce->ver, cfg.sys_ver))
			return "installed";
		return "new";
	}
	return "";
}

void page_update(void)
{
	static const keyhint_t k[] = {
		{ G_UP G_DOWN, "Choose" }, { "Enter", "Install" }, { "F5", "Read the card again" }, { G_LEFT, "Menu" },
	};
	static const keyhint_t kb[] = { { "", "Please do not switch off" } };
	static const keyhint_t k2[] = { { "F5", "Read the card again" }, { G_LEFT, "Menu" } };
	char buf[64];
	card_entry_t *list[CARD_MAX];
	int n = update_list(list, CARD_MAX);

	page_items = 0;
	if (!card.sd_ok && !card.usb) {
		text_at(5, "No SD card.", C_WHITE);
		text_at(7, "Put the card in (or a USB flash drive with", C_GREY);
		text_at(8, "\\SYS\\GATEWARE on it) and press F5.", C_GREY);
		draw_keys(k2, 2);
		return;
	}
	if (!card.found && !card.usb) {
		text_at(5, "This card has no \\SYS\\GATEWARE folder.", C_WHITE);
		text_at(7, "Systems for machboot go in \\SYS\\GATEWARE\\<NAME>\\:", C_GREY);
		text_at(8, "a SYSTEM.CFG and one .BIT file per board.", C_GREY);
		draw_keys(k2, 2);
		return;
	}
	if (!n) {
		text_at(5, "There are no systems on the card.", C_WHITE);
		draw_keys(k2, 2);
		return;
	}

	text_at(5, card.usb ? (card.found ? "On the SD card and the USB drive:" : "On the USB drive:") : "On the SD card:", C_GREY);
	const char *labels[CARD_MAX];
	const char *rights[CARD_MAX];
	char (*lab)[44] = page_lab;
	for (int i = 0; i < n; i++) {
		if (list[i]->ver[0])
			mb_snprintf(lab[i], sizeof(lab[i]), "%s %s%s", list[i]->name, list[i]->ver, list[i]->usb ? "  (USB)" : "");
		else
			mb_snprintf(lab[i], sizeof(lab[i]), "%s%s", list[i]->name, list[i]->usb ? "  (USB)" : "");
		labels[i] = lab[i];
		rights[i] = entry_status(list[i]);
	}
	if (sel >= n)
		sel = n - 1;
	page_items = inst_running ? 0 : n;
	int y = draw_items(7, labels, rights, n, sel, 1) + 1;

	if (inst_running) {
		mb_snprintf(buf, sizeof(buf), "%s %s ...", inst_what, list[sel]->name);
		text_at(y, buf, C_WHITE);
		scr_bar(CX, y + 2, 40, inst_permille, C_LGREEN, C_BLUE);
		mb_snprintf(buf, sizeof(buf), "%d %%", inst_permille / 10);
		text_at(y + 2, "", C_WHITE);
		scr_put(CX + 42, y + 2, buf, C_WHITE, C_BLUE);
		draw_keys(kb, 1);
		return;
	}
	if (inst_msg[0]) {
		text_at(y, inst_msg, inst_msg_ok ? C_LGREEN : C_LRED);
		y += 2;
	}
	card_entry_t *ce = list[sel];
	if (!ce->has_bit) {
		mb_snprintf(buf, sizeof(buf), "It has no %s for this board.", board_file(mb.board));
		text_at(y, buf, C_GREY);
	} else if (mb.install_mode) {
		text_at(y, "Install machboot first (Start); this system is", C_GREY);
		text_at(y + 1, "installed with it.", C_GREY);
	} else if (!can_install() && ((mb.board & 0xff) == 2 || (mb.board & 0xff) == 3)) {
		/* Sergei, Mozart: the module's flash is written through the RP2040 */
		text_at(y, "On this board, installing comes with the RP2040", C_GREY);
		text_at(y + 1, "supervisor's next version. Start runs what is", C_GREY);
		text_at(y + 2, "in the module's flash.", C_GREY);
	} else if (!can_install()) {
		text_at(y, "Installing needs machboot installed at the start", C_GREY);
		text_at(y + 1, "of the flash (not the JTAG test build).", C_GREY);
	} else if (mb.sys_ok && cfg.sys_name[0] && strcmp_ci(ce->name, cfg.sys_name)) {
		mb_snprintf(buf, sizeof(buf), "%s replaces %s.", ce->name, cfg.sys_name);
		text_at(y, buf, C_GREY);
		text_at(y + 1, "The Zeitlos region in flash is not touched.", C_GREY);
	} else {
		mb_snprintf(buf, sizeof(buf), "%u KB, into the system slot.", (ce->bit.size + 1023) / 1024);
		text_at(y, buf, C_GREY);
		text_at(y + 1, "The Zeitlos region in flash is not touched.", C_GREY);
	}
	if (details) {
		mb_snprintf(buf, sizeof(buf), "Card        partition %d, FAT%d", card.vol->part, card.vol->type);
		text_at(y + 4, buf, C_DGREY);
		mb_snprintf(buf, sizeof(buf), "File        \\SYS\\GATEWARE\\%s\\%s", ce->dir, ce->has_bit ? ce->bit.name : "-");
		text_at(y + 5, buf, C_DGREY);
	}
	draw_keys(k, 4);
}

