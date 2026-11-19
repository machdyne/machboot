/*
 * machboot
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The list of systems (Sergei, Mozart; docs/systems.md): the systems the
 * user has installed, in the user's order. The top one is the default.
 *
 *   - the module's own system: what is in the module's flash (it starts
 *     with PROGRAMN, as before; machboot does not change it);
 *   - the others: kept in the RP2040's flash (the cache) while there is
 *     room, loaded into the FPGA over JTAG from there; when one is not
 *     there, it is copied from the SD card first (the lowest-placed ones
 *     make room), then started.
 *
 * The list is kept with the settings, as "sys=" lines (fw/xlink.c).
 *
 * Lakritz and Konfekt (MB_SLOT): one system slot (0x060000-0x0FFFFF) and
 * the Zeitlos region, always reserved. The entry "in the slot" is the
 * one installed there (module = 1 here); starting any other copies it
 * from the card into the slot first (fw/install.c: only the sectors that
 * differ). Install only adds to the list; nothing is written until a
 * system is started, except an update of the one in the slot, or the
 * first one into an empty slot. The list is the "sys.list" setting:
 * names, in order (fw/settings.c).
 */

#ifndef MB_SYSTEMS_H
#define MB_SYSTEMS_H

#include <stdint.h>
#include "card.h"
#include "rcache.h"

#define SYS_MAX 8

typedef struct {
	int module;		/* the module's own system */
	char name[24], ver[16];
	uint32_t crc, len;	/* the bitstream in the cache (not the module's) */
	/* systems_refresh(): */
	int cached;		/* in the RP2040's flash now */
	card_entry_t *ce;	/* on the card (or a USB drive), for this board */
} sysent_t;

extern sysent_t sys_list[SYS_MAX];
extern int sys_n;
extern rc_list_t sys_rc;	/* the cache, as last listed */

int systems_on(void);			/* this board has the list (the RP2040 answers) */
void systems_refresh(void);		/* the cache's list, the card's, the module's */
void systems_clear(void);
void systems_line(const char *line, uint32_t len);	/* a "sys=" line of the settings */
int systems_text(char *b, int cap);

sysent_t *systems_find(const char *name);
int systems_move(int i, int to);	/* the new place */
int systems_ready(const sysent_t *e);	/* can start: module, cached, or on the card */
int systems_needs_copy(const sysent_t *e);

/* the systems copying len bytes would push out of the cache (lowest-placed
   first; things in the cache that are not in the list before those):
   their names in out (", "-separated); -1 if it cannot fit at all */
int systems_plan(uint32_t len, uint32_t keep_crc, char *out, int cap);
int systems_plan_lost(void);		/* of those, one is not on the card */
extern const char *systems_phase;	/* MB_SLOT: "Checking" (Esc stops it) or "Writing" */
uint32_t systems_copy_s(uint32_t len);	/* about how long a copy takes, s */

/* copy e (or a card entry, added at the bottom) into the cache;
   progress may cancel. 0 ok, else RC/XL_R_* */
int systems_copy(sysent_t *e, int (*progress)(uint32_t done, uint32_t len));
int systems_add(card_entry_t *ce, int (*progress)(uint32_t done, uint32_t len));
int systems_remove(int i);		/* from the list (and the cache) */
int systems_slot_writes(card_entry_t *ce);	/* MB_SLOT: Install's Enter writes the slot */
int systems_names(char *b, int cap);		/* MB_SLOT: "Zeitlos|Mach86|Kakao" */
void systems_from_names(const char *s);

const char *rc_error(int r);

#endif
