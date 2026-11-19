/*
 * machboot supervisor
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The power-on menu, settings and Start, end to end on a PC
 * (docs/power-on.md): the RP2040's own sv.c, ecp5.c, msc.c, xlink.c and
 * store.c against a model module -- an ECP5's JTAG configuration logic
 * (ecp5_model.h) whose FPGA runs machboot's real fw/xlink.c once the
 * menu is loaded, and a system that does not answer on the link after a
 * reset -- with the RP2040's flash in RAM.
 *
 * The two sides take turns as on the board: the firmware's clock reads
 * run the RP2040's main loop, the RP2040's waits run the firmware.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "sv.h"
#include "ecp5.h"
#include "msc.h"
#include "store.h"

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
		rp_once();		/* the RP2040 runs on meanwhile */
	return now_ms;
}

#include "ecp5_model.h"
#include "xlink_model.h"

/* -- the RP2040's flash: the menu and the settings ----------------------- */

static uint8_t menu_area[MENU_HDR + 64 * 1024];
static uint8_t store_area[STORE_SIZE];
static int erases, programs;
static void st_erase(uint32_t off) { memset(store_area + off, 0xFF, STORE_SECTOR); erases++; }
static void st_program(uint32_t off, const uint8_t *page) { for (int i = 0; i < STORE_PAGE; i++) store_area[off + i] &= page[i]; programs++; }
static const store_io_t sio = { store_area, st_erase, st_program };

static uint32_t crc32(const uint8_t *p, uint32_t n)
{
	uint32_t c = 0xFFFFFFFFu;
	while (n--) { c ^= *p++; for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1; }
	return ~c;
}

static void make_menu(uint32_t idcode, uint32_t len)
{
	memset(menu_area, 0xFF, sizeof(menu_area));
	uint8_t *bit = menu_area + MENU_HDR;
	for (uint32_t i = 0; i < len; i++) bit[i] = (uint8_t)(i * 7 + 1);
	menu_hdr_t h;
	memset(&h, 0, sizeof(h));
	memcpy(h.magic, MENU_MAGIC, 4);
	h.version = 1; h.idcode = idcode; h.length = len; h.crc = crc32(bit, len); h.board = 0x202;
	strcpy(h.name, "machboot test menu");
	memcpy(menu_area, &h, sizeof(h));
}

/* -- the board around it ---------------------------------------------------- */

static int pc_on, pc_now, edge;
static uint32_t ms(void) { return now_ms; }
static int pc_active(void) { return pc_on; }
static int pc_wants(void) { return pc_now; }
static int reset_edge(void) { int e = edge; edge = 0; return e; }
static int reset_low(void) { return 0; }
static void say(const char *s) { printf("     [sv: %s]\n", s); }
static const sv_io_t sv_io = { menu_area, sizeof(menu_area), ms, pc_active, pc_wants, reset_edge, reset_low, 0, say };

static int settings_get(char *b, int cap) { return store_get(b, cap); }
static void settings_put(const char *t, int n) { store_put(t, n); }
static const msc_hooks_t hooks = { settings_get, settings_put, sv_start, 0, 0, 0, 0 };

static void rp_once(void)
{
	in_rp = 1;
	msc_tick();
	sv_tick();
	in_rp = 0;
}

/* the FPGA after a load: machboot boots (its firmware's start) */
static int seen_done;
static void fpga_follow(void)
{
	if (done && !seen_done) {
		seen_done = 1;
		fpga_machboot = 1;
		rx_full = tx_armed = 0;
		memset(&usbd, 0, sizeof(usbd));
		settings_remote = settings_dirty = 0;
		cfg.autostart = 1; cfg.wait_s = 3; cfg.kbd = 0;	/* machboot's defaults */
		usbd_init();			/* waits for the RP2040's settings */
	}
}

/* a reset on SYS_RST_N: the FPGA reconfigures from the module's flash */
static void reset_pulse(void)
{
	edge = 1;
	fpga_machboot = 0;
	done = 0;
	seen_done = 0;
}

static void run(uint32_t ms_n)
{
	uint32_t end = now_ms + ms_n;
	while (now_ms < end) {
		now_ms++;
		rp_once();
		fpga_follow();
		if (fpga_machboot) firmware();
	}
}

int main(void)
{
	char txt[256];
	sd.present = 1;
	sd.sectors = 4096;
	mb.features = FEAT_RPLINK;
	jio_init();
	memset(store_area, 0xFF, sizeof(store_area));
	store_init(&sio);
	store_put("boot.auto=1\nboot.wait=7\nboot.kbd=de\n", 36);
	msc_hooks(&hooks);

	/* 1. power on, a menu stored: loaded, machboot runs, gets its settings */
	make_menu(0x41112043u, 40000);
	sv_init(&sv_io, 1);
	CHECK(sv_menu_ok(), "a menu is stored, its CRC right");
	run(50);
	CHECK(sv_loads() == 1 && done, "power on: the menu is loaded into the FPGA");
	CHECK(burst_bits == 40000 * 8, "  all of it, through the burst");
	run(500);
	CHECK(fpga_machboot && usbd.rp2040 == 2, "  machboot runs and the RP2040 answers");
	CHECK(cfg.kbd == 1 && cfg.wait_s == 7 && cfg.autostart == 1, "  machboot has the settings the RP2040 keeps (before its countdown)");
	CHECK(settings_remote, "  and keeps them there from now on");

	/* 2. a setting changed in the menu: the RP2040 keeps the new text */
	cfg.wait_s = 2;
	settings_dirty = 1;			/* settings_save(), as the Settings page does */
	run(200);
	int n = store_get(txt, sizeof(txt)); txt[n] = 0;
	CHECK(strstr(txt, "boot.wait=2") != 0, "a change in the menu: kept by the RP2040");
	CHECK(!settings_dirty, "  and machboot knows it was taken");
	int p0 = programs;
	settings_dirty = 1;			/* saved again, unchanged */
	run(200);
	CHECK(programs == p0, "  saved again unchanged: no flash write");
	store_init(&sio);			/* a power cycle */
	n = store_get(txt, sizeof(txt)); txt[n] = 0;
	CHECK(strstr(txt, "boot.wait=2") != 0, "  still there after a power cycle");

	/* 3. Start: machboot tells the RP2040, then pulls PROGRAMN */
	int f0;
	usbd_before_start();
	reset_pulse();				/* PROGRAMN, through SYS_RST_N */
	run(100);
	CHECK(sv_loads() == 1, "Start: that reset boots the module's flash, no menu");
	f0 = frames_total;
	run(10000);
	CHECK(frames_total == f0, "  10 s of the system running: the RP2040 does not touch XA-XD");

	/* 4. the system reboots (or the reset button): the menu again */
	reset_pulse();
	run(50);
	CHECK(sv_loads() == 2, "a reboot of the system: the menu again");
	run(500);
	CHECK(fpga_machboot && usbd.rp2040 == 2 && cfg.wait_s == 2, "  machboot, with the settings as last changed");

	/* 5. without START, Start's reset is a reset like any other */
	reset_pulse();
	run(50);
	CHECK(sv_loads() == 3, "a reset machboot did not announce: the menu");
	run(500);

	/* 6. the PC uses JTAG: no load, and the probing after it is short */
	pc_on = 1;
	reset_pulse();
	run(100);
	CHECK(sv_loads() == 3, "a reset while the PC uses JTAG: the PC's, no menu");
	f0 = frames_total;
	pc_on = 0;
	run(10000);
	CHECK(frames_total - f0 <= 12, "  the PC done: a few probes (the PC may have loaded machboot), then quiet");
	printf("     (%d probe frames)\n", frames_total - f0);

	/* 7. the PC wants JTAG in the middle of our load: it gets it */
	reset_pulse();
	pc_now = 0;
	{
		/* stop the load after a few chunks */
		uint32_t b0 = burst_bits;
		(void)b0;
	}
	make_menu(0x41112043u, 60000);		/* longer: several chunks */
	sv_init(&sv_io, 0);
	edge = 1;
	pc_now = 1;
	run(10);
	CHECK(sv_last() == ECP5_ABORTED && !done, "the PC wants JTAG during a load: the load stops");
	pc_now = 0;

	/* 8. a menu for another FPGA (the UF2 of another module): not loaded */
	make_menu(0x41111043u, 40000);
	sv_init(&sv_io, 1);
	run(50);
	CHECK(sv_last() == ECP5_ERR_IDCODE && !done, "a menu for a 25F on a 45F: not loaded");

	/* 8b. at power-on the FPGA is busy with its flash and refuses the first
	   ISC_ENABLE: the load is tried again, and works */
	make_menu(0x41112043u, 40000);
	refuse_enable = 1;
	isc_en = 0;				/* section 7's stopped load left it in ISC mode */
	sv_init(&sv_io, 1);
	int l1 = sv_loads();
	run(50);
	CHECK(sv_loads() == l1, "the first ISC_ENABLE refused: no menu yet");
	run(1000);
	CHECK(sv_loads() == l1 + 1 && done, "  half a second later, tried again: loaded");
	refuse_enable = 0;
	run(500);

	/* 9. no menu (machboot-sv.uf2 alone): nothing at power-on */
	memset(menu_area, 0xFF, MENU_HDR);
	sv_init(&sv_io, 1);
	int l0 = sv_loads();
	run(100);
	CHECK(!sv_menu_ok() && sv_loads() == l0, "no menu stored: power-on loads nothing");

	/* 10. a damaged menu (one byte): refused */
	make_menu(0x41112043u, 40000);
	menu_area[MENU_HDR + 1234] ^= 1;
	sv_init(&sv_io, 1);
	CHECK(!sv_menu_ok(), "a damaged menu (CRC): not used");

	printf(fails ? "FAIL power-on: %d failures\n" : "ok   power-on, settings, Start: 0 failures\n", fails);
	return fails != 0;
}
