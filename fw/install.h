/*
 * machboot
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Writing a system from the card into the system slot (docs/install.md).
 */

#ifndef MB_INSTALL_H
#define MB_INSTALL_H

#include "card.h"

enum {
	INST_OK = 0,
	INST_SAME,		/* already exactly this: nothing written */
	INST_READ,		/* the card could not be read */
	INST_NOT_BIT,		/* not an ECP5 bitstream */
	INST_WRONG_FPGA,	/* built for another FPGA */
	INST_TOO_BIG,		/* larger than the slot */
	INST_FLASH,		/* the flash refused or read back wrong */
	INST_CANCELLED,		/* install_cancel was set while checking: nothing written */
};

/* progress: 0..1000 */
int install(card_entry_t *ce, void (*progress)(int permille, const char *what));
/* set during install()'s check (the first 30 %, "Checking"): it stops
   before anything is written. Once "Writing", it cannot stop. */
extern volatile int install_cancel;
const char *install_error(int r);

/* 4 KB at a time, also for fw/usbd.c's DFU: n bytes, the rest of the sector erased */
extern uint8_t sector_buf[4096];
int same_as_flash(uint32_t addr, const uint8_t *p, uint32_t n);
int write_sector(uint32_t addr, const uint8_t *p, uint32_t n);	/* erase, program, read back */

#endif
