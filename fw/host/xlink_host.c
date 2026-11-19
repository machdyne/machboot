/*
 * machboot
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The PC build's XA-XD link (Sergei, Mozart): rtl/mb_xlink.v's CPU-side
 * registers and buffers, and an RP2040 on the other side that sends
 * requests the way rp2040/src/msc.c does -- one at a time, the next
 * only after the reply (docs/xlink.md). The wire itself is
 * tests/xlink_tb.cc's business.
 *
 * Script tokens (fw/host/hw_host.c passes "xl:..." here):
 *
 *   xl:hello                 HELLO (the RP2040 also sends one every second
 *                            by itself once "xl:on" is given)
 *   xl:on / xl:off           the RP2040 runs machboot-sv (sends HELLOs) or not
 *   xl:read:LBA:N:FILE       READ, N sectors (1-3), the data to FILE
 *   xl:write:LBA:FILE        WRITE, FILE's bytes (1-3 sectors)
 *   xl:eject                 EJECT: the PC ejected the drive
 *   xl:wait                  wait until nothing is outstanding
 *   xl:set:TEXT              SET: the settings the RP2040 keeps (_ is a space, | a newline, ~ a |)
 *   xl:note:TEXT             NOTE: "a system did not start" (_ is a space)
 *   xl:cachesize:N           the system cache's size (default the RP2040's, 0x2F0000)
 *   xl:put:NAME:VER:FILE     a bitstream in the cache already (as an earlier machboot left it)
 *
 * The system cache is rp2040/src/cache.c itself, on flash in memory; the
 * RP2040 serves machboot's requests (FLAGS bit 3: REQ, CHUNK..., DONE)
 * as rp2040/src/msc.c does, and reports a RUN as a JTAG load would go.
 *
 * Once "xl:on" is given, the RP2040 also answers FLAGS as machboot-sv does:
 * bit 1 (starting) with START, bit 2 (settings changed) with GET, whose
 * text it reports and keeps; and it SETs what it keeps when it first
 * hears machboot.
 *
 * Reports on stderr: [xl: ...] lines.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "../../rp2040/src/cache.h"
#include "../xlink_proto.h"

uint32_t ms_now(void);

int xl_present;				/* hw_host.c: the "sechzig" setup token */

static uint8_t rxbuf[2048], txbuf[2048];
static int rx_full, rx_len, rx_seq, tx_armed, tx_len;
static uint32_t flags;

typedef struct { uint8_t msg[2048]; int len; char file[256]; } req_t;
static req_t q[16];
static int qh, qt, outstanding, alive;
static uint8_t tag;
static uint32_t next_hello;
static char kept[1024] = "";		/* the settings the RP2040 keeps */
static int greeted;

/* -- the system cache: cache.c on a flash in memory ---------------------- */

#define CA_SIZE 0x2F0000
static uint8_t caflash[CA_SIZE];
static uint32_t ca_size = CA_SIZE;
static int ca_up;
static void ca_erase(uint32_t off, uint32_t len)
{
	if ((off | len) & 4095 || off + len > ca_size) { fprintf(stderr, "[xl: cache: bad erase %x %x]\n", off, len); exit(3); }
	memset(caflash + off, 0xFF, len);
}
static void ca_program(uint32_t off, const uint8_t *page)
{
	if (off & 255 || off + 256 > ca_size) { fprintf(stderr, "[xl: cache: bad program %x]\n", off); exit(3); }
	for (int i = 0; i < 256; i++)
		caflash[off + i] &= page[i];		/* NOR: bits only go to 0 */
}
static cache_io_t ca_io = { caflash, CA_SIZE, ca_erase, ca_program };
static void ca_init(void)
{
	if (ca_up)
		return;
	memset(caflash, 0xFF, sizeof(caflash));
	ca_io.size = ca_size;
	cache_init(&ca_io);
	ca_up = 1;
}

/* machboot's request in progress: 1 REQ out, 2 a PUT's CHUNKs */
static int rq_phase, rq_kind;
static uint32_t put_len, put_off, run_crc;

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

static req_t *push(int op)
{
	req_t *r = &q[qt];
	qt = (qt + 1) % 16;
	memset(r, 0, sizeof(*r));
	r->msg[0] = (uint8_t)op;
	r->msg[1] = ++tag;
	r->msg[2] = 1;			/* a PC has configured the USB */
	r->len = 4;
	return r;
}

static uint32_t g32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static void done(int kind, int result, const uint8_t *d, int n)
{
	req_t *r = push(XL_OP_DONE);
	r->msg[4] = (uint8_t)kind;
	r->msg[5] = (uint8_t)result;
	if (n)
		memcpy(r->msg + 8, d, (size_t)n);
	r->len = 8 + n;
}

static void chunk(void)
{
	req_t *r = push(XL_OP_CHUNK);
	uint32_t k = put_len - put_off < XL_CHUNK ? put_len - put_off : XL_CHUNK;
	put32(r->msg + 4, put_off);
	put32(r->msg + 8, k);
	r->len = 12;
}

static void rq_reply(int st, int n)
{
	if (st != 0 || n < 8) {
		fprintf(stderr, "[xl: REQ: status %d: nothing asked]\n", st);
		rq_phase = 0;
		return;
	}
	rq_kind = txbuf[4];
	const uint8_t *a = txbuf + 8;
	if (rq_kind == XL_RQ_LIST) {
		static uint8_t out[16 + 32 * XL_ITEM];
		cache_item_t it[32];
		uint32_t largest, fr = cache_free(&largest);
		int k = cache_list(it, 32);
		memset(out, 0, 16);
		out[0] = (uint8_t)k;
		put32(out + 4, fr); put32(out + 8, largest); put32(out + 12, cache_size());
		for (int i = 0; i < k; i++) {
			uint8_t *q = out + 16 + i * XL_ITEM;
			put32(q, it[i].crc); put32(q + 4, it[i].len); put32(q + 8, it[i].off);
			memcpy(q + 12, it[i].name, 24); memcpy(q + 36, it[i].ver, 16);
		}
		fprintf(stderr, "[xl: LIST: %d in the cache, %u KB free, largest gap %u KB]\n", k, fr / 1024, largest / 1024);
		done(rq_kind, XL_R_OK, out, 16 + k * XL_ITEM);
	} else if (rq_kind == XL_RQ_PUT && n >= 8 + 44) {
		char name[25], ver[17];
		memcpy(name, a + 4, 24); name[24] = 0;
		memcpy(ver, a + 28, 16); ver[16] = 0;
		put_len = g32(a);
		put_off = 0;
		int r = cache_begin(put_len, name, ver);
		fprintf(stderr, "[xl: PUT: %s %s, %u bytes: %s]\n", name, ver, put_len, r == CACHE_OK ? "copying" : "no room");
		if (r != CACHE_OK) {
			done(rq_kind, XL_R_NOROOM, 0, 0);
		} else {
			rq_phase = 2;
			chunk();
		}
	} else if (rq_kind == XL_RQ_RUN && n >= 12) {
		uint32_t crc = g32(a);
		int ok = cache_find(crc) != 0;
		fprintf(stderr, "[xl: RUN asked: %08x %s]\n", crc, ok ? "in the cache" : "not in the cache");
		if (ok)
			run_crc = crc;
		done(rq_kind, ok ? XL_R_OK : XL_R_NOTFOUND, 0, 0);
	} else if (rq_kind == XL_RQ_DROP && n >= 12) {
		uint32_t crc = g32(a);
		int r = cache_drop(crc);
		fprintf(stderr, "[xl: DROP: %08x: %s]\n", crc, r == CACHE_OK ? "erased" : "not there");
		done(rq_kind, r == CACHE_OK ? XL_R_OK : XL_R_NOTFOUND, 0, 0);
	} else {
		done(rq_kind, XL_R_ERROR, 0, 0);
	}
}

static void chunk_reply(int st, int n)
{
	uint32_t k = put_len - put_off < XL_CHUNK ? put_len - put_off : XL_CHUNK;
	if (st != XL_R_OK || (uint32_t)n != 4 + k || cache_data(txbuf + 4, k) != CACHE_OK) {
		cache_abort();
		int res = st != XL_R_OK ? st : XL_R_ERROR;
		fprintf(stderr, "[xl: PUT: %s at %u of %u bytes]\n", res == XL_R_CANCELLED ? "cancelled" : "failed", put_off, put_len);
		uint8_t d[4] = { 0, 0, 0, 0 };
		done(rq_kind, res, d, 4);
		return;
	}
	put_off += k;
	if (put_off < put_len) {
		chunk();
		return;
	}
	uint32_t crc = 0;
	int r = cache_end(&crc);
	fprintf(stderr, "[xl: PUT: done, CRC %08x: %s]\n", crc, r == CACHE_OK ? "in the cache" : "failed");
	uint8_t d[4];
	put32(d, crc);
	done(rq_kind, r == CACHE_OK ? XL_R_OK : XL_R_ERROR, d, 4);
}

/* the RP2040's side: hand over the next request, take a reply */
static void rp_step(void)
{
	if (!xl_present)
		return;
	ca_init();
	if (alive && rq_phase == 0 && qh == qt && !outstanding && (flags & XL_F_REQUEST)) {
		push(XL_OP_REQ);
		rq_phase = 1;
	}
	if (alive && !greeted && qh == qt && !outstanding) {
		/* it has just seen machboot: the settings it keeps, first */
		greeted = 1;
		if (kept[0]) {
			req_t *r = push(6);
			int n = (int)strlen(kept);
			memcpy(r->msg + 4, kept, (size_t)n);
			r->len = 4 + n;
		}
	}
	if (alive && !rq_phase && qh == qt && !outstanding && (flags & 2))
		push(5);			/* START */
	if (alive && !rq_phase && qh == qt && !outstanding && (flags & 4))
		push(7);			/* GET */
	if (alive && !rq_phase && (int32_t)(ms_now() - next_hello) >= 0 && qh == qt && !outstanding) {
		push(1);
		next_hello = ms_now() + 1000;
	}
	if (outstanding && tx_armed) {
		req_t *r = &q[(qh + 15) % 16];
		int n = tx_len ? tx_len : 2048;
		tx_armed = 0;
		outstanding = 0;
		if (txbuf[0] != (r->msg[0] | 0x80) || txbuf[1] != r->msg[1]) {
			fprintf(stderr, "[xl: reply %02x tag %d does not match request %02x tag %d]\n",
			        txbuf[0], txbuf[1], r->msg[0], r->msg[1]);
			return;
		}
		int op = r->msg[0], st = txbuf[2], cnt = txbuf[3];
		if (op == 1)
			fprintf(stderr, "[xl: HELLO: shared %d, %u sectors, flags %02x]\n", txbuf[4],
			        txbuf[8] | txbuf[9] << 8 | txbuf[10] << 16 | (uint32_t)txbuf[11] << 24, flags);
		else if (op == 2) {
			fprintf(stderr, "[xl: READ: status %d, %d sectors, %d bytes]\n", st, cnt, n);
			if (r->file[0] && st == 0) {
				FILE *f = fopen(r->file, "wb");
				fwrite(txbuf + 4, 1, (size_t)cnt * 512, f);
				fclose(f);
			}
		} else if (op == 3)
			fprintf(stderr, "[xl: WRITE: status %d, %d sectors]\n", st, cnt);
		else if (op == 5)
			fprintf(stderr, "[xl: START: status %d: the next reset boots the module's flash]\n", st);
		else if (op == 6)
			fprintf(stderr, "[xl: SET: status %d]\n", st);
		else if (op == XL_OP_REQ)
			rq_reply(st, n);
		else if (op == XL_OP_CHUNK)
			chunk_reply(st, n);
		else if (op == XL_OP_DONE) {
			fprintf(stderr, "[xl: DONE: kind %d, result %d]\n", r->msg[4], r->msg[5]);
			if (run_crc) {
				const cache_item_t *it = cache_find(run_crc);
				fprintf(stderr, "[xl: RUN: %s %s, %u bytes, loaded into the FPGA over JTAG]\n",
				        it ? it->name : "?", it ? it->ver : "", it ? it->len : 0);
				run_crc = 0;
			}
			rq_phase = 0;
		} else if (op == XL_OP_NOTE)
			fprintf(stderr, "[xl: NOTE: status %d]\n", st);
		else if (op == 7) {
			int tn = n - 4 < 1023 ? n - 4 : 1023;
			memcpy(kept, txbuf + 4, (size_t)tn);
			kept[tn] = 0;
			char shown[1024];
			for (int i = 0; i <= tn; i++) shown[i] = kept[i] == '\n' ? '|' : kept[i];
			fprintf(stderr, "[xl: GET: %s]\n", shown);
		}
		else
			fprintf(stderr, "[xl: op %d: status %d]\n", op, st);
		return;
	}
	if (!outstanding && !rx_full && qh != qt) {
		req_t *r = &q[qh];
		qh = (qh + 1) % 16;
		memcpy(rxbuf, r->msg, (size_t)r->len);
		rx_len = r->len;
		rx_full = 1;
		rx_seq++;
		outstanding = 1;
	}
}

int xl_busy(void)
{
	return qh != qt || outstanding;
}

void xl_script(const char *c)
{
	if (!strcmp(c, "on")) { alive = 1; greeted = 0; next_hello = ms_now(); return; }
	if (!strncmp(c, "set:", 4)) {
		int n = 0;
		for (const char *s = c + 4; *s && n < 255; s++)
			kept[n++] = *s == '_' ? ' ' : *s == '|' ? '\n' : *s == '~' ? '|' : *s;
		kept[n] = 0;
		req_t *r = push(6);
		memcpy(r->msg + 4, kept, (size_t)n);
		r->len = 4 + n;
		return;
	}
	if (!strcmp(c, "off")) { alive = 0; return; }
	if (!strncmp(c, "note:", 5)) {
		req_t *r = push(XL_OP_NOTE);
		int n = 0;
		for (const char *s = c + 5; *s && n < 200; s++)
			r->msg[4 + n++] = (uint8_t)(*s == '_' ? ' ' : *s);
		r->len = 4 + n;
		return;
	}
	if (!strncmp(c, "cachesize:", 10)) {
		ca_size = (uint32_t)strtoul(c + 10, 0, 0) & ~4095u;
		if (ca_size > CA_SIZE) ca_size = CA_SIZE;
		ca_up = 0;
		ca_init();
		return;
	}
	if (!strncmp(c, "put:", 4)) {
		char name[32], ver[32], file[256];
		if (sscanf(c + 4, "%31[^:]:%31[^:]:%255s", name, ver, file) != 3) exit(2);
		FILE *f = fopen(file, "rb");
		if (!f) { perror(file); exit(2); }
		static uint8_t b[CA_SIZE];
		uint32_t n = (uint32_t)fread(b, 1, sizeof(b), f);
		fclose(f);
		uint32_t skip = 0;
		for (uint32_t i = 3; i + 1 < n && i < 4096; i++)
			if (b[i] == 0xBD && b[i + 1] == 0xB3 && b[i - 1] == 0xFF && b[i - 2] == 0xFF && b[i - 3] == 0xFF) { skip = i - 3; break; }
		uint32_t crc = 0;
		ca_init();
		if (cache_begin(n - skip, name, ver) || cache_data(b + skip, n - skip) || cache_end(&crc)) {
			fprintf(stderr, "xl: put %s: no room\n", name);
			exit(2);
		}
		fprintf(stderr, "[xl: cache has %s %s, CRC %08x]\n", name, ver, crc);
		return;
	}
	if (!strcmp(c, "hello")) { push(1); return; }
	if (!strcmp(c, "eject")) { push(4); return; }
	if (!strncmp(c, "read:", 5)) {
		unsigned lba, n;
		char file[256] = "";
		if (sscanf(c + 5, "%u:%u:%255s", &lba, &n, file) < 2) exit(2);
		req_t *r = push(2);
		put32(r->msg + 4, lba);
		r->msg[8] = (uint8_t)n;
		r->len = 12;
		strcpy(r->file, file);
		return;
	}
	if (!strncmp(c, "write:", 6)) {
		unsigned lba;
		char file[256];
		if (sscanf(c + 6, "%u:%255s", &lba, file) != 2) exit(2);
		FILE *f = fopen(file, "rb");
		if (!f) { perror(file); exit(2); }
		req_t *r = push(3);
		size_t n = fread(r->msg + 12, 1, 3 * 512, f);
		fclose(f);
		put32(r->msg + 4, lba);
		r->msg[8] = (uint8_t)(n / 512);
		r->len = 12 + (int)n;
		return;
	}
	fprintf(stderr, "xl: unknown %s\n", c);
	exit(2);
}

/* -- rtl/mb_xlink.v's CPU side ----------------------------------------------- */

uint32_t xl_rd(uint32_t off)
{
	rp_step();
	if (off >= 0x1000 && off < 0x1800) {
		uint32_t o = off - 0x1000;
		return rxbuf[o] | rxbuf[o + 1] << 8 | rxbuf[o + 2] << 16 | (uint32_t)rxbuf[o + 3] << 24;
	}
	switch (off) {
	case 0x0000: return xl_present ? 0x4B4E4C58u : 0;
	case 0x0004: return (uint32_t)(rx_len & 0x7ff) << 16 | (uint32_t)(rx_seq & 0xff) << 8 | (uint32_t)rx_full;
	case 0x0008: return (uint32_t)tx_armed;
	case 0x000C: return flags;
	default: return 0;
	}
}

void xl_wr(uint32_t off, uint32_t v)
{
	if (off >= 0x1800 && off < 0x2000) {
		put32(txbuf + (off - 0x1800), v);
		return;
	}
	switch (off) {
	case 0x0004: if (v & 1) rx_full = 0; break;
	case 0x0008: tx_armed = (v >> 31) & 1; tx_len = v & 0x7ff; break;
	case 0x000C: flags = v & 0xff; break;
	}
	rp_step();
}
