/*
 * machboot
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The list of systems in the menu (fw/systems.h, docs/systems.md): what
 * stays resident -- starting one (copying it first if it must be), the
 * bar while it copies, the countdown's line, the hint lines both pages
 * use. The pages are fw/page_systems.c (Start) and fw/page_sysinst.c
 * (Install). MB_SYSTEMS builds only.
 *
 * Whatever writes flash says so before it happens, and how long it takes.
 * Moving systems in the list and the settings are not announced.
 */

#include "ui_int.h"
#include "systems.h"

char sys_msg[80];		/* the outcome of the last thing done, on Start */
int sys_msg_ok;
int rm_ask;			/* Start: "Remove ...? Y/N" */
int startup_copying;		/* the startup screen: Esc cancels */

int multi(void)
{
	return systems_on() && !mb.install_mode;
}

const char *sys_where(const sysent_t *e)
{
	if (e->module)
#ifdef MB_SLOT
		return "in flash";
#else
		return "module";
#endif
	if (e->cached)
		return "in flash";
	if (e->ce)
		return e->ce->usb ? "on the USB drive" : "on the card";
	return "not found";
}

const char *src_name(const card_entry_t *ce)
{
	return ce->usb ? "the USB drive" : "the SD card";
}

/* "about 7 s": the copy, and the start */
uint32_t about_s(uint32_t len)
{
	return systems_copy_s(len) + 1;
}

/* the lines saying what Enter will write: returns the next row */
int copy_hint(int y, const char *verb_line, card_entry_t *ce, uint32_t self)
{
	char buf[96], gone[64];
	text_at(y++, verb_line, C_WHITE);
	int n = systems_plan(ce->bit.size, self, gone, sizeof(gone));
	if (n < 0) {
#ifdef MB_SLOT
		text_at(y++, "It does not fit into the system slot.", C_LRED);
#else
		text_at(y++, "It does not fit into the RP2040's flash.", C_LRED);
#endif
	} else if (n > 0) {
#ifdef MB_SLOT
		mb_snprintf(buf, sizeof(buf), "It replaces %s in flash.", gone);
#else
		mb_snprintf(buf, sizeof(buf), "To make room, %s %s removed from flash.", gone, n == 1 ? "is" : "are");
#endif
		text_at(y++, buf, C_YELLOW);
		if (systems_plan_lost())
			text_at(y++, "It is not on the card: put it back there first.", C_LRED);
		else
			text_at(y++, "It is copied back when it is started again.", C_GREY);
	}
	return y;
}

/* -- copying, with the bar ---------------------------------------------------- */

static const char *cp_name;
static int cp_last, cp_inline;		/* inline: on the Install page */
static uint32_t cp_t;

static int copy_progress(uint32_t done, uint32_t len)
{
	int pm = len >= 1000 ? (int)(done / (len / 1000)) : 0;
	if (pm > 1000)
		pm = 1000;
#ifdef MB_SLOT
	/* install() checks first (Esc stops it), then writes (it cannot stop) */
	startup_copying = pm < 300 ? 1 : 2;
#endif
	if (pm / 5 != cp_last && ms_now() - cp_t >= 100) {
		cp_last = pm / 5;
		cp_t = ms_now();
		if (cp_inline) {
			inst_permille = pm;
			redraw();
		} else {
			char msg[80];
#ifdef MB_SLOT
			if (pm < 300)
				mb_snprintf(msg, sizeof(msg), "Comparing %s on the SD card with flash ... %d %%", cp_name, pm / 10);
			else
				mb_snprintf(msg, sizeof(msg), "Copying %s from the SD card into flash ... %d %%", cp_name, pm / 10);
#else
			mb_snprintf(msg, sizeof(msg), "Copying %s from the SD card into flash ... %d %%", cp_name, pm / 10);
#endif
			draw_startup(msg, C_WHITE, pm, 1);
		}
	}
	int k = key_poll();
	return k == KEY_ESC;
}

int do_copy(sysent_t *e, card_entry_t *ce, int inline_)
{
	cp_name = ce->name;
	cp_last = -1;
	cp_t = 0;
	cp_inline = inline_;
	startup_copying = 1;
	con_printf("systems: copying %s %s from %s into flash, %u bytes\n", ce->name, ce->ver, src_name(ce), ce->bit.size);
	int r = e ? systems_copy(e, copy_progress) : systems_add(ce, copy_progress);
	startup_copying = 0;
	con_printf("systems: %s: %s\n", ce->name, rc_error(r));
	return r;
}

/* start the i-th: copy it first if it must be */
void start_entry(int i)
{
	char name[24];
	sysent_t *e = &sys_list[i];
	sys_msg[0] = 0;
	if (e->module) {
		boot_module();
		return;
	}
	if (!systems_ready(e))
		return;
	memcpy(name, e->name, sizeof(name));
	rc_note_clear();
	if (systems_needs_copy(e)) {
		card_entry_t *ce = e->ce;
		int r = do_copy(e, ce, 0);
		if (r != XL_R_OK) {
			if (r == XL_R_CANCELLED)
				mb_snprintf(sys_msg, sizeof(sys_msg), "Cancelled: nothing was written or started.");
			else
				mb_snprintf(sys_msg, sizeof(sys_msg), "%s was not copied: %s.", name, rc_error(r));
			sys_msg_ok = 0;
			return;
		}
		e = systems_find(name);
		if (!e)
			return;
	}
#ifdef MB_SLOT
	if (e->module)
		boot_module();		/* it is in the slot now: PROGRAMN, as ever */
	else {
		mb_snprintf(sys_msg, sizeof(sys_msg), "%s is not in flash after the copy.", name);
		sys_msg_ok = 0;
	}
#else
	{
		char msg[80];
		mb_snprintf(msg, sizeof(msg), "Starting %s ...", name);
		draw_startup(msg, C_WHITE, 1000, 1);
		con_printf("%s (from the RP2040's flash, over JTAG)\n", msg);
		int r = rc_run(e->crc);
		if (r != XL_R_OK) {
			mb_snprintf(sys_msg, sizeof(sys_msg), "%s did not start: %s.", name, rc_error(r));
			sys_msg_ok = 0;
			systems_refresh();
			return;
		}
		/* the RP2040 loads it into the FPGA now: machboot is gone in a moment */
		ms_sleep(3000);
		mb_snprintf(sys_msg, sizeof(sys_msg), "%s did not start: the RP2040 did not load it.", name);
		sys_msg_ok = 0;
		con_printf("%s\n", sys_msg);
	}
#endif
}

/* the countdown's line, for the top system: 0 if it cannot start by itself */
int systems_countdown(char *msg, int cap, int left)
{
	if (!sys_n || rc_note())
		return 0;
	const sysent_t *e = &sys_list[0];
	if (!systems_ready(e))
		return 0;
	if (systems_needs_copy(e))
		mb_snprintf(msg, (size_t)cap, "Copying %s from the SD card into flash, then starting it, in %d ...", e->name, left);
	else
		mb_snprintf(msg, (size_t)cap, "Starting %s in %d ...", e->name, left);
	return 1;
}

#ifdef MB_SLOT
/* no RP2040 here: nothing to note */
const char *rc_note(void) { return 0; }
void rc_note_clear(void) {}
#endif
