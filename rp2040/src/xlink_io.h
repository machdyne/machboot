/*
 * machboot supervisor
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The pins of the XA-XD link, for xlink.c: src/xlink_rp2040.c on the
 * board, tests/test_xlink.c on a PC.
 */

#ifndef XLINK_IO_H
#define XLINK_IO_H

#include <stdint.h>

void xio_attach(int on);	/* on: SCK, MOSI and CS driven (CS high); off: all inputs */
void xio_select(int on);	/* CS low (on) or high */
void xio_xfer(const uint8_t *out, uint8_t *in, int n);	/* out may be 0 (zeros), in may be 0 */
uint32_t xio_ms(void);
void xio_idle(void);		/* while waiting: keep USB going */

#endif
