/*
 * machboot
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The card reader on Sergei and Mozart: the PC's USB drive is the
 * RP2040's (rp2040/src/msc.c), and every sector it asks for comes here
 * over the XA-XD link (rtl/mb_xlink.v, docs/xlink.md). This file stands
 * in for fw/usbd.c and keeps the same state, so the menu's Card reader
 * page works unchanged.
 *
 * The RP2040 sends one request at a time and waits for its reply:
 *
 *   request   [op][tag][pc][0] then op's arguments
 *               pc bit 0: a PC has configured the RP2040's USB
 *   reply     [op|0x80][tag][status][n] then op's data
 *               status 0 ok, 1 no medium (not shared), 2 I/O error,
 *               3 bad request
 *
 *   1 HELLO   reply data: [shared][0][0][0][sectors, 4 LE]
 *   2 READ    args [lba, 4 LE][count][0][0][0]; count 1-3;
 *             reply n = count, data count x 512 bytes
 *   3 WRITE   args as READ, then count x 512 bytes; reply n = sectors written
 *   4 EJECT   the PC ejected the drive: the card is machboot's again
 *   5 START   the RP2040 has seen FLAGS bit 1: it lets the next reset
 *             boot the module's flash, and machboot may pull PROGRAMN
 *   6 SET     the settings the RP2040 keeps, as text: "key=value\n"...
 *             (boot.auto, boot.wait, boot.kbd)
 *   7 GET     reply data: the settings as text (FLAGS bit 2 asked for it)
 *   8 REQ     what machboot asks for (FLAGS bit 3): reply [kind][0][0][0]
 *             and its arguments (fw/xlink_proto.h, docs/systems.md)
 *   9 CHUNK   args [off 4][n 4]: n bytes of the bitstream a PUT copies;
 *             reply status 4 when the user cancelled
 *  10 DONE    [kind][result][0][0] and data: the request's outcome
 *  11 NOTE    text: the RP2040 could not start a system
 *
 * FLAGS (sent in every STATUS frame, no message needed):
 *   bit 0     the card is shared (a medium is present)
 *   bit 1     machboot is about to start the system (waits for START)
 *   bit 2     the settings changed: the RP2040 should GET them
 *   bit 3     machboot asks for something: the RP2040 sends REQ
 *   bits 7:4  counts each change of bit 0, so the RP2040 can report
 *             "medium changed" even if it missed the moment
 */

#include <stdint.h>
#include <string.h>
#include "usbd.h"
#include "hw.h"
#include "mb.h"
#include "sd.h"
#include "console.h"
#include "settings.h"
#include "lib.h"
#include "fat.h"
#include "flash.h"
#include "rcache.h"
#include "systems.h"

#define XL_MAGIC	0x0000u
#define XL_RX		0x0004u
#define XL_TX		0x0008u
#define XL_FLAGS	0x000Cu
#define XL_FRAMES	0x0010u
#define XL_RXBUF	0x1000u
#define XL_TXBUF	0x1800u
#define XL_MAGIC_VAL	0x4B4E4C58u	/* "XLNK" */

#define XL_MAX_SECT	3

usbd_state_t usbd;

static uint32_t sectw[128];		/* word-aligned: sd_read's fast path, word copies */
#define sect ((uint8_t *)sectw)
static uint8_t gen;
static uint32_t heard_at;

/*
 * Read-ahead: after answering READ lba..lba+n-1, the next XL_MAX_SECT
 * sectors are read from the card while the RP2040 is busy taking the
 * reply and handing it to the PC, so a sequential READ is answered at
 * once. Any WRITE, and any change of sharing, forgets it.
 */
static uint32_t ra_bufw[XL_MAX_SECT * 128];
#define ra_buf ((uint8_t *)ra_bufw)
static uint32_t ra_lba, ra_n, ra_next;
static int ra_valid, ra_want;

static void ra_forget(void)
{
	ra_valid = ra_want = 0;
}

static void ra_fill(void)
{
	uint32_t n = 0;
	ra_want = 0;
	ra_valid = 0;
	if (!usbd.msc_shared || !sd.present)
		return;
	while (n < XL_MAX_SECT && (!sd.sectors || ra_next + n < sd.sectors) &&
	       !sd_read(ra_next + n, ra_buf + n * 512))
		n++;
	if (n) {
		ra_lba = ra_next;
		ra_n = n;
		ra_valid = 1;
	}
}

static int starting, start_ok, got_settings, dirty_shown;

/* a request of machboot's (rcache.h): raised, taken by REQ, done by DONE */
enum { RQ_NONE, RQ_RAISED, RQ_TAKEN, RQ_DONE };
static int rq_state, rq_kind, rq_argn, rq_result, rq_cancel;
static uint32_t rq_argw[16];
#define rq_args ((uint8_t *)rq_argw)
static uint32_t rq_crc;			/* DONE's data: a PUT's CRC */
static rc_list_t *rq_list;		/* DONE's data: a LIST, parsed into this */
static uint32_t rq_dn;
#define TXT_MAX 1024
static uint32_t txtw[TXT_MAX / 4];	/* SET's and GET's text */
/* a PUT's file: the bitstream from its preamble (skip bytes in) */
static fat_file_t rq_f;
static uint32_t rq_skip, rq_len, rq_pos;
static char note_txt[100];

static void set_flags(void)
{
	xl_wr(XL_FLAGS, (uint32_t)(gen & 0x0f) << 4 | (usbd.msc_shared && sd.present ? 1u : 0u) |
	                (starting ? 2u : 0u) | (settings_dirty ? 4u : 0u) |
	                (rq_state == RQ_RAISED ? (uint32_t)XL_F_REQUEST : 0u));
}

/* -- settings as text, both ways ---------------------------------------- */

static int settings_text(char *b, int cap)
{
	int n = mb_snprintf(b, (size_t)cap, "boot.auto=%d\nboot.wait=%d\nboot.kbd=%s\n",
	                    cfg.autostart ? 1 : 0, cfg.wait_s, cfg.kbd == KBD_DE ? "de" : "us");
	if (n < 0 || n >= cap)
		return 0;
	return n + systems_text(b + n, cap - n);
}

static void settings_from_text(const char *s, uint32_t n)
{
	systems_clear();		/* the text has the whole list */
	while (n) {
		const char *line = s;
		uint32_t l = 0;
		while (l < n && s[l] != '\n')
			l++;
		s += l < n ? l + 1 : l;
		n -= l < n ? l + 1 : l;
		if (l > 10 && !memcmp(line, "boot.auto=", 10))
			cfg.autostart = line[10] != '0';
		else if (l > 10 && !memcmp(line, "boot.wait=", 10)) {
			int w = 0;
			for (uint32_t i = 10; i < l && line[i] >= '0' && line[i] <= '9'; i++)
				w = w * 10 + (line[i] - '0');
			if (w >= 1 && w <= 30)
				cfg.wait_s = w;
		} else if (l > 9 && !memcmp(line, "boot.kbd=", 9))
			cfg.kbd = line[9] == 'd' ? KBD_DE : KBD_US;
		else
			systems_line(line, l);
	}
}

int usbd_init(void)
{
	memset(&usbd, 0, sizeof(usbd));
	if (!(mb.features & FEAT_RPLINK) || xl_rd(XL_MAGIC) != XL_MAGIC_VAL)
		return 0;
	usbd.present = 1;
	usbd.rp2040 = 1;
	xl_wr(XL_TX, 0);		/* no stale reply */
	xl_wr(XL_RX, 1);		/* nor a stale request */
	set_flags();
	/* The RP2040 hands over the settings it keeps as soon as it sees us
	   (it looks every few ms after loading the menu itself); without
	   them the defaults stand, and later ones still apply. */
	uint32_t t0 = ms_now();
	while (!got_settings && ms_now() - t0 < 400)
		usbd_poll();
	if (got_settings)
		con_puts("xlink: settings from the RP2040\n");
	return 1;
}

void usbd_before_start(void)
{
	if (usbd.rp2040 != 2)
		return;			/* no machboot-sv: nobody to tell */
	starting = 1;
	start_ok = 0;
	set_flags();
	uint32_t t0 = ms_now();
	while (!start_ok && ms_now() - t0 < 300)
		usbd_poll();
	starting = 0;
	set_flags();
}

void usbd_msc_share(int on)
{
	if (!usbd.present || on == usbd.msc_shared)
		return;
	usbd.msc_shared = on;
	ra_forget();
	if (on)
		usbd.msc_read = usbd.msc_written = 0;
	gen++;
	set_flags();
	usbd.msc_events++;
}

void usbd_cdc_putc(char c)
{
	(void)c;			/* the console reaches the PC through the module's UART */
}

int usbd_cdc_getc(void)
{
	return -1;
}

static uint32_t rx_word(uint32_t off)
{
	return xl_rd(XL_RXBUF + off);
}

static uint32_t le32(uint32_t w)
{
	return w;			/* the buffers are little-endian words, as the CPU */
}

/* put a sector (512 bytes, word-aligned) at offset off of the reply buffer */
static void tx_put(uint32_t off, const uint8_t *p, uint32_t n)
{
	const uint32_t *w = (const uint32_t *)(const void *)p;
	for (uint32_t i = 0; i < n / 4; i++)
		xl_wr(XL_TXBUF + off + i * 4, w[i]);
}

static void reply(uint32_t op, uint32_t tag, uint32_t status, uint32_t n, uint32_t len)
{
	xl_wr(XL_TXBUF, (op | 0x80u) | tag << 8 | status << 16 | n << 24);
	xl_wr(XL_TX, (1u << 31) | (len & 0x7ffu));
}

/* DONE's LIST data, at RX offset 8: [n][0][0][0][free][largest][size] + items */
static void rx_bytes(uint8_t *d, uint32_t off, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++)
		d[i] = (uint8_t)(rx_word((off + i) & ~3u) >> (8 * ((off + i) & 3)));
}

static void list_from_rx(rc_list_t *l, uint32_t dn)
{
	int n = (int)(rx_word(8) & 0xff);
	if (16 + (uint32_t)n * XL_ITEM > dn)
		return;
	l->free = rx_word(12);
	l->largest = rx_word(16);
	l->size = rx_word(20);
	if (n > RC_MAX)
		n = RC_MAX;		/* (the rest is not listed: never evicted, never shown) */
	for (int i = 0; i < n; i++) {
		uint32_t q = 8 + 16 + (uint32_t)i * XL_ITEM;	/* XL_ITEM is a multiple of 4 */
		l->it[i].crc = rx_word(q);
		l->it[i].len = rx_word(q + 4);
		l->it[i].off = rx_word(q + 8);
		rx_bytes((uint8_t *)l->it[i].name, q + 12, 24); l->it[i].name[23] = 0;
		rx_bytes((uint8_t *)l->it[i].ver, q + 36, 16); l->it[i].ver[15] = 0;
	}
	l->n = n;
}

static void serve(uint32_t len)
{
	uint32_t h = rx_word(0);
	uint32_t op = h & 0xff, tag = (h >> 8) & 0xff, pc = (h >> 16) & 0xff;
	uint32_t lba = 0, count = 0;

	usbd.configured = pc & 1;
	if (len >= 12) {
		lba = le32(rx_word(4));
		count = rx_word(8) & 0xff;
	}

	switch (op) {
	case 1: {			/* HELLO */
		uint32_t w[2];
		w[0] = (usbd.msc_shared && sd.present) ? 1u : 0u;
		w[1] = sd.present ? sd.sectors : 0;
		xl_wr(XL_TXBUF + 4, w[0]);
		xl_wr(XL_TXBUF + 8, w[1]);
		reply(op, tag, 0, 0, 12);
		return;
	}
	case 2:				/* READ */
	case 3: {			/* WRITE */
		if (len < 12 || count < 1 || count > XL_MAX_SECT ||
		    (op == 3 && len < 12 + count * 512)) {
			reply(op, tag, 3, 0, 4);
			return;
		}
		if (!usbd.msc_shared || !sd.present) {
			reply(op, tag, 1, 0, 4);
			return;
		}
		uint32_t done = 0;
		if (op == 3)
			ra_forget();
		for (; done < count; done++) {
			if (op == 2) {
				uint32_t l = lba + done;
				if (ra_valid && l >= ra_lba && l < ra_lba + ra_n) {
					tx_put(4 + done * 512, ra_buf + (l - ra_lba) * 512, 512);
				} else {
					if (sd_read(l, sect))
						break;
					tx_put(4 + done * 512, sect, 512);
				}
				usbd.msc_read += 512;
			} else {
				uint32_t base = 12 + done * 512;
				for (uint32_t i = 0; i < 128; i++)
					sectw[i] = rx_word(base + i * 4);
				if (sd_write(lba + done, sect))
					break;
				usbd.msc_written += 512;
			}
		}
		usbd.msc_events++;
		reply(op, tag, done == count ? 0 : 2, done, op == 2 ? 4 + done * 512 : 4);
		if (op == 2 && done == count) {
			ra_next = lba + count;
			ra_want = 1;
		}
		return;
	}
	case 4:				/* EJECT */
		usbd_msc_share(0);
		reply(op, tag, 0, 0, 4);
		return;
	case 5:				/* START: the RP2040 lets the next reset through */
		start_ok = 1;
		reply(op, tag, 0, 0, 4);
		return;
	case 6: {			/* SET: the settings the RP2040 keeps */
		char *txt = (char *)txtw;
		uint32_t n = len - 4 < TXT_MAX ? len - 4 : TXT_MAX;
		for (uint32_t i = 0; i < n; i += 4) {
			uint32_t w = rx_word(4 + i);
			for (uint32_t k = 0; k < 4 && i + k < n; k++)
				txt[i + k] = (char)(w >> (8 * k));
		}
		settings_from_text(txt, n);
		kbd_layout = cfg.kbd;
		got_settings = 1;
		reply(op, tag, 0, 0, 4);
		usbd.msc_events++;	/* the menu redraws */
		return;
	}
	case 7: {			/* GET */
		memset(txtw, 0, TXT_MAX);
		int n = settings_text((char *)txtw, TXT_MAX);
		for (int i = 0; i < (n + 3) / 4; i++)
			xl_wr(XL_TXBUF + 4 + (uint32_t)i * 4, txtw[i]);
		/* FLAGS bit 2 off before the reply is there to be taken, so the
		   RP2040's next STATUS does not ask again */
		settings_dirty = dirty_shown = 0;
		set_flags();
		reply(op, tag, 0, 0, 4 + (uint32_t)n);
		return;
	}
	case XL_OP_REQ:
		if (rq_state != RQ_RAISED) {
			reply(op, tag, 3, 0, 4);
			return;
		}
		xl_wr(XL_TXBUF + 4, (uint32_t)rq_kind);
		for (int i = 0; i < (rq_argn + 3) / 4; i++)
			xl_wr(XL_TXBUF + 8 + (uint32_t)i * 4, rq_argw[i]);
		rq_state = RQ_TAKEN;
		set_flags();		/* bit 3 off before the reply can be taken */
		reply(op, tag, 0, 0, 8 + (uint32_t)rq_argn);
		return;
	case XL_OP_CHUNK: {
		uint32_t off = rx_word(4), n = rx_word(8);
		if (rq_state != RQ_TAKEN || rq_kind != XL_RQ_PUT || n > XL_CHUNK || off + n > rq_len) {
			reply(op, tag, XL_R_ERROR, 0, 4);
			return;
		}
		if (rq_cancel) {
			reply(op, tag, XL_R_CANCELLED, 0, 4);
			return;
		}
		if (off != rq_pos) {
			/* asked again (or out of order): from the start of the file */
			fat_seek0(&rq_f);
			for (uint32_t k = 0; k < rq_skip + off; ) {
				uint32_t m = rq_skip + off - k < sizeof(ra_bufw) ? rq_skip + off - k : sizeof(ra_bufw);
				if (fat_read(&rq_f, ra_buf, m) != (int)m) {
					reply(op, tag, XL_R_ERROR, 0, 4);
					return;
				}
				k += m;
			}
		}
		if (fat_read(&rq_f, ra_buf, n) != (int)n) {
			rq_pos = 0xFFFFFFFFu;
			reply(op, tag, XL_R_ERROR, 0, 4);
			return;
		}
		tx_put(4, ra_buf, (n + 3) & ~3u);
		rq_pos = off + n;
		reply(op, tag, 0, 0, 4 + n);
		return;
	}
	case XL_OP_DONE: {
		uint32_t w = rx_word(4);
		if (rq_state == RQ_TAKEN && (int)(w & 0xff) == rq_kind) {
			rq_dn = len > 8 ? len - 8 : 0;
			rq_result = (int)((w >> 8) & 0xff);
			if (rq_dn >= 4)
				rq_crc = rx_word(8);
			if (rq_list && rq_dn >= 16 && rq_result == XL_R_OK)
				list_from_rx(rq_list, rq_dn);
			rq_state = RQ_DONE;
		}
		reply(op, tag, 0, 0, 4);
		return;
	}
	case XL_OP_NOTE: {
		uint32_t n = len - 4 < sizeof(note_txt) - 1 ? len - 4 : sizeof(note_txt) - 1;
		for (uint32_t i = 0; i < n; i += 4) {
			uint32_t w = rx_word(4 + i);
			for (uint32_t k = 0; k < 4 && i + k < n; k++)
				note_txt[i + k] = (char)(w >> (8 * k));
		}
		note_txt[n] = 0;
		con_printf("supervisor: %s\n", note_txt);
		usbd.msc_events++;	/* the menu redraws */
		reply(op, tag, 0, 0, 4);
		return;
	}
	default:
		reply(op, tag, 3, 0, 4);
		return;
	}
}

/* -- the RP2040's system cache (rcache.h) --------------------------------- */

int rc_available(void)
{
	return usbd.rp2040 == 2;
}

#define T_RQ_IDLE_MS	5000		/* no word from the RP2040 this long: gone */

static int rq_go(int kind, const void *args, int n, int (*progress)(uint32_t, uint32_t))
{
	if (!rc_available())
		return XL_R_NOLINK;
	rq_kind = kind;
	memset(rq_argw, 0, sizeof(rq_argw));
	memcpy(rq_args, args, (size_t)n);
	rq_argn = n;
	rq_cancel = 0;
	rq_dn = 0;
	rq_state = RQ_RAISED;
	set_flags();
	uint32_t t = ms_now(), seen = rq_pos;
	while (rq_state != RQ_DONE) {
		usbd_poll();
		if (rq_pos != seen) {
			seen = rq_pos;
			t = ms_now();
		}
		if (progress && progress(rq_pos == 0xFFFFFFFFu ? 0 : rq_pos, rq_len))
			rq_cancel = 1;
		if (!rc_available() || ms_now() - t > T_RQ_IDLE_MS) {
			con_printf("xlink: request %d: %s (at %u of %u bytes)\n", kind,
			           rc_available() ? "the RP2040 went quiet for 5 s" : "the RP2040 stopped answering for 3 s",
			           rq_pos == 0xFFFFFFFFu ? 0 : rq_pos, rq_len);
			rq_state = RQ_NONE;
			set_flags();
			return XL_R_NOLINK;
		}
	}
	rq_state = RQ_NONE;
	set_flags();
	return rq_result;
}

static void p32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

int rc_list(rc_list_t *l)
{
	memset(l, 0, sizeof(*l));
	l->n = -1;
	rq_list = l;
	int r = rq_go(XL_RQ_LIST, 0, 0, 0);
	rq_list = 0;
	if (r == XL_R_OK && l->n < 0)
		r = XL_R_ERROR;
	if (l->n < 0)
		l->n = 0;
	return r;
}

static uint8_t bget(void *ctx, uint32_t off)
{
	return ((const uint8_t *)ctx)[off];
}

int rc_put(card_entry_t *ce, uint32_t *crc, uint32_t *len, int (*progress)(uint32_t, uint32_t))
{
	slot_info_t si;
	uint32_t size = ce->bit.size, head = size < sizeof(ra_bufw) ? size : sizeof(ra_bufw);
	if (usbd.msc_shared)
		return RC_READ;		/* the card is the PC's */
	ra_forget();			/* ra_buf is the CHUNKs' now */
	fat_open(ce->vol, &ce->bit, &rq_f);
	if (fat_read(&rq_f, ra_buf, head) != (int)head)
		return RC_READ;
	bit_parse(bget, ra_buf, head, &si);
	if (!si.valid)
		return RC_NOT_BIT;
	if (si.idcode && si.idcode != mb.idcode)
		return RC_WRONG_FPGA;
	/* from the dummy bytes before the preamble, as openFPGALoader sends a
	   .bit: the comment header stays behind (tools/mkuf2.py does the same) */
	rq_skip = 0;
	for (uint32_t i = 3; i + 1 < head; i++)
		if (ra_buf[i] == 0xBD && ra_buf[i + 1] == 0xB3 &&
		    ra_buf[i - 1] == 0xFF && ra_buf[i - 2] == 0xFF && ra_buf[i - 3] == 0xFF) {
			rq_skip = i - 3;
			break;
		}
	rq_len = size - rq_skip;
	rq_pos = 0xFFFFFFFFu;		/* the first CHUNK seeks */
	uint8_t a[44];
	memset(a, 0, sizeof(a));
	p32(a, rq_len);
	memcpy(a + 4, ce->name, strlen(ce->name) < 23 ? strlen(ce->name) : 23);
	memcpy(a + 28, ce->ver, strlen(ce->ver) < 15 ? strlen(ce->ver) : 15);
	int r = rq_go(XL_RQ_PUT, a, sizeof(a), progress);
	rq_pos = 0;
	if (r == XL_R_OK) {
		if (rq_dn < 4)
			return XL_R_ERROR;
		*crc = rq_crc;
		*len = rq_len;
	}
	return r;
}

void rc_cancel(void)
{
	rq_cancel = 1;
}

int rc_drop(uint32_t crc)
{
	uint8_t a[4];
	p32(a, crc);
	return rq_go(XL_RQ_DROP, a, 4, 0);
}

int rc_run(uint32_t crc)
{
	uint8_t a[4];
	p32(a, crc);
	return rq_go(XL_RQ_RUN, a, 4, 0);
}

const char *rc_note(void)
{
	return note_txt[0] ? note_txt : 0;
}

void rc_note_clear(void)
{
	note_txt[0] = 0;
}

void usbd_poll(void)
{
	if (!usbd.present)
		return;
	if (settings_dirty != dirty_shown) {
		dirty_shown = settings_dirty;
		set_flags();
	}
	uint32_t rx = xl_rd(XL_RX);
	if (rx & 1) {
		uint32_t len = (rx >> 16) & 0x7ff;
		if (!len)
			len = 2048;
		/* the request stays in the buffer while it is served: the RP2040
		   sends the next one only after it has this one's reply */
		if (len >= 4)
			serve(len);
		xl_wr(XL_RX, 1);
		/* the reply is on its way: now read ahead, while the RP2040 takes it */
		if (ra_want)
			ra_fill();
		heard_at = ms_now();
		if (usbd.rp2040 != 2) {
			usbd.rp2040 = 2;
			settings_remote = 1;	/* kept by the RP2040 from now on */
			usbd.msc_events++;
			con_puts("xlink: the RP2040 answers\n");
		}
	} else if (usbd.rp2040 == 2 && ms_now() - heard_at > 3000) {
		/* its HELLO comes every second: it is gone (reset, or other firmware) */
		usbd.rp2040 = 1;
		usbd.configured = 0;
		settings_remote = 0;
		usbd.msc_events++;
	}
}
