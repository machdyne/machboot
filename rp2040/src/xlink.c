/*
 * machboot supervisor
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The XA-XD link, the master's side: docs/xlink.md, rtl/mb_xlink.v.
 */

#include <string.h>
#include "xlink.h"
#include "xlink_io.h"

static int attached;

static int frame_status(xl_status_t *s)
{
	uint8_t out[6] = { XL_C_STATUS, 0, 0, 0, 0, 0 }, in[6];
	xio_select(1);
	xio_xfer(out, in, 6);
	xio_select(0);
	if (in[0] != XL_SYNC)
		return -1;
	s->flags = in[1];
	s->state = in[2];
	s->txlen = (uint16_t)(in[3] | (in[4] & 7) << 8);
	if (!s->txlen)
		s->txlen = XL_MAX;
	s->rxseq = in[5];
	return 0;
}

int xl_status(xl_status_t *s)
{
	if (!attached) {
		xio_attach(1);
		attached = 1;
	}
	if (frame_status(s) == 0)
		return 0;
	/* nobody there: let go of the pins, so another image's own use of
	   them (Zeitlos ML1 puts a UART on XC/XD) never meets ours for long */
	xio_attach(0);
	attached = 0;
	return -1;
}

int xl_call(const uint8_t *req, int reqlen, uint8_t *rep, int maxrep, uint32_t timeout_ms)
{
	xl_status_t s;
	uint32_t t0 = xio_ms();
	uint8_t cmd;

	if (reqlen < 2 || reqlen > XL_MAX)
		return -3;
	/* room for it */
	for (;;) {
		if (xl_status(&s))
			return -1;
		if (s.state & 2) {
			/* a reply nobody read (a call that gave up): take it away */
			uint8_t c = XL_C_RECV;
			xio_select(1);
			xio_xfer(&c, 0, 1);
			xio_xfer(0, 0, s.txlen);
			xio_select(0);
			continue;
		}
		if (!(s.state & 1))
			break;
		if (xio_ms() - t0 > timeout_ms)
			return -2;
		xio_idle();
	}
	uint8_t seq = s.rxseq;
	cmd = XL_C_SEND;
	xio_select(1);
	xio_xfer(&cmd, 0, 1);
	xio_xfer(req, 0, reqlen);
	xio_select(0);
	/* its reply */
	for (;;) {
		if (xl_status(&s))
			return -1;
		if (s.rxseq == seq && !(s.state & 1) && !(s.state & 2))
			return -3;		/* it did not take the request */
		if (s.state & 2)
			break;
		if (xio_ms() - t0 > timeout_ms)
			return -2;
		xio_idle();
	}
	int n = s.txlen;
	if (n > maxrep)
		return -3;
	cmd = XL_C_RECV;
	xio_select(1);
	xio_xfer(&cmd, 0, 1);
	xio_xfer(0, rep, n);
	xio_select(0);
	if (n < 2 || rep[0] != (req[0] | 0x80) || rep[1] != req[1])
		return -3;
	return n;
}
