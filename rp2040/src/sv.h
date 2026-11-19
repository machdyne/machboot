/*
 * machboot supervisor
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The menu at power-on (Sergei, Mozart; docs/power-on.md). The RP2040
 * keeps machboot's menu -- a bitstream for the module's FPGA -- in its
 * own flash, and loads it into the FPGA's SRAM over JTAG:
 *
 *   - at power-on (and when the RP2040 itself is reset), not after a
 *     UF2 update;
 *   - after every reset of the module (SYS_RST_N low: the reset button,
 *     or a system's own reboot), except the one machboot itself causes
 *     when it starts the system -- machboot says so first (msc.c, START);
 *
 * and never while a PC uses JTAG: openFPGALoader always wins, a load in
 * progress stops for it.
 *
 * Portable: the board is sv_io_t's.
 */

#ifndef SV_H
#define SV_H

#include <stdint.h>

#define MENU_MAGIC	"MBMN"
#define MENU_HDR	256		/* the bitstream follows */

typedef struct {
	char magic[4];		/* "MBMN" */
	uint32_t version;	/* 1 */
	uint32_t idcode;	/* the FPGA it is for */
	uint32_t length;	/* of the bitstream */
	uint32_t crc;		/* CRC-32 of the bitstream */
	uint32_t board;		/* machboot's BOARD: carrier | module << 8 */
	char name[40];		/* "machboot 0.3 Sergei ML2" */
} menu_hdr_t;

typedef struct {
	const uint8_t *menu;		/* the header, then the bitstream (XIP) */
	uint32_t menu_max;		/* bytes available there */
	uint32_t (*ms)(void);
	int (*pc_active)(void);		/* a PC used JTAG lately */
	int (*pc_wants)(void);		/* a PC's JTAG command is waiting now */
	int (*reset_edge)(void);	/* SYS_RST_N went low since the last call (clears it) */
	int (*reset_low)(void);		/* SYS_RST_N is low now */
	void (*poll)(void);		/* keep USB going during a load */
	void (*log)(const char *s);
} sv_io_t;

void sv_init(const sv_io_t *io, int power_on);
void sv_tick(void);
void sv_start(void);			/* machboot starts the system: the next reset is its */
void sv_request(void);			/* load the menu now (a serial break on the CDC port) */
/* load a cached system (machboot asked: docs/systems.md); if it does not
   start, the menu comes back and machboot is told why (sv_note) */
void sv_run(const uint8_t *bit, uint32_t len, uint32_t idcode, const char *name);
int sv_note(char *buf, int cap);
void sv_refuse(const char *name, const char *why);	/* not started: the menu, and machboot told why */	/* the note for machboot, once (0: none) */

int sv_menu_ok(void);			/* a menu is stored, whole */
const menu_hdr_t *sv_menu(void);
int sv_loads(void);			/* menus loaded since sv_init */
int sv_last(void);			/* the last load's result (ecp5.h) */

#endif
