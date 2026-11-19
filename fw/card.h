/*
 * machboot
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * What is on the SD card for this board (docs/sdcard.md).
 */

#ifndef MB_CARD_H
#define MB_CARD_H

#include <stdint.h>
#include "fat.h"

#define CARD_MAX 16

enum { CE_SYSTEM, CE_FILE };

typedef struct {
	int kind;		/* CE_SYSTEM: a folder with SYSTEM.CFG; CE_FILE: a loose .BIT */
	char name[24];		/* "Zeitlos", or "BLINK.BIT" */
	char ver[16];
	char dir[13];		/* the folder under \SYS\GATEWARE */
	fat_entry_t bit;	/* the bitstream for this board */
	int has_bit;		/* the folder has one for this board */
	fat_vol_t *vol;		/* where it is: the SD card's volume, or a flash drive's */
	int usb;		/* on a USB flash drive */
} card_entry_t;

typedef struct {
	int sd_ok;		/* a card answered */
	int vols;		/* FAT volumes found */
	int found;		/* \SYS\GATEWARE found */
	fat_vol_t *vol;		/* on this volume */
	int usb;		/* a USB flash drive with \SYS\GATEWARE */
	int n;
	card_entry_t e[CARD_MAX];
} card_t;

extern card_t card;

void card_scan(void);
const char *board_file(uint32_t board);		/* "LAKRITZ.BIT" */
card_entry_t *card_system(const char *name);	/* by name, case-insensitive */

#endif
