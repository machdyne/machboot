/*
 * machboot
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Overlays: ovl.h, docs/overlays.md.
 *
 * In flash, after the firmware's header and payload, or right after the
 * header when the payload had to go below it (tools/mkslot.py):
 *
 *   "MBOV", count, the firmware's CRC-32 (they belong together), 0,
 *   then per overlay: offset (from "MBOV"), length, CRC-32, 0;
 *   then the overlays, each linked to run at __ovl_start.
 */

#include <stdint.h>
#include "ovl.h"
#include "mb.h"
#include "lib.h"
#include "console.h"

#define FL(a)	(*(volatile uint32_t *)(0x10000000u + (a)))
#define OVL_MAGIC 0x564F424Du	/* "MBOV" */

extern uint32_t __ovl_start[], __ovl_size[];

static int cur_id = -1;
static uint32_t loads;
static const char *err = "";

const char *ovl_error(void) { return err; }
uint32_t ovl_loads(void) { return loads; }

/* where the table is (tools/mkslot.py): after the payload when the
   payload follows the header; right after the header when the payload
   had to go below it (word 7 set) */
static uint32_t table(void)
{
	uint32_t base = mb.fw_base;
	if (FL(base + 28))
		return base + 32;
	return base + 32 + ((FL(base + 8) + 3) & ~3u);
}

int ovl_use(int id)
{
	if (id == cur_id)
		return 1;
	if (!mb.fw_base) {
		err = "this build has no flash to load pages from";
		return 0;
	}
	uint32_t t = table();
	if (FL(t) != OVL_MAGIC || (int)FL(t + 4) != OVL_COUNT || FL(t + 8) != FL(mb.fw_base + 20)) {
		err = "the pages in flash are not this firmware's";
		return 0;
	}
	uint32_t off = FL(t + 16 + (uint32_t)id * 16), len = FL(t + 20 + (uint32_t)id * 16);
	uint32_t crc = FL(t + 24 + (uint32_t)id * 16);
	if (len > (uint32_t)__ovl_size) {
		err = "a page is bigger than its window";
		return 0;
	}
	cur_id = -1;			/* half copied, it is no page */
	for (uint32_t i = 0; i < (len + 3) / 4; i++)
		__ovl_start[i] = FL(t + off + i * 4);
	if (mb_crc32(0, __ovl_start, len) != crc) {
		err = "a page in flash is damaged";
		con_printf("overlay %d: CRC wrong\n", id);
		return 0;
	}
	cur_id = id;
	loads++;
	return 1;
}
