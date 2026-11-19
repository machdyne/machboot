/*
 * machboot
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Overlays (docs/overlays.md): the menu's pages are kept in flash, after
 * the firmware, and one at a time is copied into a window of RAM when it
 * is shown. Lakritz and Konfekt only (MB_OVERLAYS), where machboot runs
 * from flash; the SRAM builds, the JTAG test build and the PC build keep
 * every page in RAM, and ovl_use() is always 1 there.
 *
 * An overlay's code may call anything resident, never another overlay.
 * Resident code calls into an overlay only after ovl_use() said yes.
 */

#ifndef MB_OVL_H
#define MB_OVL_H

#include <stdint.h>

enum { OVL_START, OVL_UPDATE, OVL_CARD, OVL_SETTINGS, OVL_DIAG, OVL_BOARD, OVL_INSTMODE, OVL_COUNT };

#ifdef MB_OVERLAYS
int ovl_use(int id);		/* 1: its code is in the window now */
const char *ovl_error(void);	/* why not */
uint32_t ovl_loads(void);	/* copies made, for Board's details */
#else
static inline int ovl_use(int id) { (void)id; return 1; }
static inline const char *ovl_error(void) { return ""; }
static inline uint32_t ovl_loads(void) { return 0; }
#endif

#endif
