/*
 * machboot
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The Start page with the list of systems (fw/systems.h, docs/systems.md),
 * and its keys: Enter starts, F3/F4 move, Del removes (after Y).
 * Part of the menu (fw/ui.c, fw/ui_int.h); on Lakritz and Konfekt an
 * overlay, loaded from flash when the page is shown (fw/ovl.h).
 */

#include "ui_int.h"
#include "systems.h"

void page_systems(void)
{
	static const keyhint_t k[] = {
		{ G_UP G_DOWN, "Choose" }, { "Enter", "Start" }, { "F3 F4", "Move" }, { "Del", "Remove" }, { G_LEFT, "Menu" },
	};
	static const keyhint_t kq[] = { { "Y", "Remove" }, { "N", "Keep" } };
	static const keyhint_t k2[] = { { G_LEFT, "Menu" }, { "F2", "Install" } };
	char buf[96];
	const char *note = rc_note();

	page_items = 0;
	int y = 5;
	if (note) {
		scr_fill(CX - 1, y, CW + 2, 1, C_YELLOW, C_RED, ' ');
		scr_put(CX, y, note, C_YELLOW, C_RED);
		y += 2;
	}
	if (!sys_n) {
		text_at(y++, "No system yet.", C_WHITE);
		y++;
		text_at(y++, card.n ? "Install one from the SD card: choose Install." :
		                      "Put one on the SD card under \\SYS\\GATEWARE.", C_GREY);
		draw_keys(k2, 2);
		return;
	}

	const char *labels[SYS_MAX], *rights[SYS_MAX];
	char (*lab)[44] = page_lab, (*rt)[44] = page_lab + SYS_MAX;	/* CARD_MAX >= 2 x SYS_MAX */
	for (int i = 0; i < sys_n; i++) {
		const sysent_t *e = &sys_list[i];
		mb_snprintf(lab[i], sizeof(lab[i]), e->ver[0] ? "%s %s" : "%s", e->name, e->ver);
		mb_snprintf(rt[i], sizeof(rt[i]), i ? "%s" : "default, %s", sys_where(e));
		labels[i] = lab[i];
		rights[i] = rt[i];
	}
	if (sel >= sys_n)
		sel = sys_n - 1;
	page_items = rm_ask ? 0 : sys_n;
	y = draw_items(y, labels, rights, sys_n, sel, 1) + 1;

	const sysent_t *e = &sys_list[sel];
	if (sys_msg[0]) {
		text_at(y, sys_msg, sys_msg_ok ? C_LGREEN : C_LRED);
		y += 2;
	}
	if (e->module) {
#ifdef MB_SLOT
		mb_snprintf(buf, sizeof(buf), "Enter starts %s from flash (nothing is written).", e->name);
		text_at(y++, buf, C_WHITE);
#else
		mb_snprintf(buf, sizeof(buf), "Enter starts %s from the module's flash.", e->name);
		text_at(y++, buf, C_WHITE);
		text_at(y++, "machboot does not change the module's flash.", C_GREY);
#endif
	} else if (e->cached) {
		mb_snprintf(buf, sizeof(buf), "Enter starts %s from flash (nothing is written).", e->name);
		text_at(y++, buf, C_WHITE);
	} else if (e->ce) {
		mb_snprintf(buf, sizeof(buf), "Enter copies %s from %s into flash", e->name, src_name(e->ce));
		text_at(y++, buf, C_WHITE);
		mb_snprintf(buf, sizeof(buf), "(about %u s), then starts it.", about_s(e->ce->bit.size));
		y = copy_hint(y, buf, e->ce, e->crc);
	} else {
		mb_snprintf(buf, sizeof(buf), "%s is not in flash, and not on the card.", e->name);
		text_at(y++, buf, C_WHITE);
		text_at(y++, "Put the card with it in, or remove it from the list.", C_GREY);
	}
	y++;
	if (cfg.autostart)
		mb_snprintf(buf, sizeof(buf), "The top one starts by itself after %d seconds.", cfg.wait_s);
	else
		mb_snprintf(buf, sizeof(buf), "Nothing starts by itself (Settings).");
	text_at(y++, buf, C_GREY);

	if (details) {
		y++;
#ifdef MB_SLOT
		mb_snprintf(buf, sizeof(buf), "Slot        0x%06x, %s", mb.slot, mb.sys_ok ? mb.sysname : "empty");
		text_at(y++, buf, C_DGREY);
		if (e->module && cfg.sys_size) {
			mb_snprintf(buf, sizeof(buf), "Installed   %u bytes, CRC-32 %08x", cfg.sys_size, cfg.sys_crc);
			text_at(y++, buf, C_DGREY);
		}
#else
		mb_snprintf(buf, sizeof(buf), "RP2040 flash %u of %u KB free, largest gap %u KB",
		            sys_rc.free / 1024, sys_rc.size / 1024, sys_rc.largest / 1024);
		text_at(y++, buf, C_DGREY);
		if (!e->module && e->crc) {
			mb_snprintf(buf, sizeof(buf), "Bitstream   %u bytes, CRC-32 %08x", e->len, e->crc);
			text_at(y++, buf, C_DGREY);
		}
#endif
		if (e->ce) {
			mb_snprintf(buf, sizeof(buf), "On card     \\SYS\\GATEWARE\\%s\\%s %s", e->ce->dir, e->ce->bit.name, e->ce->ver);
			text_at(y++, buf, C_DGREY);
		}
	}

	if (rm_ask) {
		scr_fill(CX - 1, 25, CW + 2, 2, C_YELLOW, C_RED, ' ');
		mb_snprintf(buf, sizeof(buf), "Remove %s from the list?", e->name);
		scr_put(CX, 25, buf, C_YELLOW, C_RED);
		scr_put(CX, 26, e->cached ? "It is erased from flash; the card keeps its copy." :
		                            "Nothing is written; the card keeps its copy.", C_YELLOW, C_RED);
		draw_keys(kq, 2);
		return;
	}
	draw_keys(k, 5);
}

/* Start's keys: 1 used, 0 not */
int systems_start_key(int k)
{
	if (rm_ask) {
		rm_ask = 0;
		if (k == 'y' || k == 'Y') {
			char name[24];
			memcpy(name, sys_list[sel].name, sizeof(name));
			int r = systems_remove(sel);
			if (r == XL_R_OK)
				mb_snprintf(sys_msg, sizeof(sys_msg), "%s was removed.", name);
			else
				mb_snprintf(sys_msg, sizeof(sys_msg), "%s was not removed: %s.", name, rc_error(r));
			sys_msg_ok = r == XL_R_OK;
			if (sel >= sys_n && sel)
				sel--;
		}
		return 1;
	}
	if (!sys_n)
		return k == KEY_ENTER;
	if (k == KEY_ENTER) {
		start_entry(sel);
		return 1;
	}
	if (k == KEY_F3 || k == KEY_F4) {
		sel = systems_move(sel, k == KEY_F3 ? sel - 1 : sel + 1);
		sys_msg[0] = 0;
		return 1;
	}
	if (k == KEY_DEL) {
		if (sys_list[sel].module) {
#ifdef MB_SLOT
			mb_snprintf(sys_msg, sizeof(sys_msg), "%s is in flash: it stays in the list.", sys_list[sel].name);
#else
			mb_snprintf(sys_msg, sizeof(sys_msg), "The module's own system stays in the list.");
#endif
			sys_msg_ok = 0;
		} else {
			rm_ask = 1;
			sys_msg[0] = 0;
		}
		return 1;
	}
	return 0;
}
