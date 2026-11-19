/*
 * machboot
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The Diagnostics page (fw/diag.c has the tests).
 * Part of the menu (fw/ui.c, fw/ui_int.h); on Lakritz and Konfekt an
 * overlay, loaded from flash when the page is shown (fw/ovl.h).
 */

#include "ui_int.h"

enum { D_MEMORY, D_FLASH, D_SD, D_VIDEO, D_COUNT };
static char diag_res[D_COUNT][16];
static int diag_running = -1;
static int diag_permille;


static void diag_progress(int pm)
{
	diag_permille = pm;
	redraw();
}

static void video_pattern(void)
{
	/* colour bars over the whole screen; any key returns */
	scr_clear(C_WHITE, C_BLACK);
	for (int c = 0; c < 16; c++)
		scr_fill(c * 5, 0, 5, 26, c, c, ' ');
	for (int c = 0; c < 16; c++) {
		char b[4];
		mb_snprintf(b, sizeof(b), "%X", c);
		scr_put(c * 5 + 2, 26, b, C_WHITE, C_BLACK);
	}
	scr_put(0, 27, " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~",
	        C_WHITE, C_BLACK);
	scr_put(0, 28, "\xC4\xB3\xDA\xBF\xC0\xD9\xC3\xB4\xC2\xC1\xC5 \xCD\xBA\xC9\xBB\xC8\xBC \xB0\xB1\xB2\xDB\xDC\xDF "
	        "\x10\x11\x18\x19\x1A\x1B \x07\xFE\xFA\xFB \x84\x94\x81\x8E\x99\x9A\xE1",
	        C_YELLOW, C_BLACK);
	scr_put(0, 29, "Any key returns.", C_GREY, C_BLACK);
	scr_flush();
	con_puts("\nVideo test pattern on the screen. Any key returns.\n");
	while (key_poll() == KEY_NONE)
		;
	mb_snprintf(diag_res[D_VIDEO], sizeof(diag_res[0]), "shown");
}

static void diag_run(int which)
{
	diag_running = which;
	diag_permille = 0;
	redraw();
	if (which == D_MEMORY) {
		int errs = diag_memory_run(diag_progress);
		if (errs)
			mb_snprintf(diag_res[D_MEMORY], sizeof(diag_res[0]), "%d errors", errs);
		else
			mb_snprintf(diag_res[D_MEMORY], sizeof(diag_res[0]), "OK");
	} else if (which == D_FLASH) {
		uint32_t kbps = 0;
		int ok = diag_flash_run(&kbps);
		if (ok)
			mb_snprintf(diag_res[D_FLASH], sizeof(diag_res[0]), "OK, %u KB/s", kbps);
		else
			mb_snprintf(diag_res[D_FLASH], sizeof(diag_res[0]), "error");
	} else if (which == D_SD) {
		uint32_t kbps = 0;
		int r = diag_sd_run(&kbps);
		if (r == 0)
			mb_snprintf(diag_res[D_SD], sizeof(diag_res[0]), "OK, %u KB/s", kbps);
		else
			mb_snprintf(diag_res[D_SD], sizeof(diag_res[0]), r == 1 ? "no card" : "error");
	} else if (which == D_VIDEO) {
		video_pattern();
	}
	diag_running = -1;
	con_printf("diagnostics: %s %s\n", which == D_MEMORY ? "memory" : which == D_FLASH ? "flash" :
	           which == D_SD ? "sd card" : "video",
	           diag_res[which]);
}

void page_diag(void)
{
	static const keyhint_t k[] = {
		{ G_UP G_DOWN, "Choose" }, { "Enter", "Run" }, { "A", "Run all" }, { G_LEFT, "Menu" },
	};
	const char *labels[D_COUNT] = { "Memory", "Flash", "SD card", "Video" };
	const char *rights[D_COUNT];
	static char pct[8];
	for (int i = 0; i < D_COUNT; i++)
		rights[i] = diag_res[i];
	if (diag_running >= 0) {
		mb_snprintf(pct, sizeof(pct), "%d %%", diag_permille / 10);
		rights[diag_running] = pct;
	}
	page_items = D_COUNT;
	int y = draw_items(5, labels, rights, D_COUNT, sel, 1);
	if (diag_running == D_MEMORY) {
		scr_bar(CX + 2, y + 1, 40, diag_permille, C_YELLOW, C_BLUE);
		text_at(y + 3, "Testing memory ...", C_WHITE);
	} else {
		text_at(y + 1, "The memory test covers the menu's free RAM; the", C_GREY);
		text_at(y + 2, "board's SDRAM is tested in a later version.", C_GREY);
		text_at(y + 3, "A runs memory, flash and SD card.", C_GREY);
	}
	draw_keys(k, 4);
}

int diag_key(int k)
{
	if (k == KEY_ENTER) {
		diag_run(sel);
		return 1;
	}
	if (k == 'a' || k == 'A') {
		diag_run(D_MEMORY);
		diag_run(D_FLASH);
		diag_run(D_SD);
		return 1;
	}
	return 0;
}
