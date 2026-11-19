/*
 * machboot supervisor
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The menu at power-on: sv.h.
 */

#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include "sv.h"
#include "ecp5.h"
#include "msc.h"

#define T_PASS_MS	3000		/* how long a START lets a reset through */

static const sv_io_t *io;
static int menu_ok, want_load, pc_was, loads, last = -1;
static uint32_t pass_until;
/* a load that failed for the FPGA's reasons -- at power-on it is busy
   loading its own flash for a second or two -- is tried again every
   300 ms for about 5 s; once the module's system runs, ISC_ENABLE takes
   (that is how openFPGALoader loads a running FPGA) */
static int retries;
static uint32_t retry_at;
#define T_RETRY_MS	300
#define N_RETRIES	16
static int pass;
/* a cached system to load (sv_run) */
static const uint8_t *run_bit;
static uint32_t run_len, run_idcode;
static char run_name[24];
static char note[100];
#define RUN_BLANK_MS	500	/* the FPGA blank before a system from the cache goes in */

static uint32_t crc32(const uint8_t *p, uint32_t n)
{
	static uint32_t tab[256];
	if (!tab[1])
		for (uint32_t i = 0; i < 256; i++) {
			uint32_t c = i;
			for (int k = 0; k < 8; k++)
				c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
			tab[i] = c;
		}
	uint32_t c = 0xFFFFFFFFu;
	while (n--)
		c = tab[(c ^ *p++) & 0xff] ^ (c >> 8);
	return ~c;
}

static void say(const char *fmt, ...)
{
	char b[120];
	va_list ap;
	if (!io->log)
		return;
	va_start(ap, fmt);
	vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	io->log(b);
}

void sv_init(const sv_io_t *dev, int power_on)
{
	io = dev;
	menu_ok = 0;
	want_load = 0;
	pass = 0;
	loads = 0;
	retries = 0;
	const menu_hdr_t *h = (const menu_hdr_t *)io->menu;
	if (io->menu && !memcmp(h->magic, MENU_MAGIC, 4) && h->version == 1 &&
	    h->length && h->length <= io->menu_max - MENU_HDR &&
	    crc32(io->menu + MENU_HDR, h->length) == h->crc)
		menu_ok = 1;
	if (menu_ok)
		say("menu: %.40s, %u bytes for IDCODE %08x, CRC right", h->name, (unsigned)h->length, (unsigned)h->idcode);
	else if (io->menu && !memcmp(h->magic, MENU_MAGIC, 4))
		say("menu: stored but damaged (length %u, CRC wrong): not used", (unsigned)h->length);
	else
		say("menu: none stored (machboot-sv.uf2, or a menu UF2 not copied yet)");
	if (power_on && menu_ok)
		want_load = 1;
	if (!power_on && menu_ok)
		say("not a power-on: the menu waits for a reset of the module, a power cycle, or a serial break");
}

int sv_menu_ok(void) { return menu_ok; }
const menu_hdr_t *sv_menu(void) { return menu_ok ? (const menu_hdr_t *)io->menu : 0; }
int sv_loads(void) { return loads; }
int sv_last(void) { return last; }

void sv_request(void)
{
	if (menu_ok) {
		want_load = 1;
		say("menu: asked for (serial break)");
	} else {
		say("menu: asked for, but none is stored");
	}
}

void sv_start(void)
{
	pass = 1;
	pass_until = io->ms() + T_PASS_MS;
}

static int yield(void)
{
	if (io->poll)
		io->poll();
	return io->pc_wants && io->pc_wants();
}

static void load(void)
{
	const menu_hdr_t *h = (const menu_hdr_t *)io->menu;
	want_load = 0;
	msc_link_drop();			/* whatever answered on the link is going */
	uint32_t t0 = io->ms();
	last = ecp5_load(io->menu + MENU_HDR, h->length, h->idcode, yield);
	say("menu: load: %s (IDCODE read %08x, %u ms)", ecp5_error(last), (unsigned)ecp5_last_idcode,
	    (unsigned)(io->ms() - t0));
	if (last != ECP5_OK && last != ECP5_ERR_IDCODE && last != ECP5_ABORTED && retries < N_RETRIES) {
		retries++;
		retry_at = io->ms() + T_RETRY_MS;
		want_load = 1;
		say("menu: try %d failed; again in %d ms", retries, T_RETRY_MS);
		return;
	}
	retries = 0;
	if (last == ECP5_OK) {
		loads++;
		say("menu: loaded");
		msc_link_event();		/* machboot will answer in a moment */
	} else {
		say(last == ECP5_ERR_IDCODE ? "menu: not for this module's FPGA (the UF2 for another module?)" :
		    last == ECP5_ABORTED ? "menu: the PC took JTAG" : "menu: the load failed");
	}
}

void sv_run(const uint8_t *bit, uint32_t len, uint32_t idcode, const char *name)
{
	run_bit = bit;
	run_len = len;
	run_idcode = idcode;
	strncpy(run_name, name, sizeof(run_name) - 1);
	run_name[sizeof(run_name) - 1] = 0;
	say("run: %s asked for (%u bytes)", run_name, (unsigned)len);
}

void sv_refuse(const char *name, const char *why)
{
	snprintf(note, sizeof(note), "%s did not start (%s)", name, why);
	say("run: %s", note);
	if (menu_ok) {
		want_load = 1;		/* machboot shows it once the menu is back */
		retries = 0;
	}
}

int sv_note(char *buf, int cap)
{
	int n = (int)strlen(note);
	if (!n || n >= cap)
		return 0;
	memcpy(buf, note, (size_t)n);
	note[0] = 0;
	return n;
}

static void run(void)
{
	const uint8_t *b = run_bit;
	run_bit = 0;
	msc_link_drop();			/* machboot is going */
	uint32_t id = run_idcode ? run_idcode : ecp5_idcode();
	uint32_t t0 = io->ms();
	/* as near to a cold start as the board allows: machboot (and the DDR3
	   it held in reset) gone, the FPGA blank, every pin floating, for a
	   moment -- then the system (Mach86's DDR3 training needs this) */
	last = ecp5_blank(id);
	if (last == ECP5_OK) {
		uint32_t tb = io->ms();
		while (io->ms() - tb < RUN_BLANK_MS)
			if (yield()) {
				last = ECP5_ABORTED;
				break;
			}
	}
	if (last == ECP5_OK)
		last = ecp5_load(b, run_len, id, yield);
	say("run: %s: %s (IDCODE read %08x, %u ms)", run_name, ecp5_error(last),
	    (unsigned)ecp5_last_idcode, (unsigned)(io->ms() - t0));
	if (last == ECP5_OK)
		return;				/* the system runs; its reset brings the menu */
	if (last == ECP5_ABORTED) {
		say("run: the PC took JTAG");
		return;
	}
	snprintf(note, sizeof(note), "%s did not start (%s)", run_name,
	         last == ECP5_ERR_IDCODE ? "not for this FPGA" :
	         last == ECP5_ERR_CONFIG ? "the FPGA rejected it" :
	         last == ECP5_ERR_DONE ? "loaded, but the FPGA did not start it" : ecp5_error(last));
	if (menu_ok) {
		want_load = 1;
		retries = 0;
	}
}

void sv_tick(void)
{
	int pc = io->pc_active && io->pc_active();

	/* a PC's JTAG session ended: it may have loaded machboot itself */
	if (pc_was && !pc)
		msc_link_event();
	pc_was = pc;

	if (pass && (int32_t)(io->ms() - pass_until) >= 0)
		pass = 0;			/* the START was not followed by a reset */

	if (io->reset_edge && io->reset_edge()) {
		msc_link_drop();		/* machboot, if it was there, is gone */
		if (pass) {
			pass = 0;		/* machboot's own Start: the system boots */
			say("reset: machboot started the system");
		} else if (menu_ok && !pc) {
			want_load = 1;		/* the reset button, or the system's reboot */
			say("reset: back to the menu");
		} else if (menu_ok) {
			say("reset: a PC is using JTAG, so no menu");
		}
	}

	if (run_bit && !pc) {
		run();
		return;
	}
	if (run_bit && pc) {
		run_bit = 0;
		say("run: a PC is using JTAG; not started");
	}

	if (want_load && !pc && !(io->reset_low && io->reset_low()) &&
	    (!retries || (int32_t)(io->ms() - retry_at) >= 0))
		load();
}
