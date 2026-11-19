/*
 * machboot supervisor
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The system cache (docs/systems.md): bitstreams of systems machboot
 * keeps on the board, in the RP2040's flash, each in a contiguous extent
 * of whole 4 KB sectors:
 *
 *   +0     a 256-byte header: "MBSY", version 1, length, CRC-32, IDCODE,
 *          name[24], ver[16]
 *   +256   the bitstream
 *
 * The header is written last, after the bitstream has been written and
 * read back against its CRC: an extent without a valid header (a copy
 * cancelled or cut by a power cut) is free space. A system is known by
 * its bitstream's CRC-32. Removing one erases its header's sector.
 *
 * Portable: the flash is cache_io_t's.
 */

#ifndef CACHE_H
#define CACHE_H

#include <stdint.h>

#define CACHE_HDR	256
#define CACHE_SECTOR	4096
#define CACHE_BLOCK	65536

typedef struct {
	const uint8_t *base;			/* the area, readable (XIP) */
	uint32_t size;
	void (*erase)(uint32_t off, uint32_t len);	/* whole sectors (len a multiple of 4 KB) */
	void (*program)(uint32_t off, const uint8_t *page);	/* 256 bytes */
} cache_io_t;

typedef struct {
	uint32_t off;			/* of the extent, within the area */
	uint32_t len, crc, idcode;
	char name[24], ver[16];
} cache_item_t;

void cache_init(const cache_io_t *io);
int cache_list(cache_item_t *out, int max);	/* in address order */
uint32_t cache_free(uint32_t *largest);		/* bytes free in all; the largest gap */
uint32_t cache_size(void);
const cache_item_t *cache_find(uint32_t crc);
const uint8_t *cache_bitstream(const cache_item_t *it);
int cache_check(const cache_item_t *it);	/* its bitstream still matches its CRC */

/* writing one: begin (finds room, 0 or CACHE_NOROOM), data in order, end */
enum { CACHE_OK = 0, CACHE_NOROOM, CACHE_ERROR, CACHE_NOTFOUND };
int cache_begin(uint32_t len, const char *name, const char *ver);
int cache_data(const uint8_t *p, uint32_t n);	/* in order; any length */
int cache_end(uint32_t *crc);			/* verifies, writes the header; the CRC */
void cache_abort(void);
int cache_drop(uint32_t crc);

#endif
