/*
 * machboot supervisor
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The system cache: cache.h.
 */

#include <string.h>
#include "cache.h"

#define MAX_ITEMS 32

static const cache_io_t *io;
static cache_item_t items[MAX_ITEMS];
static int n_items;

/* a write in progress */
static int w_on;
static uint32_t w_off, w_len, w_pos, w_crc, w_erased_to;
static char w_name[24], w_ver[16];
static uint8_t w_page[256];
static uint32_t w_page_n;

static uint32_t crc_upd(uint32_t c, const uint8_t *p, uint32_t n)
{
	static uint32_t tab[256];
	if (!tab[1])
		for (uint32_t i = 0; i < 256; i++) {
			uint32_t x = i;
			for (int k = 0; k < 8; k++)
				x = (x & 1) ? 0xEDB88320u ^ (x >> 1) : x >> 1;
			tab[i] = x;
		}
	c = ~c;
	while (n--)
		c = tab[(c ^ *p++) & 0xff] ^ (c >> 8);
	return ~c;
}

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

static uint32_t extent(uint32_t len)
{
	return (CACHE_HDR + len + CACHE_SECTOR - 1) & ~(uint32_t)(CACHE_SECTOR - 1);
}

/* the IDCODE in a bitstream's VERIFY_ID (0xE2 00 00 00, then it, big-endian) */
static uint32_t idcode_of(const uint8_t *b, uint32_t len)
{
	for (uint32_t i = 0; i + 8 <= len && i < 4096; i++)
		if (b[i] == 0xE2 && !b[i + 1] && !b[i + 2] && !b[i + 3])
			return (uint32_t)b[i + 4] << 24 | b[i + 5] << 16 | b[i + 6] << 8 | b[i + 7];
	return 0;
}

static void scan(void)
{
	n_items = 0;
	for (uint32_t off = 0; off + CACHE_SECTOR <= io->size && n_items < MAX_ITEMS; ) {
		const uint8_t *h = io->base + off;
		uint32_t len = rd32(h + 8);
		if (!memcmp(h, "MBSY", 4) && rd32(h + 4) == 1 && len && off + extent(len) <= io->size) {
			cache_item_t *it = &items[n_items++];
			it->off = off;
			it->len = len;
			it->crc = rd32(h + 12);
			it->idcode = rd32(h + 16);
			memcpy(it->name, h + 20, 24); it->name[23] = 0;
			memcpy(it->ver, h + 44, 16); it->ver[15] = 0;
			off += extent(len);
		} else {
			off += CACHE_SECTOR;
		}
	}
}

void cache_init(const cache_io_t *dev)
{
	io = dev;
	w_on = 0;
	scan();
}

int cache_list(cache_item_t *out, int max)
{
	int n = n_items < max ? n_items : max;
	memcpy(out, items, sizeof(cache_item_t) * (size_t)n);
	return n;
}

/* the gaps between extents, in address order: the first that holds need */
static int first_fit(uint32_t need, uint32_t *at, uint32_t *largest, uint32_t *total)
{
	uint32_t pos = 0, big = 0, sum = 0;
	int found = 0;
	for (int i = 0; i <= n_items; i++) {
		uint32_t end = i < n_items ? items[i].off : io->size;
		uint32_t gap = end > pos ? end - pos : 0;
		sum += gap;
		if (gap > big) big = gap;
		if (!found && gap >= need) { *at = pos; found = 1; }
		if (i < n_items) pos = items[i].off + extent(items[i].len);
	}
	if (largest) *largest = big;
	if (total) *total = sum;
	return found;
}

uint32_t cache_free(uint32_t *largest)
{
	uint32_t at = 0, total;
	first_fit(0xFFFFFFFFu, &at, largest, &total);
	return total;
}

uint32_t cache_size(void)
{
	return io->size;
}

const cache_item_t *cache_find(uint32_t crc)
{
	for (int i = 0; i < n_items; i++)
		if (items[i].crc == crc)
			return &items[i];
	return 0;
}

const uint8_t *cache_bitstream(const cache_item_t *it)
{
	return io->base + it->off + CACHE_HDR;
}

int cache_check(const cache_item_t *it)
{
	return crc_upd(0, cache_bitstream(it), it->len) == it->crc;
}

int cache_begin(uint32_t len, const char *name, const char *ver)
{
	uint32_t at = 0;
	if (!len || !first_fit(extent(len), &at, 0, 0))
		return CACHE_NOROOM;
	w_on = 1;
	w_off = at;
	w_len = len;
	w_pos = 0;
	w_crc = 0;
	w_page_n = 0;
	w_erased_to = at;
	memset(w_name, 0, sizeof(w_name));
	memset(w_ver, 0, sizeof(w_ver));
	strncpy(w_name, name, sizeof(w_name) - 1);
	strncpy(w_ver, ver, sizeof(w_ver) - 1);
	/* the header's sector first: whatever was there (an old header) goes,
	   and stays erased until the end */
	io->erase(at, CACHE_SECTOR);
	w_erased_to = at + CACHE_SECTOR;
	return CACHE_OK;
}

/* erase ahead of where the data goes: 64 KB blocks where aligned, else sectors */
static void erase_upto(uint32_t to)
{
	uint32_t end = w_off + extent(w_len);
	if (to > end) to = end;
	while (w_erased_to < to) {
		uint32_t n = CACHE_SECTOR;
		if (!(w_erased_to % CACHE_BLOCK) && w_erased_to + CACHE_BLOCK <= end)
			n = CACHE_BLOCK;
		io->erase(w_erased_to, n);
		w_erased_to += n;
	}
}

static void flush_page(void)
{
	uint32_t at = w_off + CACHE_HDR + w_pos - w_page_n;
	if (w_page_n < 256)
		memset(w_page + w_page_n, 0xFF, 256 - w_page_n);
	erase_upto(at + 256);
	io->program(at, w_page);
	w_page_n = 0;
}

int cache_data(const uint8_t *p, uint32_t n)
{
	if (!w_on || w_pos + n > w_len)
		return CACHE_ERROR;
	w_crc = crc_upd(w_crc, p, n);
	while (n) {
		uint32_t k = 256 - w_page_n;
		if (k > n) k = n;
		memcpy(w_page + w_page_n, p, k);
		w_page_n += k;
		w_pos += k;
		p += k;
		n -= k;
		if (w_page_n == 256)
			flush_page();
	}
	return CACHE_OK;
}

int cache_end(uint32_t *crc)
{
	if (!w_on || w_pos != w_len)
		return CACHE_ERROR;
	if (w_page_n)
		flush_page();
	w_on = 0;
	/* read it back: the flash must hold what came */
	if (crc_upd(0, io->base + w_off + CACHE_HDR, w_len) != w_crc)
		return CACHE_ERROR;
	uint8_t h[256];
	memset(h, 0xFF, sizeof(h));
	memcpy(h, "MBSY", 4);
	wr32(h + 4, 1);
	wr32(h + 8, w_len);
	wr32(h + 12, w_crc);
	wr32(h + 16, idcode_of(io->base + w_off + CACHE_HDR, w_len));
	memcpy(h + 20, w_name, 24);
	memcpy(h + 44, w_ver, 16);
	/* an older copy of the same bitstream goes: one copy each */
	const cache_item_t *old = cache_find(w_crc);
	if (old)
		io->erase(old->off, CACHE_SECTOR);
	io->program(w_off, h);
	scan();
	if (!cache_find(w_crc))
		return CACHE_ERROR;
	*crc = w_crc;
	return CACHE_OK;
}

void cache_abort(void)
{
	w_on = 0;			/* no header: the extent is free space */
}

int cache_drop(uint32_t crc)
{
	const cache_item_t *it = cache_find(crc);
	if (!it)
		return CACHE_NOTFOUND;
	io->erase(it->off, CACHE_SECTOR);
	scan();
	return CACHE_OK;
}
