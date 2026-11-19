/*
 * machboot supervisor
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The system cache, end to end on a PC (docs/systems.md): machboot's own
 * fw/xlink.c asks (rcache.h: LIST, PUT, RUN, DROP), and the RP2040's own
 * msc.c, xlink.c, cache.c, sv.c and ecp5.c serve it, over the link model
 * (xlink_model.h), with the RP2040's flash in RAM and an ECP5 model on
 * the JTAG pins (ecp5_model.h). Also cache.c alone: a copy cut by a power
 * cut, the gaps, the store's bigger records.
 *
 * The two sides take turns as on the board: machboot's clock reads run
 * the RP2040's main loop, the RP2040's waits run machboot's link code.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "sv.h"
#include "ecp5.h"
#include "msc.h"
#include "store.h"
#include "cache.h"

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("  FAIL %s\n", what); fails++; } else printf("ok   %s\n", what); } while (0)

static uint32_t now_ms;
static int in_rp;
static void rp_once(void);
uint32_t xio_ms(void) { return now_ms; }
uint32_t ms_now(void)
{
	now_ms++;
	if (!in_rp)
		rp_once();
	return now_ms;
}

#include "ecp5_model.h"
#include "xlink_model.h"

static uint32_t crc32(const uint8_t *p, uint32_t n)
{
	uint32_t c = 0xFFFFFFFFu;
	while (n--) { c ^= *p++; for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1; }
	return ~c;
}

/* -- the RP2040's flash ------------------------------------------------------ */

#define CA_SIZE (512 * 1024)
static uint8_t cache_area[CA_SIZE];
static int ca_erases, ca_programs, ca_bad;
static void ca_erase(uint32_t off, uint32_t len)
{
	if ((off | len) % 4096 || off + len > CA_SIZE) ca_bad++;
	memset(cache_area + off, 0xFF, len);
	ca_erases++;
}
static void ca_program(uint32_t off, const uint8_t *page)
{
	if (off % 256 || off + 256 > CA_SIZE) ca_bad++;
	for (int i = 0; i < 256; i++) {
		if ((cache_area[off + i] & page[i]) != page[i]) ca_bad++;	/* programming a 0 back to 1 */
		cache_area[off + i] &= page[i];
	}
	ca_programs++;
}
static const cache_io_t cio = { cache_area, CA_SIZE, ca_erase, ca_program };

static uint8_t menu_area[MENU_HDR + 32 * 1024];
static uint8_t store_area[STORE_SIZE];
static void st_erase(uint32_t off) { memset(store_area + off, 0xFF, STORE_SECTOR); }
static void st_program(uint32_t off, const uint8_t *page) { for (int i = 0; i < STORE_PAGE; i++) store_area[off + i] &= page[i]; }
static const store_io_t sio = { store_area, st_erase, st_program };

static void make_menu(void)
{
	memset(menu_area, 0xFF, sizeof(menu_area));
	uint8_t *bit = menu_area + MENU_HDR;
	uint32_t len = 20000;
	for (uint32_t i = 0; i < len; i++) bit[i] = (uint8_t)(i * 7 + 1);
	menu_hdr_t h;
	memset(&h, 0, sizeof(h));
	memcpy(h.magic, MENU_MAGIC, 4);
	h.version = 1; h.idcode = 0x41112043u; h.length = len; h.crc = crc32(bit, len); h.board = 0x202;
	strcpy(h.name, "machboot test menu");
	memcpy(menu_area, &h, sizeof(h));
}

/* -- the board ---------------------------------------------------------------- */

/* the supervisor's clock: it moves when read, so its waits end (run()'s
   blank pause) */
static uint32_t ms(void) { return now_ms++; }
static int no(void) { return 0; }
static void say(const char *s) { printf("     [sv: %s]\n", s); }
static const sv_io_t sv_io = { menu_area, sizeof(menu_area), ms, no, no, no, no, 0, say };

static int settings_get(char *b, int cap) { return store_get(b, cap); }
static void settings_put(const char *t, int n) { store_put(t, n); }
static void run_hook(uint32_t crc)
{
	const cache_item_t *it = cache_find(crc);
	if (it)
		sv_run(cache_bitstream(it), it->len, it->idcode, it->name);
}
static void log_line(const char *s) { printf("     [msc: %s]\n", s); }
static const msc_hooks_t hooks = { settings_get, settings_put, sv_start, run_hook, sv_note, 0, log_line };

static void rp_once(void)
{
	in_rp = 1;
	msc_tick();
	sv_tick();
	in_rp = 0;
}

/* after a load: whatever was loaded runs; machboot only if it was the menu */
static int seen_done, loaded_menu;
static void fpga_follow(void)
{
	if (done && !seen_done) {
		seen_done = 1;
		fpga_machboot = loaded_menu;
		rx_full = tx_armed = 0;
		if (loaded_menu) {
			memset(&usbd, 0, sizeof(usbd));
			settings_remote = settings_dirty = 0;
			usbd_init();
		}
	}
}

static void run(uint32_t n)
{
	uint32_t end = now_ms + n;
	while (now_ms < end) {
		now_ms++;
		rp_once();
		fpga_follow();
		if (fpga_machboot) firmware();
	}
}

/* -- a bitstream file on the card: a .bit header, then the bitstream ------- */

static uint8_t file_buf[700 * 1024];
static uint32_t make_file(uint32_t idcode, uint32_t len, int seed, uint32_t *stripped_crc)
{
	static const uint8_t hdr[] = "\xff\x00Part: LFE5U-45F-8BG381\x00\xff";
	uint32_t h = sizeof(hdr) - 1, i = 0;
	memcpy(file_buf, hdr, h);
	i = h;
	uint32_t start = i;
	for (int k = 0; k < 16; k++) file_buf[i++] = 0xFF;
	file_buf[i++] = 0xBD; file_buf[i++] = 0xB3;
	uint8_t v[8] = { 0xE2, 0, 0, 0, (uint8_t)(idcode >> 24), (uint8_t)(idcode >> 16), (uint8_t)(idcode >> 8), (uint8_t)idcode };
	memcpy(file_buf + i, v, 8); i += 8;
	uint32_t x = (uint32_t)seed * 2654435761u;
	while (i < h + len) { x = x * 1103515245u + 12345u; file_buf[i++] = (uint8_t)(x >> 16); }
	file_data = file_buf;
	file_size = i;
	/* what machboot sends: from 3 bytes before the preamble */
	uint32_t from = start + 16 - 3;
	*stripped_crc = crc32(file_buf + from, file_size - from);
	return file_size - from;
}

/* what the PC's USB does mid-copy: TinyUSB's callbacks run from the
   request's poll, so here they run from inside it as well */
static int usb_seen, usb_eject_ok;
static uint8_t usb_asc;
static void usb_mid(void)
{
	uint8_t k = 0, q = 0;
	int tag0 = frames_total;
	usb_seen = 1;
	msc_ready(&k, &usb_asc, &q);
	msc_eject();
	usb_eject_ok = frames_total == tag0;	/* no frame on the link */
}

static card_entry_t ce;
static int cancel_at;
static int prog_calls;
static int progress(uint32_t d, uint32_t len)
{
	(void)len;
	prog_calls++;
	return cancel_at && d >= (uint32_t)cancel_at;
}

static void ce_named(const char *name, const char *ver)
{
	memset(&ce, 0, sizeof(ce));
	strcpy(ce.name, name);
	strcpy(ce.ver, ver);
	ce.kind = CE_SYSTEM;
	ce.has_bit = 1;
	ce.bit.size = file_size;
}

int main(void)
{
	uint32_t crc, len, scrc;
	rc_list_t l;

	/* -- cache.c alone ------------------------------------------------------- */
	memset(cache_area, 0xFF, sizeof(cache_area));
	cache_init(&cio);
	uint32_t largest, fr = cache_free(&largest);
	CHECK(fr == CA_SIZE && largest == CA_SIZE && cache_list(0, 0) == 0, "cache: empty, all of it free");
	{
		/* a copy cut half way (a power cut): invisible, its room free */
		uint8_t b[1000];
		memset(b, 0x5A, sizeof(b));
		CHECK(cache_begin(100000, "Cut", "1") == CACHE_OK, "cache: a copy begins");
		for (int k = 0; k < 50; k++) cache_data(b, sizeof(b));
		cache_init(&cio);		/* the RP2040 restarts */
		cache_item_t it[4];
		fr = cache_free(&largest);
		CHECK(cache_list(it, 4) == 0 && fr == CA_SIZE, "cache: a copy cut by a power cut is not there, its room is free");
		/* and the same room takes the next one */
		CHECK(cache_begin(3000, "Small", "1") == CACHE_OK && cache_data(b, 1000) == CACHE_OK &&
		      cache_data(b, 1000) == CACHE_OK && cache_data(b, 1000) == CACHE_OK && cache_end(&crc) == CACHE_OK,
		      "cache: written over that room, whole");
		CHECK(cache_list(it, 4) == 1 && it[0].off == 0 && it[0].len == 3000 && !strcmp(it[0].name, "Small"),
		      "cache: listed, at the start, with its name");
		CHECK(cache_begin(CA_SIZE, "Huge", "1") == CACHE_NOROOM, "cache: bigger than the room: NOROOM, nothing erased");
		CHECK(cache_list(it, 4) == 1, "  the one there is untouched");
		CHECK(cache_drop(crc) == CACHE_OK && cache_list(it, 4) == 0 && cache_free(0) == CA_SIZE, "cache: dropped: gone, room free");
		CHECK(!ca_bad, "cache: every erase whole sectors, every program onto erased flash");
	}

	/* -- the store: records big enough for the list --------------------------- */
	memset(store_area, 0xFF, sizeof(store_area));
	store_init(&sio);
	{
		char big[STORE_MAX], back[STORE_MAX + 1];
		for (int i = 0; i < 700; i++) big[i] = (char)('a' + i % 26);
		store_put(big, 700);
		for (int k = 0; k < 40; k++) store_put(big, 700);	/* round the log a few times */
		store_init(&sio);
		int n = store_get(back, sizeof(back));
		CHECK(n == 700 && !memcmp(back, big, 700), "store: a 700-byte text kept whole, after 41 saves and a restart");
	}
	memset(store_area, 0xFF, sizeof(store_area));
	store_init(&sio);

	/* -- end to end: machboot asks, the RP2040 does it -------------------------- */
	sd.present = 1;
	sd.sectors = 4096;
	mb.features = FEAT_RPLINK;
	mb.idcode = 0x41112043u;
	jio_init();
	memset(cache_area, 0xFF, sizeof(cache_area));
	cache_init(&cio);
	make_menu();
	msc_hooks(&hooks);
	sv_init(&sv_io, 1);
	loaded_menu = 1;
	run(3000);
	CHECK(rc_available(), "machboot sees the supervisor");

	CHECK(rc_list(&l) == XL_R_OK && l.n == 0 && l.size == CA_SIZE && l.free == CA_SIZE, "LIST: empty, its size");

	len = make_file(0x41112043u, 200000, 1, &scrc);
	ce_named("Mach86", "r31");
	int r = rc_put(&ce, &crc, &len, progress);
	CHECK(r == XL_R_OK && crc == scrc, "PUT: copied, the CRC of the bitstream from its preamble");
	const cache_item_t *it = cache_find(crc);
	CHECK(it && it->len == len && !memcmp(cache_bitstream(it), file_buf + (file_size - len), len),
	      "  the RP2040's flash holds it byte for byte, the .bit header left behind");
	CHECK(it && it->idcode == 0x41112043u && !strcmp(it->name, "Mach86") && !strcmp(it->ver, "r31"), "  its IDCODE, name and version");
	CHECK(prog_calls >= 1, "  progress was reported (here the whole copy runs in one clock read)");
	CHECK(rc_list(&l) == XL_R_OK && l.n == 1 && l.it[0].crc == crc && !strcmp(l.it[0].name, "Mach86") &&
	      l.free == CA_SIZE - ((256 + len + 4095) & ~4095u), "LIST: it, and the room it takes");
	uint32_t m86 = crc, m86_len = it ? it->len : 0;

	/* cancelled half way: nothing new in the list, and the room is free */
	make_file(0x41112043u, 250000, 2, &scrc);
	ce_named("Kakao", "0.3");
	file_cancel_at = 100000;		/* Esc, a third of the way */
	r = rc_put(&ce, &crc, &len, progress);
	file_cancel_at = 0;
	CHECK(r == XL_R_CANCELLED, "PUT cancelled (Esc): CANCELLED");
	CHECK(rc_list(&l) == XL_R_OK && l.n == 1 && l.it[0].crc == m86, "  the cache lists only what was there");
	r = rc_put(&ce, &crc, &len, progress);
	CHECK(r == XL_R_OK && crc == scrc && rc_list(&l) == XL_R_OK && l.n == 2, "PUT again: copied, both listed");
	uint32_t kakao = crc;

	/* too big for what is left */
	make_file(0x41112043u, 300000, 3, &scrc);
	ce_named("Big", "1");
	CHECK(rc_put(&ce, &crc, &len, progress) == XL_R_NOROOM, "PUT with no room: NOROOM");
	CHECK(rc_list(&l) == XL_R_OK && l.n == 2, "  nothing changed");

	/* the PC's USB in the middle of a copy (seen on Sergei: "the RP2040
	   did not answer" at 26 %, the card reader seemed to wake): the card
	   reader looks as before, and an eject does not touch the link */
	{
		CHECK(rc_drop(kakao) == XL_R_OK, "(Kakao out again, to copy it once more)");
		make_file(0x41112043u, 250000, 2, &scrc);
		ce_named("Kakao", "0.3");
		file_usb = usb_mid;
		file_usb_at = 60000;
		r = rc_put(&ce, &crc, &len, progress);
		file_usb = 0;
		CHECK(usb_seen && usb_asc == 0x3A, "  mid-copy, TEST UNIT READY says no medium, as before the copy");
		CHECK(usb_eject_ok, "  mid-copy, an eject from the PC goes nowhere near the link");
		CHECK(r == XL_R_OK && crc == scrc, "  and the copy finishes, whole");
	}

	/* a bitstream for the 25F: machboot refuses it before anything is asked */
	make_file(0x41111043u, 50000, 4, &scrc);
	ce_named("Other", "1");
	CHECK(rc_put(&ce, &crc, &len, progress) == RC_WRONG_FPGA, "PUT of a 25F's bitstream on a 45F: refused by machboot");

	/* while a PUT runs, the PC's card reader waits */
	{
		uint8_t key = 0, asc = 0, ascq = 0;
		CHECK(msc_ready(&key, &asc, &ascq) == 0 && asc == 0x3A, "card reader: not shared, no medium (as before)");
	}

	/* DROP */
	CHECK(rc_drop(kakao) == XL_R_OK && rc_list(&l) == XL_R_OK && l.n == 1, "DROP: erased, not listed");
	CHECK(rc_drop(kakao) == XL_R_NOTFOUND, "DROP again: NOTFOUND");

	/* RUN: the RP2040 loads it over JTAG; machboot is gone */
	seen_done = 0;
	done = 0;
	loaded_menu = 0;
	CHECK(rc_run(m86) == XL_R_OK, "RUN: accepted");
	run(3000);
	CHECK(done && burst_bits == m86_len * 8, "  the FPGA was loaded with it, every bit");
	CHECK(!fpga_machboot, "  machboot is gone (the system runs)");

	/* a reset now brings the menu; machboot again, RUN a broken one */
	{
		/* one the FPGA refuses: its IDCODE in the cache's header is another die's */
		len = make_file(0x41111043u, 30000, 5, &scrc);
		uint32_t at = 0;
		uint8_t hb[4096];
		(void)hb;
		CHECK(cache_begin(len, "Broken", "1") == CACHE_OK && cache_data(file_buf + (file_size - len), len) == CACHE_OK &&
		      cache_end(&at) == CACHE_OK, "a 25F bitstream put into the cache directly");
		uint32_t broken = at;
		seen_done = 0;
		done = 0;
		loaded_menu = 1;
		sv_request();			/* (as a reset would: the menu) */
		run(3000);
		CHECK(rc_available(), "the menu again, machboot answers");
		loaded_menu = 1;
		seen_done = 0;
		rc_note_clear();
		CHECK(rc_run(broken) == XL_R_OK, "RUN of the broken one: accepted");
		run(4000);
		CHECK(rc_note() && strstr(rc_note(), "Broken did not start"), "  the menu came back, and machboot was told why");
		if (rc_note())
			printf("     (note: %s)\n", rc_note());
	}
	CHECK(!ca_bad, "every erase and program of the cache was a proper one");
	printf("     (%d erases, %d page programs)\n", ca_erases, ca_programs);

	printf(fails ? "FAIL systems: %d failures\n" : "ok   systems: 0 failures\n", fails);
	return fails != 0;
}
