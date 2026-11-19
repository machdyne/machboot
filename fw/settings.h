/*
 * machboot
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Settings, kept in flash in Zeitlos's key/value log format (fw/kvlog.c,
 * from Zeitlos): two 4 KB sectors at the end of the boot slot,
 * 0x05E000-0x05FFFF. docs/settings.md.
 */

#ifndef MB_SETTINGS_H
#define MB_SETTINGS_H

#include <stdint.h>

#define SETTINGS_OFFSET	0x05E000u	/* within the boot image */

enum { KBD_US, KBD_DE };

typedef struct {
	int autostart;		/* 1: start the system after the wait */
	int wait_s;		/* 1, 2, 3, 5 or 10 */
	int kbd;		/* KBD_US, KBD_DE */
	/* what is installed in the system slot, as the last install recorded it */
	char sys_name[24];
	char sys_ver[16];
	uint32_t sys_crc;	/* CRC-32 of the installed bitstream */
	uint32_t sys_size;
} settings_t;

extern settings_t cfg;

void settings_load(int writable);	/* writable: 0 in install mode and JTAG test builds */
int settings_writable(void);
void settings_attach(uint32_t addr);	/* install mode: the new boot slot's store, writable */
int settings_save(void);			/* 0 ok; -1 not writable or flash refused */
int settings_set_system(const char *name, const char *ver, uint32_t crc, uint32_t size);

extern int settings_remote;		/* Sergei, Mozart: kept by the RP2040 (fw/xlink.c) */
extern int settings_dirty;

#endif
