/*
 * machboot supervisor
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The dirtyJTAG protocol, version 2, as openFPGALoader and other hosts
 * speak it over the vendor interface's bulk endpoints (docs/rp2040.md).
 * Portable: the pins are jtag_io.h's, the replies go to djtag_reply().
 */

#ifndef SV_DJTAG_H
#define SV_DJTAG_H

#include <stdint.h>

enum {
	CMD_STOP = 0x00,
	CMD_INFO = 0x01,
	CMD_FREQ = 0x02,
	CMD_XFER = 0x03,
	CMD_SETSIG = 0x04,
	CMD_GETSIG = 0x05,
	CMD_CLK = 0x06,
	CMD_GOTOBOOTLOADER = 0x08,	/* machboot: the RP2040's USB boot ROM (UF2) */
};
enum { XFER_EXTEND = 0x40, XFER_NO_READ = 0x80 };
enum {
	SIG_TCK = 1 << 1,
	SIG_TDI = 1 << 2,
	SIG_TDO = 1 << 3,
	SIG_TMS = 1 << 4,
	SIG_TRST = 1 << 5,
	SIG_SRST = 1 << 6,
};

#define DJTAG_INFO "DJTAG2\n"

void djtag_init(void);
/* bytes from the host, any split across USB packets */
void djtag_feed(const uint8_t *p, uint32_t n);

/* the platform's: send reply bytes to the host (bulk IN), and push them out */
void djtag_reply(const uint8_t *p, uint32_t n);
void djtag_flush(void);

#endif
