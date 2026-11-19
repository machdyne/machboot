/*
 * machboot
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * SD card, SPI mode -- written from the SD Physical Layer Simplified
 * Specification (version 9.10), chapter 7 ("SPI Mode"):
 *
 *   power-up: >= 74 clocks with CS high, at <= 400 kHz
 *   CMD0  GO_IDLE_STATE (CRC 0x95)          -> R1 0x01
 *   CMD8  SEND_IF_COND 0x1AA (CRC 0x87)     -> R7; echo 0xAA: a v2 card
 *                                              R1 "illegal command": a v1 card
 *   ACMD41 (CMD55, CMD41; HCS for v2) until R1 is 0 (out of idle)
 *   CMD58 READ_OCR: CCS (bit 30) set means block addressing (SDHC/SDXC)
 *   CMD16 SET_BLOCKLEN 512 for byte-addressed cards
 *   CMD9  SEND_CSD: capacity
 *   CMD17 READ_SINGLE_BLOCK -> R1, then the start token 0xFE, 512 bytes, CRC16
 *
 * The SPI master is Zeitlos's rtl/spim.v: DATA stalls while a transfer
 * runs, so `write; read` is a whole exchange; CTRL bit 1 makes it four
 * bytes at a time, first byte in bits 7:0.
 */

#include "sd.h"
#include "hw.h"
#include "lib.h"
#include "console.h"

sd_info_t sd;

#define DIV_INIT	59	/* 48 MHz / (2 * 60) = 400 kHz */
#define DIV_FAST	0	/* 24 MHz: SD's SPI-mode maximum is 25. Zeitlos runs the same
				   controller on the same boards at this (sw/common/zeitlos.h,
				   Z_SPISD_DIV_FAST); a card that fails here: back to 1, 12 MHz */

static uint32_t ctrl_div = DIV_INIT;

static void ctrl(int cs, int x32)
{
	sd_wr(SD_CTRL, (ctrl_div << 8) | (x32 ? SD_CTRL_X32 : 0) | (cs ? SD_CTRL_CS : 0));
}

static uint8_t xchg(uint8_t b)
{
	sd_wr(SD_DATA, b);
	return (uint8_t)sd_rd(SD_DATA);
}

static void deselect(void)
{
	ctrl(0, 0);
	xchg(0xff);		/* the card releases MISO on the next clocks */
}

/* send a command; returns R1 (0xff: no response) */
static uint8_t cmd(uint8_t c, uint32_t arg, uint8_t crc)
{
	ctrl(1, 0);
	xchg(0xff);
	xchg(0x40 | c);
	xchg((uint8_t)(arg >> 24));
	xchg((uint8_t)(arg >> 16));
	xchg((uint8_t)(arg >> 8));
	xchg((uint8_t)arg);
	xchg(crc);
	uint8_t r = 0xff;
	for (int i = 0; i < 10 && (r & 0x80); i++)
		r = xchg(0xff);
	return r;
}

static uint8_t acmd(uint8_t c, uint32_t arg)
{
	cmd(55, 0, 0x65);
	deselect();
	return cmd(c, arg, 0x01);
}

static int wait_token(uint32_t ms)
{
	uint32_t t0 = ms_now();
	uint8_t t;
	while ((t = xchg(0xff)) == 0xff)
		if (ms_now() - t0 > ms)
			return -1;
	return t == 0xfe ? 0 : -1;
}

static uint32_t csd_sectors(const uint8_t *c)
{
	if ((c[0] >> 6) == 1) {
		/* CSD 2.0: C_SIZE [69:48], capacity (C_SIZE + 1) * 512 KB */
		uint32_t cs = ((uint32_t)(c[7] & 0x3f) << 16) | ((uint32_t)c[8] << 8) | c[9];
		return (cs + 1) * 1024;
	}
	/* CSD 1.0: (C_SIZE + 1) * 2^(C_SIZE_MULT + 2) * 2^READ_BL_LEN bytes */
	uint32_t rbl = c[5] & 0x0f;
	uint32_t cs = ((uint32_t)(c[6] & 0x03) << 10) | ((uint32_t)c[7] << 2) | (c[8] >> 6);
	uint32_t mult = ((c[9] & 0x03) << 1) | (c[10] >> 7);
	return ((cs + 1) << (mult + 2)) << rbl >> 9;
}

int sd_init(void)
{
	uint8_t r, b[16];

	memset(&sd, 0, sizeof(sd));
	if (sd_rd(SD_MAGIC) != SD_MAGIC_VAL)
		return -1;

	ctrl_div = DIV_INIT;
	ctrl(0, 0);
	for (int i = 0; i < 10; i++)
		xchg(0xff);

	r = 0xff;
	for (int i = 0; i < 10 && r != 0x01; i++) {
		r = cmd(0, 0, 0x95);
		deselect();
	}
	if (r != 0x01)
		return -1;

	r = cmd(8, 0x1aa, 0x87);
	if (r == 0x01) {
		for (int i = 0; i < 4; i++)
			b[i] = xchg(0xff);
		deselect();
		if (b[3] != 0xaa)
			return -1;
		sd.version = 2;
	} else {
		deselect();
		sd.version = 1;
	}

	uint32_t t0 = ms_now();
	do {
		r = acmd(41, sd.version == 2 ? 0x40000000u : 0);
		deselect();
		if (ms_now() - t0 > 1000)
			return -1;
	} while (r != 0x00);

	if (sd.version == 2) {
		r = cmd(58, 0, 0x01);
		for (int i = 0; i < 4; i++)
			b[i] = xchg(0xff);
		deselect();
		if (r != 0)
			return -1;
		sd.sdhc = (b[0] & 0x40) != 0;
	}
	if (!sd.sdhc) {
		r = cmd(16, 512, 0x01);
		deselect();
		if (r != 0)
			return -1;
	}

	ctrl_div = DIV_FAST;

	r = cmd(9, 0, 0x01);
	if (r == 0 && wait_token(100) == 0) {
		for (int i = 0; i < 16; i++)
			b[i] = xchg(0xff);
		xchg(0xff);
		xchg(0xff);
		sd.sectors = csd_sectors(b);
	}
	deselect();

	sd.present = 1;
	return 0;
}

int sd_read(uint32_t lba, uint8_t *buf)
{
	if (!sd.present)
		return -1;
	uint8_t r = cmd(17, sd.sdhc ? lba : lba * 512, 0x01);
	if (r != 0 || wait_token(200) != 0) {
		deselect();
		return -1;
	}
	ctrl(1, 1);
	if (((uintptr_t)buf & 3) == 0) {
		/* an aligned buffer: the word as it comes (little-endian, as the bytes) */
		uint32_t *w = (uint32_t *)(void *)buf;
		for (int i = 0; i < 128; i++) {
			sd_wr(SD_DATA, 0xffffffffu);
			w[i] = sd_rd(SD_DATA);
		}
	} else {
		for (int i = 0; i < 512; i += 4) {
			sd_wr(SD_DATA, 0xffffffffu);
			uint32_t w = sd_rd(SD_DATA);
			buf[i] = (uint8_t)w;
			buf[i + 1] = (uint8_t)(w >> 8);
			buf[i + 2] = (uint8_t)(w >> 16);
			buf[i + 3] = (uint8_t)(w >> 24);
		}
	}
	ctrl(1, 0);
	xchg(0xff);		/* CRC16, not checked */
	xchg(0xff);
	deselect();
	return 0;
}

/*
 * One block (SD Physical Layer Simplified Specification 7.2.4, 7.3.3):
 * CMD24, a gap byte, the start token 0xFE, 512 bytes and two CRC bytes
 * (not checked in SPI mode), then the card's data response xxx0sss1
 * (sss 010: accepted), then busy (MISO low) while it programs.
 */
int sd_write(uint32_t lba, const uint8_t *buf)
{
	if (!sd.present)
		return -1;
	uint8_t r = cmd(24, sd.sdhc ? lba : lba * 512, 0x01);
	if (r != 0) {
		deselect();
		return -1;
	}
	xchg(0xff);
	xchg(0xfe);
	ctrl(1, 1);
	for (int i = 0; i < 512; i += 4) {
		sd_wr(SD_DATA, (uint32_t)buf[i] | (uint32_t)buf[i + 1] << 8 |
		               (uint32_t)buf[i + 2] << 16 | (uint32_t)buf[i + 3] << 24);
		(void)sd_rd(SD_DATA);
	}
	ctrl(1, 0);
	xchg(0xff);		/* CRC16 */
	xchg(0xff);
	uint8_t resp = xchg(0xff);
	if ((resp & 0x1f) != 0x05) {
		deselect();
		return -1;
	}
	uint32_t t0 = ms_now();
	while (xchg(0xff) == 0x00)
		if (ms_now() - t0 > 500) {
			deselect();
			return -1;
		}
	deselect();
	return 0;
}
