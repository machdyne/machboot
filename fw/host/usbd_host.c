/*
 * machboot
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The PC build's USB device: rtl/mb_usbd.v's registers and buffers, and
 * a USB host on the other side that runs control and bulk transfers
 * against them a packet at a time, the way the SIE would (docs/usb-device.md).
 *
 * Script tokens (fw/host/hw_host.c passes "ud:..." here):
 *
 *   ud:enum            bus reset, enumerate, configure, open the console (DTR)
 *   ud:dfu:FILE        dfu-util -a 0 -D FILE: DNLOAD blocks of 4096 with
 *                      GETSTATUS after each, the empty DNLOAD, GETSTATUS
 *   ud:upload:FILE     dfu-util -a 0 -U FILE: UPLOAD until a short block
 *   ud:msc:tur         mass storage (endpoint 3): TEST UNIT READY, and
 *                      REQUEST SENSE if it fails
 *   ud:msc:cap         READ CAPACITY
 *   ud:msc:read:LBA:N:FILE   READ (10), the data to FILE
 *   ud:msc:write:LBA:FILE    WRITE (10), FILE's bytes (a multiple of 512)
 *   ud:msc:eject       START STOP UNIT, eject
 *   ud:cdc:TEXT        type TEXT on the console (bulk OUT); _ is a space,
 *                      % is Enter, ^ is Esc
 *   ud:wait            the script waits until the host has nothing left to do
 *
 * The host reports on stderr: [ud: ...] lines.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../hw.h"

/* -- the device's registers ---------------------------------------------- */

static int attach;
static uint32_t dev_addr, events, eptype;
static int out_full[4], out_setup[4], out_len[4], in_armed[4], in_len[4], stall[4];
static uint8_t obuf[4][64], ibuf[4][64];
int ud_present;			/* hw_host.c: "usbd" setup token */

static void host_step(void);

uint32_t usbd_rd(uint32_t off)
{
	if (!ud_present)
		return 0;
	if (off == UD_EVENTS)
		host_step();
	if (off == UD_MAGIC) return UD_MAGIC_VAL;
	if (off == UD_CTRL) return (uint32_t)attach | dev_addr << 8;
	if (off == UD_EVENTS) return events;
	if (off == UD_EPTYPE) return eptype;
	if (off >= 0x040 && off < 0x080) {
		int ep = (int)((off - 0x040) >> 4), r = (int)((off >> 2) & 3);
		if (r == 0) return (uint32_t)out_full[ep] | (uint32_t)out_setup[ep] << 1 | (uint32_t)out_len[ep] << 8;
		if (r == 1) return (uint32_t)in_armed[ep] << 8;
		if (r == 2) return (uint32_t)stall[ep];
		return 0;
	}
	if (off >= 0x400 && off < 0x600) {
		int ep = (int)((off - 0x400) >> 7), o = (int)(off & 0x3f);
		if (off & 0x40)
			return 0;
		return (uint32_t)obuf[ep][o] | (uint32_t)obuf[ep][o + 1] << 8 |
		       (uint32_t)obuf[ep][o + 2] << 16 | (uint32_t)obuf[ep][o + 3] << 24;
	}
	return 0;
}

void usbd_wr(uint32_t off, uint32_t v)
{
	if (!ud_present)
		return;
	if (off == UD_CTRL) { attach = v & 1; dev_addr = (v >> 8) & 0x7f; return; }
	if (off == UD_EVENTS) { if (v & 1) events &= ~1u; return; }
	if (off == UD_EPTYPE) { eptype = v; return; }
	if (off == UD_TOGGLE) return;
	if (off >= 0x040 && off < 0x080) {
		int ep = (int)((off - 0x040) >> 4), r = (int)((off >> 2) & 3);
		if (r == 0) { out_full[ep] = 0; out_setup[ep] = 0; }
		if (r == 1) { in_armed[ep] = (v >> 8) & 1; in_len[ep] = (int)(v & 0x7f); }
		if (r == 2) stall[ep] = (int)(v & 1);
		return;
	}
	if (off >= 0x400 && off < 0x600 && (off & 0x40)) {
		int ep = (int)((off - 0x400) >> 7), o = (int)(off & 0x3f);
		for (int k = 0; k < 4; k++)
			ibuf[ep][o + k] = (uint8_t)(v >> (8 * k));
	}
}

/* -- the host ------------------------------------------------------------- */

/*
 * A queue of control transfers and bulk packets, each run a packet at a
 * time: one step per poll of the device, as a real host's packets
 * arrive between the firmware's polls.
 */

enum { X_CTRL, X_BULK_OUT, X_BULK_IN, X_RESET, X_NOTE };
typedef struct {
	int kind;
	uint8_t setup[8];
	uint8_t *data;		/* OUT data, or where IN data goes */
	int len;		/* wLength, or the bulk length */
	int (*after)(void *x);	/* when done: may queue more; nonzero stops the job */
	int done, got, stalled, phase, off;
	int ep;
	const char *note;
	uint8_t *aux;		/* mass storage: the command's data, its length, a file for it */
	int aux_len;
	char *file;
} xfer_t;

#define XMAX 64
static xfer_t xq[XMAX];
static int xh, xt;
static int polls;

static xfer_t *xadd(int kind)
{
	xfer_t *x = &xq[xt++ % XMAX];
	memset(x, 0, sizeof(*x));
	x->kind = kind;
	return x;
}

static xfer_t *ctrl(uint8_t bm, uint8_t req, uint16_t val, uint16_t idx, uint16_t len, uint8_t *data)
{
	xfer_t *x = xadd(X_CTRL);
	uint8_t s[8] = { bm, req, (uint8_t)val, (uint8_t)(val >> 8), (uint8_t)idx, (uint8_t)(idx >> 8),
	                 (uint8_t)len, (uint8_t)(len >> 8) };
	memcpy(x->setup, s, 8);
	x->len = len;
	x->data = data;
	return x;
}

int ud_busy(void) { return xh != xt; }

/* one packet's worth of progress on the transfer at the head */
static void collect_cdc(void);

static void host_step(void)
{
	collect_cdc();
	if (++polls % 2)
		return;		/* every other poll: the firmware gets time in between */
	if (xh == xt)
		return;
	xfer_t *x = &xq[xh % XMAX];

	if (x->kind == X_NOTE) {
		fprintf(stderr, "[ud: %s]\n", x->note);
		xh++;
		return;
	}
	if (x->kind == X_RESET) {
		events |= 1;
		dev_addr = 0;
		memset(out_full, 0, sizeof(out_full));
		memset(in_armed, 0, sizeof(in_armed));
		memset(stall, 0, sizeof(stall));
		xh++;
		return;
	}
	if (!attach)
		return;
	if (x->kind == X_BULK_OUT) {
		int ep = x->ep ? x->ep : 1;
		if (out_full[ep] || !(eptype & (0x10u << ep)))
			return;		/* NAK: try again later */
		int n = x->len - x->off < 64 ? x->len - x->off : 64;
		memcpy(obuf[ep], x->data + x->off, (size_t)n);
		out_len[ep] = n;
		out_full[ep] = 1;
		x->off += n;
		if (x->off >= x->len) {
			xh++;
			if (x->after && x->after(x))
				xh = xt;
		}
		return;
	}
	if (x->kind == X_BULK_IN) {
		if (!in_armed[x->ep])
			return;		/* NAK */
		int n = in_len[x->ep];
		if (x->got + n <= x->len)
			memcpy(x->data + x->got, ibuf[x->ep], (size_t)n);
		x->got += n;
		in_armed[x->ep] = 0;
		if (n < 64 || x->got >= x->len) {
			xh++;
			if (x->after && x->after(x))
				xh = xt;
		}
		return;
	}

	/* a control transfer */
	int in = x->setup[0] & 0x80;
	switch (x->phase) {
	case 0:		/* SETUP: always accepted */
		memcpy(obuf[0], x->setup, 8);
		out_len[0] = 8;
		out_full[0] = 1;
		out_setup[0] = 1;
		in_armed[0] = 0;
		stall[0] = 0;
		x->phase = x->len ? 1 : 2;
		return;
	case 1:		/* data */
		if (stall[0]) { x->stalled = 1; x->phase = 3; return; }
		if (in) {
			if (!in_armed[0])
				return;
			int n = in_len[0];
			if (x->got + n <= x->len && x->data)
				memcpy(x->data + x->got, ibuf[0], (size_t)n);
			x->got += n;
			in_armed[0] = 0;		/* ACK */
			if (n < 64 || x->got >= x->len)
				x->phase = 2;
		} else {
			if (out_full[0])
				return;
			int n = x->len - x->got < 64 ? x->len - x->got : 64;
			memcpy(obuf[0], x->data + x->got, (size_t)n);
			out_len[0] = n;
			out_full[0] = 1;
			out_setup[0] = 0;
			x->got += n;
			if (x->got >= x->len)
				x->phase = 2;
		}
		return;
	case 2:		/* status: the other direction, zero length */
		if (stall[0]) { x->stalled = 1; x->phase = 3; return; }
		if (in) {
			if (out_full[0])
				return;
			out_len[0] = 0;
			out_full[0] = 1;
			out_setup[0] = 0;
		} else {
			if (!in_armed[0])
				return;
			if (in_len[0] != 0) {
				fprintf(stderr, "[ud: status stage with %d bytes]\n", in_len[0]);
				x->stalled = 1;
			}
			in_armed[0] = 0;
		}
		x->phase = 3;
		return;
	default:
		break;
	}
	x->done = 1;
	xh++;
	if (x->stalled)
		fprintf(stderr, "[ud: request %02x %02x stalled]\n", x->setup[0], x->setup[1]);
	if (x->after && x->after(x)) {
		/* the job stops: drop what it queued */
		xh = xt;
	}
}

/* -- jobs ------------------------------------------------------------------- */

static uint8_t desc[256];

static int after_dev(void *p)
{
	xfer_t *x = p;
	fprintf(stderr, "[ud: device %04x:%04x, class %02x, %d bytes]\n",
	        desc[8] | desc[9] << 8, desc[10] | desc[11] << 8, desc[4], x->got);
	return x->stalled;
}

static int after_cfg(void *p)
{
	xfer_t *x = p;
	fprintf(stderr, "[ud: configuration, %d bytes:", x->got);
	for (int i = 0; i < x->got; i += desc[i]) {
		if (!desc[i]) break;
		if (desc[i + 1] == 4)
			fprintf(stderr, " if%d/%02x.%02x.%02x", desc[i + 2], desc[i + 5], desc[i + 6], desc[i + 7]);
		if (desc[i + 1] == 0x21)
			fprintf(stderr, " dfu(xfer %d)", desc[i + 5] | desc[i + 6] << 8);
	}
	fprintf(stderr, "]\n");
	return x->stalled;
}

static int after_str(void *p)
{
	xfer_t *x = p;
	char s[64];
	int n = 0;
	for (int i = 2; i + 1 < x->got && n < 63; i += 2)
		s[n++] = (char)desc[i];
	s[n] = 0;
	fprintf(stderr, "[ud: string %d \"%s\"]\n", x->setup[2], s);
	return x->stalled;
}

static uint8_t *dfu_file;
static int dfu_size, dfu_block;
static uint8_t dfu_st[6];
static FILE *up_file;
static uint8_t up_buf[4096];
static int up_block;

static void dfu_next(void);

static int after_status(void *p)
{
	xfer_t *x = p;
	if (x->stalled || dfu_st[0] != 0) {
		fprintf(stderr, "[ud: dfu: status %d, state %d -- dfu-util: error]\n", dfu_st[0], dfu_st[4]);
		return 1;
	}
	if (dfu_block * 4096 >= dfu_size + 4096) {
		fprintf(stderr, "[ud: dfu: done, state %d]\n", dfu_st[4]);
		return 0;
	}
	dfu_next();
	return 0;
}

static int after_dnload(void *p)
{
	xfer_t *x = p;
	if (x->stalled) {
		fprintf(stderr, "[ud: dfu: DNLOAD %d stalled -- dfu-util: error]\n", x->setup[2] | x->setup[3] << 8);
		ctrl(0xA1, 3, 0, 2, 6, dfu_st);		/* GETSTATUS, to see why */
		xq[(xt - 1) % XMAX].after = after_status;
		return 0;
	}
	xfer_t *s = ctrl(0xA1, 3, 0, 2, 6, dfu_st);
	s->after = after_status;
	return 0;
}

static void dfu_next(void)
{
	int off = dfu_block * 4096;
	int n = dfu_size - off;
	if (n > 4096) n = 4096;
	if (n < 0) n = 0;
	xfer_t *x = ctrl(0x21, 1, (uint16_t)dfu_block, 2, (uint16_t)n, dfu_file + off);
	x->after = after_dnload;
	dfu_block++;
}

static int after_upload(void *p)
{
	xfer_t *x = p;
	if (x->stalled) {
		fprintf(stderr, "[ud: upload stalled]\n");
		return 1;
	}
	fwrite(up_buf, 1, (size_t)x->got, up_file);
	if (x->got < 4096) {
		fclose(up_file);
		fprintf(stderr, "[ud: upload: %d blocks, done]\n", up_block);
		return 0;
	}
	xfer_t *y = ctrl(0xA1, 2, (uint16_t)up_block++, 2, 4096, up_buf);
	y->after = after_upload;
	return 0;
}

static uint8_t cdc_text[256];

/* -- mass storage: bulk-only transport on endpoint 3 -- */

static uint8_t m_csw[13], m_sense[18];
static int m_tag = 1;
static char m_file[256];

static int after_csw(void *p)
{
	xfer_t *x = p;
	const char *m_name = x->note;
	uint8_t *m_data = x->aux;
	int m_data_len = x->aux_len;
	const char *m_file = x->file ? x->file : "";
	uint32_t res = (uint32_t)m_csw[8] | (uint32_t)m_csw[9] << 8 | (uint32_t)m_csw[10] << 16 | (uint32_t)m_csw[11] << 24;
	int okcsw = x->got == 13 && !memcmp(m_csw, "USBS", 4);
	fprintf(stderr, "[ud: msc: %s: %s, status %d, residue %u]\n", m_name, okcsw ? "CSW" : "NO CSW", m_csw[12], res);
	if (!strcmp(m_name, "capacity") && m_csw[12] == 0)
		fprintf(stderr, "[ud: msc: %u sectors of %u bytes]\n",
		        ((uint32_t)m_data[0] << 24 | (uint32_t)m_data[1] << 16 | (uint32_t)m_data[2] << 8 | m_data[3]) + 1,
		        (uint32_t)m_data[4] << 24 | (uint32_t)m_data[5] << 16 | (uint32_t)m_data[6] << 8 | m_data[7]);
	if (!strcmp(m_name, "sense"))
		fprintf(stderr, "[ud: msc: sense %02x/%02x/%02x]\n", m_data[2], m_data[12], m_data[13]);
	if (!strcmp(m_name, "read") && m_file[0]) {
		FILE *f = fopen(m_file, "wb");
		fwrite(m_data, 1, (size_t)m_data_len, f);
		fclose(f);
	}
	return 0;
}

static void msc_cmd(const char *name, const uint8_t *cb, int cblen, int in, uint8_t *data, int len)
{
	uint8_t *m_cbw = calloc(1, 31);	/* its own: two commands may be queued at once */
	memcpy(m_cbw, "USBC", 4);
	m_cbw[4] = (uint8_t)m_tag++;
	m_cbw[8] = (uint8_t)len; m_cbw[9] = (uint8_t)(len >> 8); m_cbw[10] = (uint8_t)(len >> 16); m_cbw[11] = (uint8_t)(len >> 24);
	m_cbw[12] = in ? 0x80 : 0;
	m_cbw[14] = (uint8_t)cblen;
	memcpy(m_cbw + 15, cb, (size_t)cblen);
	xfer_t *x = xadd(X_BULK_OUT);
	x->ep = 3; x->data = m_cbw; x->len = 31;
	if (len) {
		x = xadd(in ? X_BULK_IN : X_BULK_OUT);
		x->ep = 3; x->data = data; x->len = len;
	}
	x = xadd(X_BULK_IN);
	x->ep = 3; x->data = m_csw; x->len = 13;
	x->after = after_csw;
	x->note = name;
	x->aux = data;
	x->aux_len = len;
	x->file = m_file[0] ? strdup(m_file) : 0;
}


static void msc_script(const char *c)
{
	uint8_t cb[16];
	memset(cb, 0, sizeof(cb));
	m_file[0] = 0;
	if (!strcmp(c, "tur")) {
		msc_cmd("test unit ready", cb, 6, 0, 0, 0);
		cb[0] = 0x03; cb[4] = 18;
		msc_cmd("sense", cb, 6, 1, m_sense, 18);
		/* REQUEST SENSE's data lands in m_sense; after_csw reads m_data */
		return;
	}
	if (!strcmp(c, "cap")) {
		cb[0] = 0x25;
		msc_cmd("capacity", cb, 10, 1, malloc(8), 8);
		return;
	}
	if (!strcmp(c, "eject")) {
		cb[0] = 0x1B; cb[4] = 0x02;
		msc_cmd("eject", cb, 6, 0, 0, 0);
		return;
	}
	if (!strncmp(c, "read:", 5)) {
		unsigned lba, n;
		if (sscanf(c + 5, "%u:%u:%255s", &lba, &n, m_file) != 3) exit(2);
		cb[0] = 0x28;
		cb[2] = (uint8_t)(lba >> 24); cb[3] = (uint8_t)(lba >> 16); cb[4] = (uint8_t)(lba >> 8); cb[5] = (uint8_t)lba;
		cb[7] = (uint8_t)(n >> 8); cb[8] = (uint8_t)n;
		msc_cmd("read", cb, 10, 1, malloc((size_t)n * 512), (int)n * 512);
		return;
	}
	if (!strncmp(c, "write:", 6)) {
		unsigned lba;
		char path[256];
		if (sscanf(c + 6, "%u:%255s", &lba, path) != 2) exit(2);
		FILE *f = fopen(path, "rb");
		if (!f) { perror(path); exit(1); }
		uint8_t *wbuf = malloc(1 << 20);	/* its own: commands are queued before they run */
		int len = (int)fread(wbuf, 1, 1 << 20, f);
		fclose(f);
		unsigned n = (unsigned)len / 512;
		cb[0] = 0x2A;
		cb[2] = (uint8_t)(lba >> 24); cb[3] = (uint8_t)(lba >> 16); cb[4] = (uint8_t)(lba >> 8); cb[5] = (uint8_t)lba;
		cb[7] = (uint8_t)(n >> 8); cb[8] = (uint8_t)n;
		msc_cmd("write", cb, 10, 0, wbuf, (int)n * 512);
		return;
	}
	fprintf(stderr, "unknown ud:msc: token %s\n", c);
	exit(2);
}

void ud_script(const char *cmd)
{
	if (!strcmp(cmd, "enum")) {
		xadd(X_RESET);
		ctrl(0x80, 6, 0x0100, 0, 64, desc)->after = after_dev;		/* GET_DESCRIPTOR device, at 0 */
		ctrl(0x00, 5, 5, 0, 0, 0);						/* SET_ADDRESS 5 */
		ctrl(0x80, 6, 0x0200, 0, 9, desc);					/* configuration, header */
		ctrl(0x80, 6, 0x0200, 0, 255, desc)->after = after_cfg;
		ctrl(0x80, 6, 0x0302, 0x0409, 255, desc)->after = after_str;
		ctrl(0x80, 6, 0x0304, 0x0409, 255, desc)->after = after_str;
		ctrl(0x00, 9, 1, 0, 0, 0);						/* SET_CONFIGURATION 1 */
		ctrl(0x21, 0x22, 1, 0, 0, 0);						/* DTR */
		xadd(X_NOTE)->note = "enumerated, console open";
		return;
	}
	if (!strncmp(cmd, "dfu:", 4)) {
		FILE *f = fopen(cmd + 4, "rb");
		if (!f) { perror(cmd + 4); exit(1); }
		fseek(f, 0, SEEK_END);
		dfu_size = (int)ftell(f);
		fseek(f, 0, SEEK_SET);
		dfu_file = malloc((size_t)dfu_size + 4096);
		if (fread(dfu_file, 1, (size_t)dfu_size, f) != (size_t)dfu_size) exit(1);
		fclose(f);
		dfu_block = 0;
		fprintf(stderr, "[ud: dfu: %s, %d bytes]\n", cmd + 4, dfu_size);
		dfu_next();
		return;
	}
	if (!strncmp(cmd, "upload:", 7)) {
		up_file = fopen(cmd + 7, "wb");
		up_block = 0;
		xfer_t *y = ctrl(0xA1, 2, (uint16_t)up_block++, 2, 4096, up_buf);
		y->after = after_upload;
		return;
	}
	if (!strncmp(cmd, "msc:", 4)) {
		msc_script(cmd + 4);
		return;
	}
	if (!strncmp(cmd, "cdc:", 4)) {
		int n = 0;
		for (const char *s = cmd + 4; *s && n < 255; s++)
			cdc_text[n++] = *s == '_' ? ' ' : *s == '%' ? '\r' : *s == '^' ? 0x1b : (uint8_t)*s;
		xfer_t *x = xadd(X_BULK_OUT);
		x->data = cdc_text;
		x->len = n;
		return;
	}
	fprintf(stderr, "unknown ud: token %s\n", cmd);
	exit(2);
}

/* what the device sends on the console: to $MB_CDC_OUT, if set */
static FILE *cdc_out;
static void collect_cdc(void)
{
	if (!in_armed[1])
		return;
	if (!cdc_out && getenv("MB_CDC_OUT"))
		cdc_out = fopen(getenv("MB_CDC_OUT"), "wb");
	if (cdc_out) {
		fwrite(ibuf[1], 1, (size_t)in_len[1], cdc_out);
		fflush(cdc_out);
	}
	in_armed[1] = 0;
}
