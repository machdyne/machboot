/*
 * machboot supervisor
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The settings log: store.h. A record:
 *
 *   0  "MBST"
 *   4  sequence number (u32, little-endian; the newest is the largest)
 *   8  length (u16), CRC-16/CCITT of the text (u16)
 *  12  the text
 *
 * Pages are written in order round the area; the page after the newest
 * is the next. When that page starts a sector, the sector is erased
 * first -- it holds only older records.
 */

#include <string.h>
#include "store.h"

static const store_io_t *io;
static int newest = -1;			/* page index */
static uint32_t newest_seq, writes;

#define PAGES (STORE_SIZE / STORE_REC)	/* records, here called pages */

static uint16_t crc16(const uint8_t *p, int n)
{
	uint16_t c = 0xFFFF;
	while (n--) {
		c ^= (uint16_t)(*p++ << 8);
		for (int i = 0; i < 8; i++)
			c = (uint16_t)((c & 0x8000) ? (c << 1) ^ 0x1021 : c << 1);
	}
	return c;
}

static int valid(int page, uint32_t *seq)
{
	const uint8_t *p = io->base + page * STORE_REC;
	if (memcmp(p, "MBST", 4))
		return 0;
	uint16_t len = (uint16_t)(p[8] | p[9] << 8);
	uint16_t crc = (uint16_t)(p[10] | p[11] << 8);
	if (len > STORE_MAX || crc16(p + 12, len) != crc)
		return 0;
	*seq = p[4] | p[5] << 8 | p[6] << 16 | (uint32_t)p[7] << 24;
	return 1;
}

void store_init(const store_io_t *dev)
{
	io = dev;
	newest = -1;
	newest_seq = 0;
	writes = 0;
	for (int i = 0; i < PAGES; i++) {
		uint32_t s;
		if (valid(i, &s) && (newest < 0 || s > newest_seq)) {
			newest = i;
			newest_seq = s;
		}
	}
}

int store_get(char *buf, int cap)
{
	if (newest < 0)
		return 0;
	const uint8_t *p = io->base + newest * STORE_REC;
	int len = p[8] | p[9] << 8;
	if (len > cap)
		len = cap;
	memcpy(buf, p + 12, (size_t)len);
	return len;
}

int store_put(const char *text, int len)
{
	static char cur[STORE_MAX];		/* static: the RP2040's stack is 2 KB */
	if (len > STORE_MAX)
		len = STORE_MAX;
	int n = store_get(cur, sizeof(cur));
	if (n == len && !memcmp(cur, text, (size_t)len))
		return 1;			/* the same: no wear */

	int page = (newest + 1) % PAGES;
	uint32_t off = (uint32_t)page * STORE_REC;
	/* a fresh sector: it holds the oldest records; or a page that is not
	   blank (left from an earlier round): erase first */
	int blank = 1;
	for (int i = 0; i < STORE_REC; i++)
		if (io->base[off + i] != 0xFF) { blank = 0; break; }
	if (!blank && off % STORE_SECTOR) {
		/* leftovers in the middle of a sector (not ours): go on at the next
		   sector, never erasing the one that holds the newest record */
		page = (int)((off / STORE_SECTOR + 1) * (STORE_SECTOR / STORE_REC)) % PAGES;
		off = (uint32_t)page * STORE_REC;
	}
	if (off % STORE_SECTOR == 0)
		io->erase(off);

	static uint8_t pg[STORE_REC];
	memset(pg, 0xFF, sizeof(pg));
	memcpy(pg, "MBST", 4);
	uint32_t seq = newest_seq + 1;
	pg[4] = (uint8_t)seq; pg[5] = (uint8_t)(seq >> 8); pg[6] = (uint8_t)(seq >> 16); pg[7] = (uint8_t)(seq >> 24);
	pg[8] = (uint8_t)len; pg[9] = (uint8_t)(len >> 8);
	uint16_t c = crc16((const uint8_t *)text, len);
	pg[10] = (uint8_t)c; pg[11] = (uint8_t)(c >> 8);
	memcpy(pg + 12, text, (size_t)len);
	for (int i = 0; i < STORE_REC; i += STORE_PAGE)
		io->program(off + (uint32_t)i, pg + i);
	writes++;

	uint32_t s;
	if (valid(page, &s) && s == seq) {
		newest = page;
		newest_seq = seq;
		return 0;
	}
	return -1;
}

uint32_t store_writes(void)
{
	return writes;
}
