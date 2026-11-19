/*
 * machboot supervisor
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The card reader end to end, on a PC: the RP2040's src/xlink.c and
 * src/msc.c, the FPGA firmware's own fw/xlink.c, and between them a
 * model of rtl/mb_xlink.v at the byte level (the bit level, against the
 * RTL itself, is tests/xlink_tb.cc). The SD card is a RAM disk.
 *
 *   cc -DMB_HOST -Irp2040/src -Ifw rp2040/tests/test_xlink.c rp2040/src/xlink.c \
 *      rp2040/src/msc.c fw/xlink.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "xlink.h"
#include "xlink_io.h"
#include "msc.h"
#include "usbd.h"
#include "hw.h"
#include "mb.h"
#include "sd.h"
#include "settings.h"
#include <stdarg.h>

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("  FAIL %s\n", what); fails++; } } while (0)

/* -- time ------------------------------------------------------------------ */

static uint32_t now_ms;
uint32_t xio_ms(void) { return now_ms; }
uint32_t ms_now(void) { return now_ms++; }	/* the firmware's waits let time pass */

/* -- the FPGA: mb_xlink.v's state, and machboot's firmware ----------------- */

#include "xlink_model.h"

/* -- the tests ------------------------------------------------------------- */

static void ui_share(int on)		/* the menu's Card reader page */
{
	usbd_msc_share(on);
}

int main(void)
{
	uint8_t k, a, q, buf[16 * 512], buf2[16 * 512];
	for (size_t i = 0; i < sizeof(disk); i++) disk[i] = (uint8_t)(i * 13 + (i >> 9));
	sd.present = 1;
	sd.sectors = 4096;
	mb.features = FEAT_RPLINK;

	/* another image in the FPGA: nothing answers, the pins are let go */
	fpga_machboot = 0;
	CHECK(!msc_ready(&k, &a, &q) && k == 0x02 && a == 0x3A, "no machboot: no medium");
	CHECK(!attached, "no machboot: the pins are released after the probe");
	CHECK(!msc_link(), "no machboot: no link");

	/* machboot, the card not shared */
	fpga_machboot = 1;
	CHECK(usbd_init() == 1 && usbd.rp2040 == 1, "the firmware finds the link, the RP2040 not yet heard");
	CHECK(!msc_ready(&k, &a, &q) && a == 0x3A && !attached, "before any load: no medium, and no probing");
	msc_link_event();			/* something loaded machboot */
	for (int i = 0; i < 200 && msc_link_state() != 2; i++) { now_ms++; msc_tick(); firmware(); }
	CHECK(msc_link_state() == 2, "after a load: the probe finds machboot");
	CHECK(!msc_ready(&k, &a, &q) && a == 0x3A, "not shared: no medium");
	CHECK(msc_link() && usbd.rp2040 == 2, "the link is up, both ways");
	msc_set_pc(1);
	now_ms += 1000; msc_tick();
	CHECK(usbd.configured == 1, "the firmware knows a PC is there (HELLO)");
	CHECK(msc_read(0, buf, 1) == -1, "READ before sharing: no medium");

	/* shared: UNIT ATTENTION once, then ready, the card's size */
	ui_share(1);
	CHECK(!msc_ready(&k, &a, &q) && k == 0x06 && a == 0x28, "shared: UNIT ATTENTION, medium changed");
	CHECK(msc_ready(&k, &a, &q), "then ready");
	CHECK(msc_sectors() == 4096, "READ CAPACITY: the card's sectors");

	CHECK(msc_read(10, buf, 7) == 0 && !memcmp(buf, disk + 10 * 512, 7 * 512), "READ 7 sectors (3+3+1): the card's bytes");
	CHECK(msc_read(4090, buf, 6) == 0 && !memcmp(buf, disk + 4090 * 512, 6 * 512), "READ the last 6 sectors");
	CHECK(msc_read(4095, buf, 2) == -2, "READ past the end: an error");
	CHECK(usbd.msc_read == 14 * 512, "the menu counts what was read (13, and sector 4095 before the error)");

	for (int i = 0; i < 5 * 512; i++) buf2[i] = (uint8_t)(i ^ 0x5A);
	CHECK(msc_write(100, buf2, 5) == 0 && !memcmp(disk + 100 * 512, buf2, 5 * 512), "WRITE 5 sectors: on the card");
	CHECK(disk_writes == 5 && usbd.msc_written == 5 * 512, "five sector writes, counted");

	/* a slow card: the RP2040 waits, polling */
	fw_delay = 40;
	CHECK(msc_read(200, buf, 3) == 0 && !memcmp(buf, disk + 200 * 512, 3 * 512), "a slow reply: waited for");

	/* a call that gives up, then the next one still works (the late reply is drained) */
	fw_hold = 1;
	uint32_t t0 = now_ms;
	CHECK(msc_read(300, buf, 1) == -2, "the FPGA busy for 2 s: the call gives up");
	CHECK(now_ms - t0 >= 2000 && now_ms - t0 < 2100, "after about 2 s");
	fw_hold = 0;
	firmware();			/* the late reply */
	CHECK(msc_read(301, buf, 2) == 0 && !memcmp(buf, disk + 301 * 512, 2 * 512), "the next call works");

	/* a long sequential read, as the PC does it: one 3-sector callback at a
	   time, the main loop's fetch-ahead between (while USB would send) */
	int ok = 1, r0 = disk_reads;
	for (uint32_t l = 1000; l < 1000 + 90; l += 3) {
		if (msc_read(l, buf, 3) || memcmp(buf, disk + l * 512, 3 * 512)) ok = 0;
		msc_background();
		for (int i = 0; i < 3; i++) firmware();	/* the FPGA reads ahead meanwhile */
	}
	CHECK(ok, "90 sectors read in sequence, through both read-aheads: the card's bytes");
	CHECK(disk_reads - r0 <= 90 + 6, "each sector read from the card once (plus the read-ahead past the end)");

	/* a write into what both sides have read ahead: never the old data */
	CHECK(msc_read(500, buf, 3) == 0, "READ 500-502");
	msc_background();
	for (int i = 0; i < 3; i++) firmware();
	for (int i = 0; i < 512; i++) buf2[i] = (uint8_t)(0xC3 ^ i);
	CHECK(msc_write(503, buf2, 1) == 0, "WRITE 503, which both sides had read ahead");
	CHECK(msc_read(503, buf, 3) == 0 && !memcmp(buf, buf2, 512) && !memcmp(buf + 512, disk + 504 * 512, 1024),
	      "READ 503 after it: the new data, not the read-ahead's");
	/* the FPGA has read ahead past what the RP2040 holds (506-508 now) */
	CHECK(msc_read(500, buf, 3) == 0, "READ 500-502 again");
	msc_background();		/* the RP2040: 503-505 */
	for (int i = 0; i < 3; i++) firmware();	/* the FPGA: 506-508 */
	for (int i = 0; i < 512; i++) buf2[i] = (uint8_t)(0x3C ^ i);
	CHECK(msc_write(506, buf2, 1) == 0, "WRITE 506, in the FPGA's read-ahead");
	CHECK(msc_read(506, buf, 1) == 0 && !memcmp(buf, buf2, 512), "READ 506 after it: the new data");

	/* the PC ejects: the menu sees it, the medium is gone */
	msc_eject();
	CHECK(!usbd.msc_shared, "EJECT: the card is machboot's again");
	CHECK(!msc_ready(&k, &a, &q) && a == 0x3A, "ejected: no medium");

	/* the menu shares again, then stops sharing */
	ui_share(1);
	CHECK(!msc_ready(&k, &a, &q) && a == 0x28, "shared again: UNIT ATTENTION");
	CHECK(msc_ready(&k, &a, &q), "ready");
	ui_share(0);
	CHECK(!msc_ready(&k, &a, &q) && a == 0x3A, "the menu stops sharing: no medium");
	CHECK(msc_write(100, buf2, 1) == -1, "WRITE then: no medium, nothing written");

	/* machboot goes (Start: another image): the link drops, the pins are let go */
	fpga_machboot = 0;
	now_ms += 1000; msc_tick();
	CHECK(!msc_link() && !attached, "machboot gone: link down, pins released");
	now_ms += 4000; firmware();

	printf("%s xlink and the card reader: %d failures\n", fails ? "FAIL" : "ok  ", fails);
	return fails ? 1 : 0;
}
