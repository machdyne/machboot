/*
 * machboot
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Install mode: the Start page that replaces tinydfu (docs/install-mode.md).
 * Part of the menu (fw/ui.c, fw/ui_int.h); on Lakritz and Konfekt an
 * overlay, loaded from flash when the page is shown (fw/ovl.h).
 */

#include "ui_int.h"
#include "selfinst.h"

int im_phase;
#if !defined(MB_SYSTEMS) || defined(MB_SLOT)	/* not Sergei, Mozart */
static int im_permille;
static const char *im_what = "";
static char im_msg[64];

/* what install mode puts in the system slot afterwards: Zeitlos if the card has it */
static card_entry_t *im_system(void)
{
	card_entry_t *z = card_system("Zeitlos");
	if (z && z->has_bit)
		return z;
	for (int i = 0; i < card.n; i++)
		if (card.e[i].kind == CE_SYSTEM && card.e[i].has_bit)
			return &card.e[i];
	return 0;
}

static void im_progress(int pm, const char *what)
{
	im_permille = pm;
	im_what = what;
	redraw();
}

void page_install_mode(void)
{
	static const keyhint_t k_offer[] = { { "Enter", "Install" }, { G_LEFT, "Menu" }, { "F1", "Details" } };
	static const keyhint_t k_conf[] = { { "Y", "Yes, install" }, { "Esc", "Cancel" } };
	static const keyhint_t k_run[] = { { "", "Please do not switch off" } };
	static const keyhint_t k_fail[] = { { "Enter", "Try again" } };
	static const keyhint_t k_none[] = { { G_LEFT, "Menu" }, { "F1", "Details" } };
	char buf[64];
	card_entry_t *ce = im_system();

	page_items = 0;
	if (si.state == SI_NONE) {
		text_at(5, "machboot is running from where dfu-util put it.", C_WHITE);
		text_at(7, "This image cannot install itself. To install", C_GREY);
		text_at(8, "machboot, write machboot-lakritz-install.bin", C_GREY);
		text_at(9, "with dfu-util instead. Nothing has been changed.", C_GREY);
		draw_keys(k_none, 2);
		return;
	}
	if (si.state == SI_BAD && im_phase != IM_FAILED) {
		scr_fill(CX - 1, 5, CW + 2, 1, C_YELLOW, C_RED, ' ');
		scr_put(CX, 5, "This machboot installer is damaged.", C_YELLOW, C_RED);
		text_at(7, "Write machboot-lakritz-install.bin again with", C_GREY);
		text_at(8, "dfu-util. Nothing has been changed.", C_GREY);
		draw_keys(k_none, 2);
		return;
	}

	if (im_phase == IM_RUNNING) {
		text_at(5, "Installing machboot.", C_WHITE);
		mb_snprintf(buf, sizeof(buf), "%s ...", im_what);
		text_at(7, buf, C_WHITE);
		scr_bar(CX, 9, 40, im_permille, C_LGREEN, C_BLUE);
		mb_snprintf(buf, sizeof(buf), "%d %%", im_permille / 10);
		scr_put(CX + 42, 9, buf, C_WHITE, C_BLUE);
		text_at(11, "The board must stay on until this is done.", C_YELLOW);
		draw_keys(k_run, 1);
		return;
	}
	if (im_phase == IM_DONE) {
		text_at(5, "machboot is installed.", C_LGREEN);
		if (im_msg[0])
			text_at(7, im_msg, C_WHITE);
		text_at(9, "Restarting ...", C_GREY);
		draw_keys(k_run, 1);
		return;
	}
	if (im_phase == IM_FAILED) {
		scr_fill(CX - 1, 5, CW + 2, 1, C_YELLOW, C_RED, ' ');
		scr_put(CX, 5, "Installing machboot did not finish.", C_YELLOW, C_RED);
		text_at(7, im_msg, C_WHITE);
		text_at(9, "Do not switch off: the board may not start", C_YELLOW);
		text_at(10, "again until machboot is installed. Enter tries", C_YELLOW);
		text_at(11, "again from where it stopped.", C_YELLOW);
		draw_keys(k_fail, 1);
		return;
	}

	text_at(5, "machboot is running from where dfu-util put it.", C_WHITE);
	const char *labels[1] = { "Install machboot" };
	const char *rights[1] = { "replaces tinydfu" };
	page_items = im_phase == IM_OFFER ? 1 : 0;
	int y = draw_items(7, labels, rights, 1, sel, 1);
	text_at(y++, "It becomes this board's boot manager. It takes", C_GREY);
	text_at(y++, "about 20 seconds, and the board must stay on.", C_GREY);
	y++;
	if (ce) {
		mb_snprintf(buf, sizeof(buf), "Then %s %s is installed from the card.", ce->name, ce->ver);
		text_at(y++, buf, C_GREY);
	} else {
		text_at(y++, "There is no system for this board on the card;", C_GREY);
		text_at(y++, "install one afterwards from Install.", C_GREY);
	}
	text_at(y++, "The Zeitlos region in flash is not touched.", C_GREY);
	if (details) {
		y++;
		mb_snprintf(buf, sizeof(buf), "Image       0x%06x, %u bytes, CRC-32 %08x", si.src, si.h.payload_len, si.h.payload_crc);
		text_at(y++, buf, C_DGREY);
		mb_snprintf(buf, sizeof(buf), "Version     %u.%u.%u", si.h.version >> 16, (si.h.version >> 8) & 0xff, si.h.version & 0xff);
		text_at(y++, buf, C_DGREY);
		text_at(y++, "Writes      0x000000-0x05FFFF, then the system slot", C_DGREY);
	}
	if (im_phase == IM_CONFIRM) {
		scr_fill(CX - 1, 26, CW + 2, 1, C_YELLOW, C_RED, ' ');
		scr_put(CX, 26, "Replace tinydfu with machboot now?", C_YELLOW, C_RED);
		draw_keys(k_conf, 2);
	} else {
		draw_keys(k_offer, 3);
	}
}

static void im_run(void)
{
	im_phase = IM_RUNNING;
	im_permille = 0;
	im_what = "Checking";
	redraw();
	int r = selfinst_run(im_progress);
	if (r != SI_OK) {
		if (r == SI_ERR_IMAGE) {
			im_phase = IM_OFFER;	/* the page now says "damaged" */
			mb_snprintf(im_msg, sizeof(im_msg), "The installer image is damaged; nothing was written.");
		} else {
			im_phase = IM_FAILED;
			mb_snprintf(im_msg, sizeof(im_msg), "The flash could not be written at 0x%06x.", si.fail_at);
		}
		con_printf("install mode: %s\n", im_msg);
		return;
	}

	/* the new boot slot's settings: keep the keyboard chosen here */
	settings_attach(SETTINGS_AT);
	settings_save();

	im_msg[0] = 0;
	card_entry_t *ce = im_system();
	if (ce) {
		im_what = "Installing";
		im_progress(0, ce->name);
		int ri = install(ce, im_progress);
		if (ri == INST_OK || ri == INST_SAME)
			mb_snprintf(im_msg, sizeof(im_msg), "%s %s is installed too.", ce->name, ce->ver);
		else
			mb_snprintf(im_msg, sizeof(im_msg), "%s was not installed: %s.", ce->name, install_error(ri));
		con_printf("install mode: %s\n", im_msg);
	}
	im_phase = IM_DONE;
	redraw();
	mirror(0);
	con_puts("install mode: done, restarting\n");
	ms_sleep(3000);
	sys_wr(SYS_RECONFIG, RECONFIG_KEY);
	ms_sleep(500);
	mb_snprintf(im_msg, sizeof(im_msg), "Switch the board off and on to start machboot.");
	redraw();
	mirror(0);
	for (;;)
		;
}

int install_mode_key(int k)
{
	if (si.state == SI_NONE)
		return 0;
	if (im_phase == IM_FAILED) {
		if (k == KEY_ENTER)
			im_run();
		return 1;		/* nothing else until it is done */
	}
	if (si.state != SI_READY)
		return 0;
	if (im_phase == IM_OFFER && k == KEY_ENTER) {
		im_phase = IM_CONFIRM;
		return 1;
	}
	if (im_phase == IM_CONFIRM) {
		if (k == 'y' || k == 'Y')
			im_run();
		else
			im_phase = IM_OFFER;
		return 1;
	}
	return 0;
}
#else
/* Sergei, Mozart: never in install mode (the menu runs from SRAM) */
void page_install_mode(void) {}
int install_mode_key(int k) { (void)k; return 0; }
#endif

