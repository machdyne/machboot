/*
 * machboot supervisor
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * machboot's settings (and its list of systems), kept in the RP2040's
 * flash. A log of 1 KB records in a 64 KB area: each save programs one
 * record; a sector is erased only when the log comes round to it, so 4
 * saves per erase, and the newest record is always intact (a power cut
 * mid-write leaves a record whose CRC does not match, which is skipped).
 *
 * Portable: the flash is store_io's.
 */

#ifndef STORE_H
#define STORE_H

#include <stdint.h>

#define STORE_SIZE	(64 * 1024)
#define STORE_PAGE	256			/* what program() writes */
#define STORE_REC	1024			/* a record: four pages */
#define STORE_SECTOR	4096
#define STORE_MAX	(STORE_REC - 12)	/* bytes of text per record */

typedef struct {
	const uint8_t *base;			/* the area, readable (XIP) */
	void (*erase)(uint32_t off);		/* a 4 KB sector at off */
	void (*program)(uint32_t off, const uint8_t *page);	/* 256 bytes at off */
} store_io_t;

void store_init(const store_io_t *io);
int store_get(char *buf, int cap);		/* the newest text, its length (0: none) */
int store_put(const char *text, int len);	/* 0 written, 1 unchanged (nothing written) */
uint32_t store_writes(void);			/* since store_init */

#endif
