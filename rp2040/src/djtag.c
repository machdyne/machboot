/*
 * machboot supervisor
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The dirtyJTAG command stream (docs/rp2040.md), written from the
 * protocol as its hosts use it -- not from dirtyJTAG's firmware.
 *
 * The host's bulk OUT data is one byte stream: a command and its
 * arguments may straddle USB packets (an XFER of 496 bits is 64 bytes
 * of data after its header), so the parser collects each command whole
 * before it runs, wherever the packets end.
 *
 *   STOP                        end of a batch: replies go out
 *   INFO                        reply "DJTAG2\n"
 *   FREQ  khz_hi khz_lo         TCK frequency in kHz
 *   XFER[|0x40][|0x80] n data   n bits (+256 with 0x40) on TDI, MSB of
 *                               each byte first; reply the TDO bits the
 *                               same way unless 0x80 (NO_READ)
 *   SETSIG mask value           set TCK/TDI/TMS/TRST/SRST bits in mask
 *   GETSIG                      reply one byte: the outputs and TDO
 *   CLK   signals count         count TCK pulses, TMS/TDI from signals
 *   GOTOBOOTLOADER              machboot: restart into the USB boot ROM
 */

#include "djtag.h"
#include "jtag_io.h"
#include <string.h>

static uint8_t cmd[2 + 64];	/* the longest: XFER with 511 bits */
static uint32_t have, need;

void djtag_init(void)
{
	have = 0;
	need = 0;
	jio_init();
}

/* how long the command that starts with c (and has b1 after it) is */
static uint32_t cmd_len(void)
{
	uint8_t c = cmd[0] & 0x3f;
	switch (c) {
	case CMD_STOP:
	case CMD_INFO:
	case CMD_GETSIG:
	case CMD_GOTOBOOTLOADER:
		return 1;
	case CMD_FREQ:
	case CMD_SETSIG:
	case CMD_CLK:
		return 3;
	case CMD_XFER:
		if (have < 2)
			return 2;	/* the length byte first */
		{
			uint32_t bits = cmd[1] + ((cmd[0] & XFER_EXTEND) ? 256u : 0u);
			return 2 + (bits + 7) / 8;
		}
	default:
		return 1;		/* unknown: skipped, one byte */
	}
}

static void run(void)
{
	uint8_t c = cmd[0];
	switch (c & 0x3f) {
	case CMD_STOP:
		djtag_flush();
		break;
	case CMD_INFO:
		djtag_reply((const uint8_t *)DJTAG_INFO, sizeof(DJTAG_INFO) - 1);
		djtag_flush();
		break;
	case CMD_FREQ:
		jio_freq((uint32_t)cmd[1] << 8 | cmd[2]);
		break;
	case CMD_XFER: {
		uint32_t bits = cmd[1] + ((c & XFER_EXTEND) ? 256u : 0u);
		uint8_t tdo[64];
		if (c & XFER_NO_READ) {
			jio_shift(cmd + 2, 0, bits);
		} else {
			memset(tdo, 0, sizeof(tdo));
			jio_shift(cmd + 2, tdo, bits);
			djtag_reply(tdo, (bits + 7) / 8);
			djtag_flush();
		}
		break;
	}
	case CMD_SETSIG:
		jio_set(cmd[1], cmd[2]);
		break;
	case CMD_GETSIG: {
		uint8_t s = jio_get();
		djtag_reply(&s, 1);
		djtag_flush();
		break;
	}
	case CMD_CLK:
		jio_clk((cmd[1] & SIG_TMS) != 0, (cmd[1] & SIG_TDI) != 0, cmd[2]);
		break;
	case CMD_GOTOBOOTLOADER:
		djtag_flush();
		jio_bootloader();
		break;
	default:
		break;
	}
}

void djtag_feed(const uint8_t *p, uint32_t n)
{
	while (n) {
		cmd[have++] = *p++;
		n--;
		need = cmd_len();
		while (have < need && n) {
			uint32_t k = need - have < n ? need - have : n;
			memcpy(cmd + have, p, k);
			have += k;
			p += k;
			n -= k;
			need = cmd_len();	/* XFER: now it knows its length */
		}
		if (have < need)
			return;			/* the rest comes in the next packet */
		run();
		have = 0;
	}
}
