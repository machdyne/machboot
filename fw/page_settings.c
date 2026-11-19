/*
 * machboot
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The Settings page (docs/settings.md).
 * Part of the menu (fw/ui.c, fw/ui_int.h); on Lakritz and Konfekt an
 * overlay, loaded from flash when the page is shown (fw/ovl.h).
 */

#include "ui_int.h"

static const int waits[] = { 1, 2, 3, 5, 10 };
static int settings_changed;
static char settings_msg[64];

void page_settings(void)
{
	static const keyhint_t k[] = { { G_UP G_DOWN, "Choose" }, { G_LEFT G_RIGHT, "Change" }, { "Esc", "Save and back" } };
	char wait[16];
	mb_snprintf(wait, sizeof(wait), "%d seconds", cfg.wait_s);
	const char *labels[4] = { "Start by itself", "Wait", "Keyboard", "Language" };
	const char *rights[4] = {
		cfg.autostart ? (mb.sys_ok ? mb.sysname : "yes") : "no",
		wait,
		cfg.kbd == KBD_DE ? "Deutsch" : "US",
		"English",
	};
	page_items = 4;
	int y = draw_items(5, labels, rights, 4, sel, 1) + 1;
	if (sel == 3)
		text_at(y, "German comes in a later version.", C_GREY);
	else if (!settings_writable())
		if ((mb.board & 0xff) == 2 || (mb.board & 0xff) == 3)
		{
			text_at(y, "Not kept: on this board the RP2040 keeps them,", C_GREY);
			text_at(y + 1, "and it needs machboot-sv.uf2 (docs/rp2040.md).", C_GREY);
		}
		else
			text_at(y, "Settings are kept once machboot is installed.", C_GREY);
	else if (settings_msg[0])
		text_at(y, settings_msg, C_LGREEN);
	draw_keys(k, 3);
}

int settings_key(int k)
{
	if (k != KEY_LEFT && k != KEY_RIGHT)
		return 0;
	int d = k == KEY_RIGHT ? 1 : -1;
	if (sel == 0) {
		cfg.autostart = !cfg.autostart;
	} else if (sel == 1) {
		int i = 0;
		while (i < 4 && waits[i] < cfg.wait_s)
			i++;
		i += d;
		if (i < 0) i = 0;
		if (i > 4) i = 4;
		cfg.wait_s = waits[i];
	} else if (sel == 2) {
		cfg.kbd = cfg.kbd == KBD_DE ? KBD_US : KBD_DE;
		kbd_layout = cfg.kbd;
	} else {
		return 1;
	}
	settings_changed = 1;
	settings_msg[0] = 0;
	return 1;
}

void settings_leave(void)
{
	if (!settings_changed)
		return;
	settings_changed = 0;
	if (settings_save() == 0)
		mb_snprintf(settings_msg, sizeof(settings_msg), "Saved.");
	else if (settings_writable())
		mb_snprintf(settings_msg, sizeof(settings_msg), "Could not save.");
}

