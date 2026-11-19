/*
 * machboot
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The startup screen and the menu (docs/ui.md).
 *
 * One startup screen; everything else is one frame -- sections on the
 * left, the page on the right, the page's keys on the bottom line. The
 * serial console shows the same pages as text (mirror()). The pages
 * themselves are in fw/page_*.c (fw/ui_int.h): on Lakritz and Konfekt
 * each is loaded from flash when it is shown (fw/ovl.h).
 */

#include "ui_int.h"
#ifdef MB_SYSTEMS
#include "systems.h"
#endif

/* the card (and a USB drive) read again; the list of systems follows it */
void rescan(void)
{
	card_scan();
#ifdef MB_SYSTEMS
	if (systems_on())
		systems_refresh();
#endif
}

/* ------------------------------------------------------------------------- */
/* frame                                                                     */
/* ------------------------------------------------------------------------- */

section_t sections[S_COUNT] = {
	{ "Start",       "Start a system",           's', 1 },
	{ "Install",     "Install a system",         'i', 1 },
	{ "Card reader", "Card reader",              'c', 0 },
	{ "Settings",    "Settings",                 't', 1 },
	{ "Run a file",  "Run a file from the card", 'r', 0 },
	{ "Diagnostics", "Diagnostics",              'd', 1 },
	{ "Board",       "Board",                    'b', 1 },
};

/* a separator is drawn before these */
static int sep_before(int s) { return s == S_RUN; }

int cur = S_START;
int focus_side;
int sel;
int details;

static void draw_frame(void)
{
	scr_clear(C_WHITE, C_BLUE);

	scr_fill(0, 0, 80, 1, C_BLACK, C_GREY, ' ');
	scr_put(2, 0, "MACHBOOT", C_BLUE, C_GREY);
	const char *bn = board_name(mb.board);
	char bl[40];
	mb_snprintf(bl, sizeof(bl), "Machdyne %s", bn);
	scr_put(78 - (int)strlen(bl), 0, bl, C_BLACK, C_GREY);

	scr_fill(0, 1, SB, 28, C_BLACK, C_GREY, ' ');
	int y = 3;
	for (int s = 0; s < S_COUNT; s++) {
		if (sep_before(s)) {
			scr_hrule(2, y, SB - 4, C_DGREY, C_GREY);
			y += 1;
		}
		const section_t *sc = &sections[s];
		if (s == cur) {
			scr_fill(0, y, SB, 1, C_WHITE, C_BLUE, ' ');
			if (focus_side) {
				scr_put(1, y, G_TRI_R, C_WHITE, C_BLUE);
				scr_put(3, y, sc->name, C_WHITE, C_BLUE);
			} else {
				scr_put(2, y, sc->name, C_YELLOW, C_BLUE);
			}
		} else {
			scr_put(2, y, sc->name, sc->avail ? C_BLACK : C_DGREY, C_GREY);
		}
		y += 2;
	}

	const char *t = sections[cur].title;
	scr_put(CX, 2, t, C_WHITE, C_BLUE);
	scr_hrule(CX, 3, (int)strlen(t), C_GREY, C_BLUE);
}

void draw_keys(const keyhint_t *k, int n)
{
	scr_fill(0, 29, 80, 1, C_BLACK, C_GREY, ' ');
	int x = 2;
	for (int i = 0; i < n; i++) {
		if (k[i].key[0]) {
			scr_put(x, 29, k[i].key, C_RED, C_GREY);
			x += (int)strlen(k[i].key) + 1;
		}
		scr_put(x, 29, k[i].label, C_BLACK, C_GREY);
		x += (int)strlen(k[i].label) + 3;
	}
}

/* a list of choices; returns the row after it */
int draw_items(int y, const char *const *labels, const char *const *rights, int n, int selected, int gap)
{
	for (int i = 0; i < n; i++) {
		const char *r = rights ? rights[i] : 0;
		if (i == selected && !focus_side) {
			scr_fill(CX - 1, y, CW + 2, 1, C_BLUE, C_GREY, ' ');
			scr_put(CX, y, G_TRI_R " ", C_BLUE, C_GREY);
			scr_put(CX + 2, y, labels[i], C_BLUE, C_GREY);
			if (r && *r)
				scr_put(CX + CW - (int)strlen(r), y, r, C_RED, C_GREY);
		} else {
			scr_put(CX + 2, y, labels[i], C_WHITE, C_BLUE);
			if (r && *r)
				scr_put(CX + CW - (int)strlen(r), y, r, C_LCYAN, C_BLUE);
		}
		y += 1 + gap;
	}
	return y;
}

void text_at(int y, const char *s, int fg)
{
	scr_put(CX, y, s, fg, C_BLUE);
}

void kv_at(int y, const char *k, const char *v)
{
	scr_put(CX, y, k, C_YELLOW, C_BLUE);
	scr_put(CX + 13, y, v, C_WHITE, C_BLUE);
}

/* ------------------------------------------------------------------------- */
/* the serial mirror                                                         */
/* ------------------------------------------------------------------------- */

void mirror(int with_sidebar)
{
	char line[84];

	con_puts("\n");
	if (with_sidebar) {
		con_puts("Sections:");
		for (int s = 0; s < S_COUNT; s++)
			con_printf("  %c %s%s", sections[s].letter, sections[s].name,
			           sections[s].avail ? "" : " (later)");
		con_puts("\n");
	}
	con_printf("== %s ==\n", sections[cur].title);
	int blank = 0;
	for (int y = 4; y < 29; y++) {
		scr_row_text(y, CX - 1, 80, line, sizeof(line));
		if (!line[0]) {
			blank++;
			continue;
		}
		if (blank && y > 5)
			con_puts("\n");
		blank = 0;
		con_printf("%s\n", line);
	}
	scr_row_text(29, 0, 80, line, sizeof(line));
	con_printf("[%s]\n", line + (line[0] == ' ' ? 1 : 0));
}

/* ------------------------------------------------------------------------- */
/* pages                                                                     */
/* ------------------------------------------------------------------------- */

int page_items;

static void page_later(void)
{
	static const keyhint_t k[] = { { G_LEFT, "Menu" } };
	page_items = 0;
	text_at(5, "This comes in a later version of machboot.", C_GREY);
	draw_keys(k, 1);
}

/* the card's copy of the installed system, if the card has one for this board */
card_entry_t *card_copy_of_installed(void)
{
	if (!cfg.sys_name[0])
		return 0;
	card_entry_t *ce = card_system(cfg.sys_name);
	return (ce && ce->has_bit) ? ce : 0;
}

/* writing the slot is safe only when machboot runs from 0x000000 */
int can_install(void)
{
	return !mb.install_mode && mb.fw_base != 0;
}


/* -- update ------------------------------------------------------------- */

int inst_running;
char page_lab[CARD_MAX][44];
int inst_permille;
const char *inst_what = "";
char inst_msg[64];
int inst_msg_ok;

static void update_progress(int pm, const char *what);

/* the systems on the card, in card order (loose files are for "Run a file") */
int update_list(card_entry_t **out, int max)
{
	int n = 0;
	for (int i = 0; i < card.n && n < max; i++)
		if (card.e[i].kind == CE_SYSTEM)
			out[n++] = &card.e[i];
	return n;
}


/* -- card reader (docs/usb-device.md) -- */

card_entry_t *card_offer;	/* after the PC had the card: what is new on it */

/* the card came back from the PC: anything to install? */
static void card_find_offer(void)
{
	card_offer = 0;
	if (!can_install())
		return;
	card_offer = newer_on_card();
	if (!card_offer && !mb.sys_ok)
		for (int i = 0; i < card.n && !card_offer; i++)
			if (card.e[i].kind == CE_SYSTEM && card.e[i].has_bit)
				card_offer = &card.e[i];
}

void card_unshare(void)
{
	if (!usbd.msc_shared)
		return;
	usbd_msc_share(0);
	rescan();		/* the PC may have changed the files */
	card_find_offer();
	sel = 0;
	con_puts("card reader: the card is machboot's again\n");
}

static void draw_usb_band(void)
{
	char buf[80];
	if (usbd.dfu == UD_DFU_IDLE)
		return;
	if (usbd.dfu == UD_DFU_RECEIVING) {
		mb_snprintf(buf, sizeof(buf), " dfu-util: receiving ... %u KB ", usbd.dfu_bytes / 1024);
		scr_fill(CX - 1, 27, CW + 2, 1, C_BLACK, C_YELLOW, ' ');
		scr_put(CX, 27, buf, C_BLACK, C_YELLOW);
	} else if (usbd.dfu == UD_DFU_DONE) {
		if (usbd.dfu_region_bytes)
			mb_snprintf(buf, sizeof(buf), " dfu-util: done. %u KB system, %u KB Zeitlos region ",
			            usbd.dfu_slot_bytes / 1024, usbd.dfu_region_bytes / 1024);
		else
			mb_snprintf(buf, sizeof(buf), " dfu-util: done. %u KB into the system slot ", usbd.dfu_slot_bytes / 1024);
		scr_fill(CX - 1, 27, CW + 2, 1, C_BLACK, C_LGREEN, ' ');
		scr_put(CX, 27, buf, C_BLACK, C_LGREEN);
	} else {
		mb_snprintf(buf, sizeof(buf), " dfu-util: not written: %s ", usbd.dfu_error ? usbd.dfu_error : "error");
		scr_fill(CX - 1, 27, CW + 2, 1, C_YELLOW, C_RED, ' ');
		scr_put(CX, 27, buf, C_YELLOW, C_RED);
	}
}

/* a page whose code could not be loaded from flash (fw/ovl.c) */
static void page_missing(void)
{
	static const keyhint_t k[] = { { G_LEFT, "Menu" } };
	page_items = 0;
	text_at(5, "This page could not be loaded:", C_WHITE);
	text_at(6, ovl_error(), C_LRED);
	text_at(8, "Install machboot again (docs/install.md).", C_GREY);
	draw_keys(k, 1);
}

/* the overlay a section's page is in */
static int page_ovl(int s)
{
	switch (s) {
	case S_START: return mb.install_mode ? OVL_INSTMODE : OVL_START;
	case S_UPDATE: return OVL_UPDATE;
	case S_CARD: return OVL_CARD;
	case S_SETTINGS: return OVL_SETTINGS;
	case S_DIAG: return OVL_DIAG;
	case S_BOARD: return OVL_BOARD;
	default: return -1;
	}
}

/* the page's code is there to call (a page key, a redraw) */
static int page_here(void)
{
	int o = page_ovl(cur);
	return o < 0 || ovl_use(o);
}

/* noinline: the calls into the pages stay here, where tools/check_ovl.py
   looks for them (after ovl_use()) */
__attribute__((noinline)) void redraw(void)
{
	draw_frame();
	if (!page_here())
		page_missing();
#ifdef MB_SYSTEMS
	else if (multi() && cur == S_START)
		page_systems();
	else if (multi() && cur == S_UPDATE)
		page_install();
#endif
#ifdef MB_SLOT
	/* Lakritz, Konfekt installed: the list's pages, never the old ones
	   (those are the JTAG test build's, which has no list) */
	else if (cur == S_START && mb.install_mode)
		page_install_mode();
	else if (cur == S_START || cur == S_UPDATE)
		page_missing();
#endif
	else switch (cur) {
#ifndef MB_SLOT
	case S_START:
		if (mb.install_mode)
			page_install_mode();
		else
			page_start();
		break;
	case S_UPDATE: page_update(); break;
#endif
	case S_CARD: page_card(); break;
	case S_SETTINGS: page_settings(); break;
	case S_DIAG: page_diag(); break;
	case S_BOARD: page_board(); break;
	default: page_later(); break;
	}
	draw_usb_band();
	scr_flush();
}

static void go_section(int s)
{
	if (s < 0 || s >= S_COUNT)
		return;
	if (cur == S_CARD && s != S_CARD)
		card_unshare();
	cur = s;
	sel = 0;
}

static void update_progress(int pm, const char *what)
{
	inst_permille = pm;
	inst_what = what;
	redraw();
}

void do_install(card_entry_t *ce)
{
	inst_running = 1;
	inst_permille = 0;
	inst_what = "Checking";
	inst_msg[0] = 0;
	redraw();
	int r = install(ce, update_progress);
	inst_running = 0;
	mb_probe_system();
	if (r == INST_OK)
		mb_snprintf(inst_msg, sizeof(inst_msg), "%s %s is installed.", ce->name, ce->ver);
	else if (r == INST_SAME)
		mb_snprintf(inst_msg, sizeof(inst_msg), "%s %s was already installed.", ce->name, ce->ver);
	else
		mb_snprintf(inst_msg, sizeof(inst_msg), "Not installed: %s.", install_error(r));
	inst_msg_ok = (r == INST_OK || r == INST_SAME);
	con_printf("update: %s\n", inst_msg);
}

__attribute__((noinline)) static int page_key(int k)
{
	/* returns 1 if the page used the key */
#ifdef MB_SYSTEMS
	if (multi() && cur == S_START)
		return ovl_use(OVL_START) && systems_start_key(k);
	if (multi() && cur == S_UPDATE)
		return ovl_use(OVL_UPDATE) && systems_install_key(k);
#endif
	if (cur == S_START && mb.install_mode && ovl_use(OVL_INSTMODE) && install_mode_key(k))
		return 1;
	if (cur == S_START && k == KEY_ENTER && mb.sys_ok) {
		ui_boot();
		return 1;
	}
	if (cur == S_START && k == KEY_ENTER && !mb.sys_ok && card_copy_of_installed() && can_install()) {
		card_entry_t *ce = card_copy_of_installed();
		go_section(S_UPDATE);
		card_entry_t *list[CARD_MAX];
		int n = update_list(list, CARD_MAX);
		for (int i = 0; i < n; i++)
			if (list[i] == ce)
				sel = i;
		do_install(ce);
		return 1;
	}
	if (cur == S_UPDATE) {
		if (k == KEY_F5) {
			inst_msg[0] = 0;
			rescan();
			sel = 0;
			return 1;
		}
		if (k == KEY_ENTER) {
			card_entry_t *list[CARD_MAX];
			int n = update_list(list, CARD_MAX);
			if (sel < n && list[sel]->has_bit && can_install())
				do_install(list[sel]);
			return 1;
		}
	}
	if (cur == S_CARD) {
		if (k == KEY_F5 && !usbd.msc_shared) {
			rescan();
			return 1;
		}
		if (k == KEY_ENTER && sd.present && !usbd.msc_shared && card_offer && sel == 0) {
			card_entry_t *ce = card_offer;
			card_offer = 0;
			go_section(S_UPDATE);
			do_install(ce);
			return 1;
		}
		if (k == KEY_ENTER && sd.present) {
			if (usbd.msc_shared) {
				card_unshare();
			} else {
				usbd.msc_read = usbd.msc_written = 0;
				usbd_msc_share(1);
				con_puts("card reader: the card is the PC's (a USB drive)\n");
			}
			return 1;
		}
	}
	if (cur == S_SETTINGS)
		return ovl_use(OVL_SETTINGS) && settings_key(k);
	if (cur == S_DIAG)
		return ovl_use(OVL_DIAG) && diag_key(k);
	return 0;
}

__attribute__((noinline)) void ui_menu(int where)
{
	sections[S_CARD].avail = usbd.present;
	if (mb.install_mode)
		sections[S_START].title = "Install machboot";
	go_section(where == UI_UPDATE ? S_UPDATE : S_START);
	focus_side = 0;
	redraw();
	mirror(1);

	int dfu_seen = usbd.dfu_events, msc_seen = usbd.msc_events, was_shared = usbd.msc_shared;
	uint32_t msc_t = ms_now();
#ifdef MB_SYSTEMS
	int multi_was = multi();
	const char *note_was = rc_note();
#endif
	for (;;) {
		int k = key_poll();
#ifdef MB_SYSTEMS
		if (multi() != multi_was || rc_note() != note_was) {
			/* the RP2040 came (or went), or told us something */
			multi_was = multi();
			note_was = rc_note();
			if (multi_was)
				systems_refresh();
			sel = 0;
			redraw();
			mirror(0);
		}
		if (k != KEY_NONE && cur == S_START && rm_ask) {
			page_key(k);		/* the question gets every key */
			redraw();
			mirror(0);
			continue;
		}
#endif
		if (usbd.dfu_events != dfu_seen) {
			dfu_seen = usbd.dfu_events;
			redraw();
		}
		if (usb_drive_changed()) {
			/* a flash drive came or went: what it has is listed with the card's */
			if (!usbd.msc_shared)
				rescan();
			redraw();
			mirror(0);
		}
		if (was_shared && !usbd.msc_shared) {
			/* the PC ejected it */
			rescan();
			card_find_offer();
			sel = 0;
			con_puts("card reader: ejected on the PC; the card is machboot's again\n");
			redraw();
			mirror(0);
		}
		was_shared = usbd.msc_shared;
		if ((usbd.msc_events != msc_seen || (usbd.msc_shared && ms_now() - msc_t > 1000)) &&
		    cur == S_CARD && ms_now() - msc_t > 250) {
			msc_seen = usbd.msc_events;
			msc_t = ms_now();
			redraw();
		}
		if (k == KEY_NONE)
			continue;

		int changed = 1, side = 0;

		if (cur == S_START && mb.install_mode && (im_phase == IM_CONFIRM || im_phase == IM_FAILED)) {
			/* a question on the page: it gets every key */
			if (ovl_use(OVL_INSTMODE))
				install_mode_key(k);
			redraw();
			mirror(0);
			continue;
		}

		if (k == KEY_F1) {
			details = !details;
		} else if (k == KEY_F2 && cur != S_UPDATE) {
			go_section(S_UPDATE);
			focus_side = 0;
		} else if (focus_side) {
			if (k == KEY_UP) {
				go_section(cur > 0 ? cur - 1 : S_COUNT - 1);
			} else if (k == KEY_DOWN) {
				go_section(cur < S_COUNT - 1 ? cur + 1 : 0);
			} else if (k == KEY_RIGHT || k == KEY_ENTER) {
				focus_side = 0;
			} else {
				int found = 0;
				for (int s = 0; s < S_COUNT; s++)
					if (k == sections[s].letter) {
						go_section(s);
						found = 1;
					}
				changed = found;
			}
			side = 1;
		} else {
			if ((k == KEY_LEFT && cur != S_SETTINGS) || k == KEY_ESC || k == KEY_BKSP) {
				if (cur == S_SETTINGS && ovl_use(OVL_SETTINGS))
					settings_leave();
				if (cur == S_CARD)
					card_unshare();
				focus_side = 1;
				side = 1;
			} else if (k == KEY_UP) {
				if (page_items && sel > 0)
					sel--;
			} else if (k == KEY_DOWN) {
				if (page_items && sel < page_items - 1)
					sel++;
			} else {
				changed = page_key(k);
			}
		}

		if (changed) {
			redraw();
			mirror(side);
		}
	}
}

/* ------------------------------------------------------------------------- */
/* startup                                                                   */
/* ------------------------------------------------------------------------- */

static const char *startup_note;	/* one line, only if something needs attention */

void draw_startup(const char *msg, int msg_fg, int permille, int show_bar)
{
	scr_clear(C_GREY, C_BLACK);
	scr_center(9, "M A C H B O O T", C_WHITE, C_BLACK);
	char bl[40];
	mb_snprintf(bl, sizeof(bl), "Machdyne %s", board_name(mb.board));
	scr_center(10, bl, C_DGREY, C_BLACK);
	scr_center(14, msg, msg_fg, C_BLACK);
	if (show_bar) {
		int n = (24 * permille + 500) / 1000;
		scr_fill(28, 16, n, 1, C_LGREEN, C_BLACK, CH_BLOCK);
		scr_fill(28 + n, 16, 24 - n, 1, C_DGREY, C_BLACK, CH_SHADE1);
	}
	if (startup_note)
		scr_center(20, startup_note, C_YELLOW, C_BLACK);
	scr_fill(0, 29, 80, 1, C_GREY, C_BLACK, ' ');
#ifdef MB_SYSTEMS
	if (startup_copying == 2) {
		scr_put(28, 29, "Please do not switch off", C_YELLOW, C_BLACK);
		scr_flush();
		return;
	}
	if (startup_copying) {
		scr_put(36, 29, "Esc", C_LRED, C_BLACK);
		scr_put(40, 29, "Cancel", C_GREY, C_BLACK);
		scr_flush();
		return;
	}
#endif
	scr_put(30, 29, "Enter", C_LRED, C_BLACK);
	scr_put(36, 29, "Start now", C_GREY, C_BLACK);
	scr_put(48, 29, "F8", C_LRED, C_BLACK);
	scr_put(51, 29, "Menu", C_GREY, C_BLACK);
	scr_flush();
}

/* a newer version of the installed system on the card? */
card_entry_t *newer_on_card(void)
{
	if (!cfg.sys_name[0] || !mb.sys_ok || mb.install_mode || !mb.fw_base)
		return 0;
	card_entry_t *ce = card_system(cfg.sys_name);
	if (!ce || !ce->has_bit || !strcmp_ci(ce->ver, cfg.sys_ver))
		return 0;
	return ce;
}

int ui_startup(void)
{
	char msg[96];
	static char note[64];

	con_printf("\nmachboot %s  Machdyne %s\n", MB_VERSION_STR, board_name(mb.board));

#ifdef MB_SYSTEMS
	if (multi()) {
		char m[80];
		if (!cfg.autostart || !systems_countdown(m, sizeof(m), cfg.wait_s))
			return UI_MENU;
	} else
#endif
	if (!mb.sys_ok || !cfg.autostart)
		return UI_MENU;	/* nothing to start, or not by itself: the menu */

	card_entry_t *ce = newer_on_card();
	startup_note = 0;
	if (ce) {
		mb_snprintf(note, sizeof(note), "A new %s is on the card.  F2 Install", ce->name);
		startup_note = note;
		con_printf("%s %s is on the card (F2: update)\n", ce->name, ce->ver);
	}

	con_printf("Starting %s in %d ... (Enter: start now, F8, Esc or m: menu)\n",
#ifdef MB_SYSTEMS
	           multi() ? sys_list[0].name :
#endif
	           mb.sysname, cfg.wait_s);

	uint32_t t0 = ms_now();
	int last = -1;
	for (;;) {
		/* settings may still arrive (Sergei: from the RP2040) */
		uint32_t total = (uint32_t)cfg.wait_s * 1000;
		if (!cfg.autostart)
			return UI_MENU;
		uint32_t el = ms_now() - t0;
		if (el >= total)
			return UI_BOOT;
		int left = (int)((total - el + 999) / 1000);
		int pm = (int)(el * 1000 / total);
		if (pm / 20 != last) {
			last = pm / 20;
			mb_snprintf(msg, sizeof(msg), "Starting %s in %d ...", mb.sysname, left);
#ifdef MB_SYSTEMS
			if (multi() && !systems_countdown(msg, sizeof(msg), left))
				return UI_MENU;
#endif
			draw_startup(msg, C_WHITE, pm, 1);
		}
		int k = key_poll();
		if (usbd.dfu != UD_DFU_IDLE) {
			con_puts("dfu-util is writing: the menu instead\n");
			return UI_MENU;
		}
		if (k == KEY_ENTER)
			return UI_BOOT;
		if (k == KEY_F2 && ce)
			return UI_UPDATE;
		if (k == KEY_F8 || k == KEY_ESC || k == KEY_DEL || k == 'm' || k == 'M')
			return UI_MENU;
	}
}

void boot_module(void)
{
	char msg[64];

	if (!mb.sys_ok)
		return;
	mb_snprintf(msg, sizeof(msg), "Starting %s ...", mb.sysname);
	draw_startup(msg, C_WHITE, 1000, 1);
	con_printf("%s\n", msg);
	ms_sleep(20);			/* let the UART drain */

	usbd_before_start();
	sys_wr(SYS_RECONFIG, RECONFIG_KEY);
	ms_sleep(500);

	/* still here: PROGRAMN did not reconfigure the FPGA */
	draw_startup("The FPGA did not restart (PROGRAMN).", C_LRED, 0, 0);
	con_puts("PROGRAMN was pulled but the FPGA did not reconfigure.\n");
	ms_sleep(3000);
}

void ui_boot(void)
{
#ifdef MB_SYSTEMS
	if (multi()) {
		if (sys_n)
			start_entry(0);
		return;
	}
#endif
	boot_module();
}
