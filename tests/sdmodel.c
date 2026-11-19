/*
 * machboot
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 */

#include "sdmodel.h"
#include <string.h>

static uint8_t *img;
static size_t img_size;
static int is_sdhc;

static uint8_t cmdbuf[6];
static int cmdlen;
static uint8_t out[600];		/* response bytes queued */
static int out_len, out_pos;
static int idle = 1, acmd, acmd41_tries;
static uint32_t nreads, nwrites;
/* a CMD24 in progress: waiting for the token (1), taking data (2) */
static int wstate, wcount;
static uint64_t waddr;
static uint8_t wbuf[514];

void sdm_init(uint8_t *image, size_t size, int sdhc)
{
	img = image;
	img_size = size;
	is_sdhc = sdhc;
	cmdlen = 0;
	out_len = out_pos = 0;
	idle = 1;
	acmd = 0;
	acmd41_tries = 0;
	nreads = 0;
	nwrites = 0;
	wstate = 0;
}

uint32_t sdm_reads(void) { return nreads; }
uint32_t sdm_writes(void) { return nwrites; }

uint8_t sdm_peek(void) { return out_pos < out_len ? out[out_pos] : 0xff; }

static void q(uint8_t b)
{
	if (out_len < (int)sizeof(out))
		out[out_len++] = b;
}

static uint8_t r1(void) { return idle ? 0x01 : 0x00; }

static void command(void)
{
	uint8_t c = cmdbuf[0] & 0x3f;
	uint32_t arg = (uint32_t)cmdbuf[1] << 24 | (uint32_t)cmdbuf[2] << 16 | (uint32_t)cmdbuf[3] << 8 | cmdbuf[4];
	int was_acmd = acmd;
	acmd = 0;

	out_len = out_pos = 0;
	q(0xff);		/* NCR: one byte before the response */

	if (was_acmd && c == 41) {
		if (++acmd41_tries >= 2)
			idle = 0;
		q(r1());
		return;
	}
	switch (c) {
	case 0:
		idle = 1;
		acmd41_tries = 0;
		q(0x01);
		break;
	case 8:
		q(r1());
		q(0x00); q(0x00); q((uint8_t)((arg >> 8) & 0x0f)); q((uint8_t)arg);
		break;
	case 55:
		acmd = 1;
		q(r1());
		break;
	case 58:
		q(r1());
		q(is_sdhc ? 0xC0 : 0x80); q(0xFF); q(0x80); q(0x00);
		break;
	case 16:
		q(r1());
		break;
	case 9: {
		/* CSD 2.0: C_SIZE = sectors / 1024 - 1 */
		uint32_t cs = (uint32_t)(img_size / 512 / 1024);
		if (cs) cs--;
		uint8_t csd[16] = { 0x40, 0x0e, 0x00, 0x32, 0x5b, 0x59, 0x00,
			(uint8_t)((cs >> 16) & 0x3f), (uint8_t)(cs >> 8), (uint8_t)cs,
			0x7f, 0x80, 0x0a, 0x40, 0x00, 0x01 };
		q(0x00);
		q(0xff);
		q(0xfe);
		for (int i = 0; i < 16; i++) q(csd[i]);
		q(0x00); q(0x00);
		break;
	}
	case 17: {
		uint64_t a = is_sdhc ? (uint64_t)arg * 512 : arg;
		if (idle || a + 512 > img_size) {
			q(0x40);	/* parameter error */
			break;
		}
		nreads++;
		q(0x00);
		q(0xff);
		q(0xff);
		q(0xfe);
		for (int i = 0; i < 512; i++) q(img[a + i]);
		q(0x00); q(0x00);
		break;
	}
	case 24: {
		uint64_t a = is_sdhc ? (uint64_t)arg * 512 : arg;
		if (idle || a + 512 > img_size) {
			q(0x40);
			break;
		}
		q(0x00);
		waddr = a;
		wstate = 1;
		wcount = 0;
		break;
	}
	default:
		q(0x05);		/* illegal command (and idle) */
		break;
	}
}

/* a byte of a CMD24's data phase */
static void write_byte(uint8_t mosi)
{
	if (wstate == 1) {
		if (mosi == 0xfe)
			wstate = 2;
		return;
	}
	wbuf[wcount++] = mosi;
	if (wcount == 514) {
		memcpy(img + waddr, wbuf, 512);
		nwrites++;
		wstate = 0;
		out_len = out_pos = 0;
		q(0xe5);		/* data response: accepted (xxx0 010 1) */
		q(0x00); q(0x00); q(0x00);	/* busy */
	}
}

uint8_t sdm_xchg(uint8_t mosi)
{
	uint8_t miso = 0xff;
	if (out_pos < out_len)
		miso = out[out_pos++];

	if (wstate) {
		write_byte(mosi);
		return miso;
	}
	if (cmdlen == 0) {
		/* a command starts with 01xxxxxx */
		if ((mosi & 0xc0) == 0x40)
			cmdbuf[cmdlen++] = mosi;
	} else {
		cmdbuf[cmdlen++] = mosi;
		if (cmdlen == 6) {
			cmdlen = 0;
			command();
		}
	}
	return miso;
}

void sdm_deselect(void)
{
	cmdlen = 0;
	out_len = out_pos = 0;
	wstate = 0;
}
