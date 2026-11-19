/*
 * machboot
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The RP2040's system cache (Sergei, Mozart; docs/systems.md): machboot
 * asks the supervisor, over the XA-XD link, to list what it keeps, to
 * copy a bitstream from the card into it, to drop one, and to load one
 * into the FPGA. fw/xlink.c; fw/usbd.c (the other boards) has none.
 *
 * Every call blocks until the RP2040 has done it (or gone).
 */

#ifndef MB_RCACHE_H
#define MB_RCACHE_H

#include <stdint.h>
#include "card.h"
#include "xlink_proto.h"

#define RC_MAX 16			/* listed; the RP2040 may hold more */

typedef struct {
	uint32_t crc, len, off;
	char name[24], ver[16];
} rc_item_t;

typedef struct {
	int n;
	uint32_t free, largest, size;	/* bytes */
	rc_item_t it[RC_MAX];
} rc_list_t;

/* results: XL_R_* (0 ok), and machboot's own */
enum { RC_READ = 16, RC_NOT_BIT, RC_WRONG_FPGA, RC_TOO_BIG };

int rc_available(void);			/* the RP2040's supervisor answers */
int rc_list(rc_list_t *l);
/* copy ce's bitstream into the cache; progress (often) may return 1 to cancel */
int rc_put(card_entry_t *ce, uint32_t *crc, uint32_t *len, int (*progress)(uint32_t done, uint32_t len));
void rc_cancel(void);			/* the PUT in progress stops at its next CHUNK */
int rc_drop(uint32_t crc);
int rc_run(uint32_t crc);		/* ok: the RP2040 now loads it; machboot goes */
const char *rc_note(void);		/* what the RP2040 said (a system did not start), or 0 */
void rc_note_clear(void);
const char *rc_error(int r);

#define RC_EXTENT(len)	((256u + (len) + 4095u) & ~4095u)	/* what one takes in the cache */
#define RC_RATE		100000u		/* bytes a second, card to cache (an estimate, not yet measured) */

#endif
