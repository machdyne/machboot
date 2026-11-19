/*
 * machboot
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The Install page with the list of systems (fw/systems.h,
 * docs/systems.md): the card's systems, into the list -- and into flash
 * where that is the point: on Sergei and Mozart always (the RP2040's
 * flash keeps several); on Lakritz and Konfekt only an update of the
 * system in the slot, or the first one into an empty slot. Everything
 * else is copied when it is started, and the page says so.
 * Part of the menu (fw/ui.c, fw/ui_int.h); on Lakritz and Konfekt an
 * overlay, loaded from flash when the page is shown (fw/ovl.h).
 */

#include "ui_int.h"
#include "systems.h"

void page_install(void)
{
	static const keyhint_t k[] = {
		{ G_UP G_DOWN, "Choose" }, { "Enter", "Install" }, { "F5", "Read the card again" }, { G_LEFT, "Menu" },
	};
	static const keyhint_t kb[] = { { "Esc", "Cancel" } };
	static const keyhint_t kw[] = { { "", "Please do not switch off" } };
	static const keyhint_t k2[] = { { "F5", "Read the card again" }, { G_LEFT, "Menu" } };
	char buf[96];
	card_entry_t *list[CARD_MAX];
	int n = update_list(list, CARD_MAX);

	page_items = 0;
	if ((!card.sd_ok && !card.usb) || (!card.found && !card.usb) || !n) {
		text_at(5, !card.sd_ok && !card.usb ? "No SD card." : "There are no systems on the card.", C_WHITE);
		text_at(7, "Systems for machboot go in \\SYS\\GATEWARE\\<NAME>\\:", C_GREY);
		text_at(8, "a SYSTEM.CFG and one .BIT file per board. Then F5.", C_GREY);
		draw_keys(k2, 2);
		return;
	}

	text_at(5, card.usb ? (card.found ? "On the SD card and the USB drive:" : "On the USB drive:") : "On the SD card:", C_GREY);
	const char *labels[CARD_MAX], *rights[CARD_MAX];
	char (*lab)[44] = page_lab;
	for (int i = 0; i < n; i++) {
		const sysent_t *e = systems_find(list[i]->name);
		mb_snprintf(lab[i], sizeof(lab[i]), list[i]->ver[0] ? "%s %s%s" : "%s%s%s", list[i]->name,
		            list[i]->ver[0] ? list[i]->ver : (list[i]->usb ? "  (USB)" : ""), list[i]->ver[0] && list[i]->usb ? "  (USB)" : "");
		labels[i] = lab[i];
		rights[i] = !list[i]->has_bit ? "not for this board" :
		            !e ? "" :
		            (e->module || e->cached) && strcmp_ci(e->ver, list[i]->ver) ? "new" :
		            e->module || e->cached ? "installed" : "in the list";
	}
	if (sel >= n)
		sel = n - 1;
	page_items = inst_running ? 0 : n;
	int y = draw_items(7, labels, rights, n, sel, 1) + 1;
	card_entry_t *ce = list[sel];
	const sysent_t *e = systems_find(ce->name);

	if (inst_running) {
#ifdef MB_SLOT
		int writing = inst_permille >= 300;
		mb_snprintf(buf, sizeof(buf), writing ? "Copying %s from %s into flash ..." : "Comparing %s on %s with flash ...",
		            ce->name, src_name(ce));
#else
		int writing = 0;
		mb_snprintf(buf, sizeof(buf), "Copying %s from %s into flash ...", ce->name, src_name(ce));
#endif
		text_at(y, buf, C_WHITE);
		scr_bar(CX, y + 2, 40, inst_permille, C_LGREEN, C_BLUE);
		mb_snprintf(buf, sizeof(buf), "%d %%", inst_permille / 10);
		scr_put(CX + 42, y + 2, buf, C_WHITE, C_BLUE);
		draw_keys(writing ? kw : kb, 1);
		return;
	}
	if (inst_msg[0]) {
		text_at(y, inst_msg, inst_msg_ok ? C_LGREEN : C_LRED);
		y += 2;
	}
	int in_flash = e && (e->module || e->cached);
	if (!ce->has_bit) {
		mb_snprintf(buf, sizeof(buf), "It has no %s for this board.", board_file(mb.board));
		text_at(y, buf, C_GREY);
	} else if (in_flash && !strcmp_ci(e->ver, ce->ver)) {
		mb_snprintf(buf, sizeof(buf), "%s %s is installed: start it from Start.", ce->name, ce->ver);
		text_at(y, buf, C_GREY);
	} else if (!e && sys_n >= SYS_MAX) {
		mb_snprintf(buf, sizeof(buf), "The list is full (%d): remove one first.", SYS_MAX);
		text_at(y, buf, C_GREY);
#ifdef MB_SLOT
	} else if (!systems_slot_writes(ce)) {
		/* into the list only: nothing is written now */
		if (e) {
			mb_snprintf(buf, sizeof(buf), "%s is in Start's list. It is copied into", ce->name);
			text_at(y++, buf, C_WHITE);
			text_at(y++, "flash when it is started.", C_WHITE);
		} else {
			mb_snprintf(buf, sizeof(buf), "Enter adds %s %s to Start's list.", ce->name, ce->ver);
			text_at(y++, buf, C_WHITE);
			text_at(y++, "Nothing is written now: it is copied into flash", C_GREY);
			text_at(y++, "when it is started, so keep the card at hand.", C_GREY);
		}
#endif
	} else {
		mb_snprintf(buf, sizeof(buf), "Enter copies %s %s from %s into flash", ce->name, ce->ver, src_name(ce));
		text_at(y++, buf, C_WHITE);
		if (in_flash)
			mb_snprintf(buf, sizeof(buf), "(about %u s), in place of %s.", systems_copy_s(ce->bit.size), e->ver[0] ? e->ver : "the copy there");
		else
			mb_snprintf(buf, sizeof(buf), "(about %u s), and adds it to Start's list.", systems_copy_s(ce->bit.size));
#ifdef MB_SLOT
		text_at(y++, buf, C_WHITE);
		text_at(y++, "The Zeitlos region is not touched.", C_GREY);
#else
		y = copy_hint(y, buf, ce, e ? e->crc : 0);
#endif
	}
	if (details) {
		y += 2;
		mb_snprintf(buf, sizeof(buf), "File        \\SYS\\GATEWARE\\%s\\%s, %u bytes", ce->dir, ce->has_bit ? ce->bit.name : "-", ce->bit.size);
		text_at(y++, buf, C_DGREY);
#ifndef MB_SLOT
		mb_snprintf(buf, sizeof(buf), "RP2040 flash %u of %u KB free", sys_rc.free / 1024, sys_rc.size / 1024);
		text_at(y++, buf, C_DGREY);
#endif
	}
	draw_keys(k, 4);
}

static void install_entry(card_entry_t *ce)
{
	const sysent_t *e = systems_find(ce->name);
	int in_flash = e && (e->module || e->cached);
	if (!ce->has_bit || (in_flash && !strcmp_ci(e->ver, ce->ver)) || (!e && sys_n >= SYS_MAX))
		return;
#ifdef MB_SLOT
	if (!systems_slot_writes(ce)) {
		if (e)
			return;
		int r = systems_add(ce, 0);	/* a settings write, no more */
		if (r == XL_R_OK)
			mb_snprintf(inst_msg, sizeof(inst_msg), "%s is in Start's list.", ce->name);
		else
			mb_snprintf(inst_msg, sizeof(inst_msg), "Not added: %s.", rc_error(r));
		inst_msg_ok = r == XL_R_OK;
		return;
	}
#endif
	inst_running = 1;
	inst_permille = 0;
	inst_msg[0] = 0;
	redraw();
	int r = do_copy(0, ce, 1);
	inst_running = 0;
	if (r == XL_R_OK)
		mb_snprintf(inst_msg, sizeof(inst_msg), "%s %s is installed.", ce->name, ce->ver);
	else if (r == XL_R_CANCELLED)
		mb_snprintf(inst_msg, sizeof(inst_msg), "Cancelled: %s was not installed.", ce->name);
	else
		mb_snprintf(inst_msg, sizeof(inst_msg), "Not installed: %s.", rc_error(r));
	inst_msg_ok = r == XL_R_OK;
}

/* Install's keys: 1 used, 0 not */
int systems_install_key(int k)
{
	if (k == KEY_F5) {
		inst_msg[0] = 0;
		rescan();
		sel = 0;
		return 1;
	}
	if (k == KEY_ENTER) {
		card_entry_t *list[CARD_MAX];
		int n = update_list(list, CARD_MAX);
		if (sel < n)
			install_entry(list[sel]);
		return 1;
	}
	return 0;
}
