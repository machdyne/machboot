/*
 * machboot supervisor
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The XA-XD link to the FPGA, the master's side (docs/xlink.md). The
 * slave is machboot's gateware (rtl/mb_xlink.v); when something else is
 * loaded, nothing answers 0x5A and the link stays released: the pins
 * are inputs except during a probe.
 *
 * Portable: the pins are xlink_io.h's.
 */

#ifndef XLINK_H
#define XLINK_H

#include <stdint.h>

#define XL_SYNC		0x5A
#define XL_C_STATUS	0x01
#define XL_C_SEND	0x02
#define XL_C_RECV	0x03
#define XL_MAX		2048

typedef struct {
	uint8_t flags;		/* machboot's: bit 0 the card is shared, 7:4 changes */
	uint8_t state;		/* bit 0 its receive buffer is full, bit 1 a reply is ready */
	uint16_t txlen;		/* the reply's length (2048 as 0 on the wire) */
	uint8_t rxseq;
} xl_status_t;

/* one STATUS frame: 0 and *s if machboot answers, -1 if not */
int xl_status(xl_status_t *s);

/*
 * One request and its reply: waits for room, sends, waits up to
 * timeout_ms for the reply, reads it. The reply's first two bytes must
 * be the request's op | 0x80 and its tag. Returns the reply's length,
 * or -1 no link, -2 timeout, -3 a reply that does not match.
 */
int xl_call(const uint8_t *req, int reqlen, uint8_t *rep, int maxrep, uint32_t timeout_ms);

#endif
