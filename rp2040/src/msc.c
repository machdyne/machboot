/*
 * machboot supervisor
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The card reader's logic: msc.h. The requests are fw/xlink.c's (the
 * FPGA side): HELLO, READ, WRITE, EJECT.
 */

#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include "msc.h"
#include "xlink.h"
#include "xlink_io.h"

#include "../../fw/xlink_proto.h"
#include "cache.h"

#define OP_HELLO	XL_OP_HELLO
#define OP_READ		XL_OP_READ
#define OP_WRITE	XL_OP_WRITE
#define OP_EJECT	XL_OP_EJECT
#define OP_START	XL_OP_START
#define OP_SET		XL_OP_SET
#define OP_GET		XL_OP_GET

#define F_SHARED	0x01
#define F_STARTING	0x02
#define F_SETTINGS	0x04

/*
 * When the RP2040 may touch XA-XD. machboot is in the FPGA only after a
 * load (ours, or the PC's over JTAG); other images may use these balls
 * (Zeitlos's ML1 builds: a UART on XC/XD), so the link is probed only in
 * a short window after such an event, and polled while machboot answers.
 */
enum { L_DOWN, L_PROBE, L_UP };
static int lstate = L_DOWN;
static uint32_t ev_at, last_poll;
static int probe_i;
static const uint16_t probe_ms[] = { 20, 40, 70, 100, 150, 250, 400, 600, 1000, 1500, 2500, 4000 };
#define N_PROBES (int)(sizeof(probe_ms) / sizeof(probe_ms[0]))
#define T_POLL_MS 20
static const msc_hooks_t *hooks;

#define T_HELLO_MS	1000
#define T_CALL_MS	2000		/* an SD card may take a while for a write */
#define T_HELLO_WAIT_MS	250		/* the background HELLO: never hold USB up for long */

static uint8_t tag, pc;
static int link_up, shared;
static uint8_t gen_seen, gen_now;
static int gen_valid;
static uint32_t sectors, hello_at;
static uint8_t msg[XL_MAX], rep[XL_MAX];

/*
 * Fetch-ahead: after a READ callback returns, TinyUSB's hardware sends
 * those sectors to the PC by itself; meanwhile msc_background() fetches
 * the next MSC_MAX_SECT over the link, so a sequential READ (the PC
 * reads files and FAT tables in long runs) finds them here. A WRITE,
 * an eject, or a change of sharing forgets them.
 */
static uint8_t pf_buf[MSC_MAX_SECT * 512];
static uint32_t pf_lba, pf_n, pf_next;
static int pf_valid, pf_want;

static void pf_forget(void)
{
	pf_valid = pf_want = 0;
}

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static int last_len;

static int busy;			/* a request of machboot's in progress (request()) */

static int call(int op, uint32_t lba, uint32_t n, const uint8_t *data, int *status)
{
	if (busy)
		return -2;		/* (from USB, during a request): not now, and not a lost link */
	msg[0] = (uint8_t)op;
	msg[1] = ++tag;
	msg[2] = pc;
	msg[3] = 0;
	int len = 4;
	if (op == OP_READ || op == OP_WRITE) {
		put32(msg + 4, lba);
		msg[8] = (uint8_t)n;
		msg[9] = msg[10] = msg[11] = 0;
		len = 12;
		if (op == OP_WRITE) {
			memcpy(msg + 12, data, n * 512);
			len += (int)(n * 512);
		}
	}
	if (lstate != L_UP)
		return -1;
	int r = xl_call(msg, len, rep, sizeof(rep), op == OP_HELLO ? T_HELLO_WAIT_MS : T_CALL_MS);
	if (r == -1) {
		lstate = L_DOWN;		/* machboot is gone */
		link_up = 0;
		shared = 0;
	}
	if (r < 4)
		return -1;
	last_len = r;
	*status = rep[2];
	return r;
}

static void hello(void)
{
	int st;
	hello_at = xio_ms();
	if (call(OP_HELLO, 0, 0, 0, &st) >= 12 && st == 0) {
		shared = rep[4] & 1;
		sectors = get32(rep + 8);
	} else {
		shared = 0;
	}
}

void msc_set_pc(int configured)
{
	pc = configured ? 1 : 0;
}

void msc_hooks(const msc_hooks_t *h)
{
	hooks = h;
}

void msc_link_event(void)
{
	if (lstate == L_UP)
		return;
	lstate = L_PROBE;
	ev_at = xio_ms();
	probe_i = 0;
}

void msc_link_drop(void)
{
	lstate = L_DOWN;
	link_up = 0;
	shared = 0;
	gen_valid = 0;
	pf_valid = pf_want = 0;
}

int msc_link_state(void)
{
	return lstate;
}

static int send(int op, const uint8_t *p, int n, int *status);

/* something the supervisor wants machboot to show (a system that did not start) */
static void note_out(void)
{
	if (hooks && hooks->note_get) {
		char t[120];
		int st, n = hooks->note_get(t, sizeof(t));
		if (n > 0)
			send(XL_OP_NOTE, (const uint8_t *)t, n, &st);
	}
}

/* machboot has just answered (s: its STATUS): hand it the settings,
   then the usual. Its sharing state now is where "medium changed" counts
   from. */
static void greet(const xl_status_t *s)
{
	lstate = L_UP;
	link_up = 1;
	gen_seen = s->flags >> 4;
	gen_valid = 1;
	shared = s->flags & F_SHARED;
	if (hooks && hooks->settings_get) {
		int n = hooks->settings_get((char *)msg + 4, XL_MAX - 4);
		if (n > 0) {
			msg[0] = OP_SET; msg[1] = ++tag; msg[2] = pc; msg[3] = 0;
			xl_call(msg, 4 + n, rep, sizeof(rep), T_CALL_MS);
		}
	}
	hello();
	note_out();
}

/* a message of ours with a payload: [op][tag][pc][0] + p; the reply in rep */
static int send(int op, const uint8_t *p, int n, int *status)
{
	if (lstate != L_UP)
		return -1;
	msg[0] = (uint8_t)op; msg[1] = ++tag; msg[2] = pc; msg[3] = 0;
	if (n)
		memcpy(msg + 4, p, (size_t)n);
	int r = xl_call(msg, 4 + n, rep, sizeof(rep), T_CALL_MS);
	if (r == -1) {
		msc_link_drop();
		return -1;
	}
	if (r < 4)
		return -1;
	last_len = r;
	*status = rep[2];
	return r;
}


/* a line for the supervisor's log (msc_hooks' log, if any) */
static void say(const char *fmt, ...)
{
	char b[120];
	va_list ap;
	if (!hooks || !hooks->log)
		return;
	va_start(ap, fmt);
	vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	hooks->log(b);
}

static void done(int kind, int result, const uint8_t *p, int n)
{
	static uint8_t d[XL_MAX - 4];		/* static: the RP2040's stack is 2 KB */
	int st;
	d[0] = (uint8_t)kind; d[1] = (uint8_t)result; d[2] = d[3] = 0;
	if (n)
		memcpy(d + 4, p, (size_t)n);
	send(XL_OP_DONE, d, 4 + n, &st);
}

/* machboot asked for something (FLAGS bit 3): what, and do it */
static void request(void)
{
	int st;
	if (send(XL_OP_REQ, 0, 0, &st) < 8)
		return;
	uint8_t a[64];
	int an = last_len - 8 < (int)sizeof(a) ? last_len - 8 : (int)sizeof(a);
	int kind = rep[4];
	memcpy(a, rep + 8, (size_t)(an > 0 ? an : 0));
	busy = 1;
	if (kind == XL_RQ_LIST) {
		static uint8_t out[16 + 32 * XL_ITEM];	/* < XL_MAX - 8 */
		static cache_item_t it[32];
		uint32_t largest, freeb = cache_free(&largest);
		int n = cache_list(it, 32);
		out[0] = (uint8_t)n; out[1] = out[2] = out[3] = 0;
		put32(out + 4, freeb);
		put32(out + 8, largest);
		put32(out + 12, cache_size());
		for (int i = 0; i < n; i++) {
			uint8_t *q = out + 16 + i * XL_ITEM;
			put32(q, it[i].crc); put32(q + 4, it[i].len); put32(q + 8, it[i].off);
			memcpy(q + 12, it[i].name, 24); memcpy(q + 36, it[i].ver, 16);
		}
		done(kind, XL_R_OK, out, 16 + n * XL_ITEM);
	} else if (kind == XL_RQ_PUT && an >= 44) {
		uint32_t len = get32(a);
		char name[25], ver[17];
		memcpy(name, a + 4, 24); name[24] = 0;
		memcpy(ver, a + 28, 16); ver[16] = 0;
		int r = cache_begin(len, name, ver);
		uint32_t t0 = xio_ms(), off = 0;
		say("cache: copying %s %s, %u bytes: %s", name, ver, (unsigned)len, r == CACHE_OK ? "begun" : "no room");
		if (r != CACHE_OK) {
			done(kind, r == CACHE_NOROOM ? XL_R_NOROOM : XL_R_ERROR, 0, 0);
		} else {
			int res = XL_R_OK;
			for (off = 0; off < len; off += XL_CHUNK) {
				uint8_t c[8];
				uint32_t k = len - off < XL_CHUNK ? len - off : XL_CHUNK;
				put32(c, off); put32(c + 4, k);
				int got = send(XL_OP_CHUNK, c, 8, &st);
				if (got < 0) { res = XL_R_NOLINK; break; }
				if (st != XL_R_OK) { res = st; break; }
				if ((uint32_t)got != 4 + k || cache_data(rep + 4, k) != CACHE_OK) { res = XL_R_ERROR; break; }
				if (hooks && hooks->poll)
					hooks->poll();		/* USB meanwhile */
			}
			uint32_t crc = 0;
			if (res == XL_R_OK && cache_end(&crc) != CACHE_OK)
				res = XL_R_ERROR;
			if (res != XL_R_OK)
				cache_abort();
			uint8_t d[4];
			put32(d, crc);
			say("cache: %s %s: %s at %u of %u bytes, %u ms%s", name, ver,
			    res == XL_R_OK ? "copied" : res == XL_R_CANCELLED ? "cancelled" :
			    res == XL_R_NOLINK ? "machboot stopped answering" : "failed",
			    (unsigned)(off < len ? off : len), (unsigned)len, (unsigned)(xio_ms() - t0),
			    res == XL_R_OK ? "" : " (nothing kept)");
			if (res != XL_R_NOLINK)
				done(kind, res, d, 4);
		}
	} else if (kind == XL_RQ_RUN && an >= 4) {
		uint32_t crc = get32(a);
		int ok = cache_find(crc) != 0;
		say("cache: start %08x asked for: %s", (unsigned)crc, ok ? "in the cache" : "not in the cache");
		done(kind, ok ? XL_R_OK : XL_R_NOTFOUND, 0, 0);
		if (ok && hooks && hooks->run)
			hooks->run(crc);		/* machboot is going: the system comes */
	} else if (kind == XL_RQ_DROP && an >= 4) {
		int r = cache_drop(get32(a));
		say("cache: remove %08x: %s", (unsigned)get32(a), r == CACHE_OK ? "erased" : "not there");
		done(kind, r == CACHE_OK ? XL_R_OK : XL_R_NOTFOUND, 0, 0);
	} else {
		done(kind, XL_R_ERROR, 0, 0);
	}
	busy = 0;
}

static void on_flags(uint8_t f)
{
	if (f & XL_F_REQUEST)
		request();
	int st;
	if (f & F_STARTING) {
		if (call(OP_START, 0, 0, 0, &st) >= 4 && hooks && hooks->start)
			hooks->start();		/* the next reset boots the module's flash */
	}
	if ((f & F_SETTINGS) && call(OP_GET, 0, 0, 0, &st) >= 4 && st == 0) {
		/* the reply's text follows its 4-byte header */
		if (hooks && hooks->settings_put)
			hooks->settings_put((const char *)rep + 4, last_len - 4);
	}
}

void msc_tick(void)
{
	if (busy)
		return;			/* (from a request's poll) */
	xl_status_t s;
	uint32_t now = xio_ms();
	if (lstate == L_PROBE) {
		if (probe_i >= N_PROBES) {
			lstate = L_DOWN;
			return;
		}
		if (now - ev_at < probe_ms[probe_i])
			return;
		probe_i++;
		if (xl_status(&s) == 0)
			greet(&s);
		return;
	}
	if (lstate != L_UP || now - last_poll < T_POLL_MS)
		return;
	last_poll = now;
	if (xl_status(&s)) {
		msc_link_drop();
		return;
	}
	if (s.flags & (F_STARTING | F_SETTINGS | XL_F_REQUEST))
		on_flags(s.flags);
	if (lstate == L_UP && now - hello_at >= T_HELLO_MS)
		hello();
}

int msc_link(void)
{
	return link_up;
}

int msc_ready(uint8_t *key, uint8_t *asc, uint8_t *ascq)
{
	xl_status_t s;
	if (busy) {
		/* as before the request (machboot does not share the card while it
		   asks): the PC sees nothing change, and does not start probing */
		*key = 0x02; *asc = 0x3A; *ascq = 0x00;		/* NOT READY, medium not present */
		return 0;
	}
	if (lstate != L_UP || xl_status(&s)) {
		if (lstate == L_UP)
			msc_link_drop();
		link_up = 0;
		*key = 0x02; *asc = 0x3A; *ascq = 0x00;		/* NOT READY, medium not present */
		return 0;
	}
	link_up = 1;
	gen_now = s.flags >> 4;
	if (!gen_valid || gen_now != gen_seen) {
		pf_forget();
		/* shared or given back since we last looked: new size, and tell the PC */
		int first = !gen_valid;
		gen_seen = gen_now;
		gen_valid = 1;
		hello();
		if (!first && (s.flags & 1)) {
			*key = 0x06; *asc = 0x28; *ascq = 0x00;	/* UNIT ATTENTION, medium changed */
			return 0;
		}
	}
	if (!(s.flags & 1)) {
		shared = 0;
		pf_forget();
		*key = 0x02; *asc = 0x3A; *ascq = 0x00;
		return 0;
	}
	return 1;
}

uint32_t msc_sectors(void)
{
	return shared ? sectors : 0;
}

static int fetch(uint32_t lba, uint8_t *buf, uint32_t n);

int msc_read(uint32_t lba, uint8_t *buf, uint32_t n)
{
	if (busy)
		return -1;
	/* what the fetch-ahead has, first */
	while (n && pf_valid && lba >= pf_lba && lba < pf_lba + pf_n) {
		memcpy(buf, pf_buf + (lba - pf_lba) * 512, 512);
		buf += 512;
		lba++;
		n--;
	}
	int r = fetch(lba, buf, n);
	if (r == 0) {
		pf_next = lba + n;
		pf_want = 1;
	}
	return r;
}

void msc_background(void)
{
	if (busy)
		return;
	if (!pf_want)
		return;
	pf_want = 0;
	pf_valid = 0;
	uint32_t n = MSC_MAX_SECT;
	if (!shared || pf_next >= sectors)
		return;
	if (pf_next + n > sectors)
		n = sectors - pf_next;
	if (fetch(pf_next, pf_buf, n) == 0) {
		pf_lba = pf_next;
		pf_n = n;
		pf_valid = 1;
	}
}

static int fetch(uint32_t lba, uint8_t *buf, uint32_t n)
{
	while (n) {
		uint32_t k = n > MSC_MAX_SECT ? MSC_MAX_SECT : n;
		int st, r = call(OP_READ, lba, k, 0, &st);
		if (r < 0)
			return -2;
		if (st == 1)
			return -1;
		if (st != 0 || rep[3] != k || r != (int)(4 + k * 512))
			return -2;
		memcpy(buf, rep + 4, k * 512);
		buf += k * 512;
		lba += k;
		n -= k;
	}
	return 0;
}

int msc_write(uint32_t lba, const uint8_t *buf, uint32_t n)
{
	if (busy)
		return -1;
	pf_forget();
	while (n) {
		uint32_t k = n > MSC_MAX_SECT ? MSC_MAX_SECT : n;
		int st, r = call(OP_WRITE, lba, k, buf, &st);
		if (r < 0)
			return -2;
		if (st == 1)
			return -1;
		if (st != 0 || rep[3] != k)
			return -2;
		buf += k * 512;
		lba += k;
		n -= k;
	}
	return 0;
}

void msc_eject(void)
{
	int st;
	call(OP_EJECT, 0, 0, 0, &st);
	shared = 0;
	pf_forget();
}
