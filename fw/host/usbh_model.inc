/*
 * machboot
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Included by hw_host.c: Zeitlos's USB host controller (rtl/usb/
 * usb_host.v's register map) with devices behind it, for the PC build.
 *
 * Written from the USB 2.0 specification (chapters 9 and 11: the
 * standard requests, the hub class), HID 1.11 (boot keyboards) and
 * Mass Storage Bulk-Only 1.0 / SCSI (flash drives). It checks what a
 * real bus would punish:
 *
 *   - a low-speed device directly on a root port answers only
 *     LOWSPEED|INVERTED; behind a hub only LOWSPEED|PRE; a full-speed
 *     device only neither -- anything else is a timeout;
 *   - two devices that answer the same address collide (a CRC error),
 *     and the model says so on stderr;
 *   - a hub port must be powered to show its device, and reset to
 *     enable it; a disabled port's device answers nothing;
 *   - bulk data toggles: a wrong one is reported on stderr.
 *
 * Script tokens (before or during a run):
 *   usb:kbd  usb:mouse  usb:hub  usb:unplug     root port 1
 *   usb:stick:FILE                              root port 1: a flash drive holding FILE
 *   usb:hub:kbd@N  usb:hub:mouse@N  usb:hub:stick@N:FILE  usb:hub:unplug@N
 *                                               behind the hub on root port 1, its port N
 *   u:KEY                                       a key on the (first) keyboard
 */

enum { K_NONE, K_KBD, K_MOUSE, K_HUB, K_STICK };
#define MD_MAX 8
#define HUB_N 4

typedef struct {
	int type;
	int lowspeed;
	int addr, new_addr, pending_addr, config;
	uint8_t resp[64 + 512];
	int resp_len, resp_pos, nak_once;
	int parent, pport;		/* -1: on root port 1 */
	/* a hub */
	struct { int child, power, enabled, c_conn, c_reset; uint32_t reset_until; } port[HUB_N + 1];
	/* a flash drive */
	uint8_t *img;
	size_t size;
	int tog_in, tog_out;
	uint8_t data[4096];
	int data_len, data_pos;		/* IN data waiting for the host */
	uint8_t csw[13];
	int csw_ready, attention;
	uint32_t wr_lba, wr_left;
} mdev_t;

static mdev_t md[MD_MAX];
static int root_dev = -1;		/* the device on root port 1 */
static int u_enabled;			/* root port 1 enabled (after a reset) */
static uint8_t ubuf[2048];
static uint32_t u_ctrl, u_xs, u_info[2], u_keys[2], u_polla[4], u_pollb[4];
static uint8_t kbd_held[6];
static uint32_t kbd_release_at;
static int n_xact, n_collisions, n_toggle_errors, n_wrong_speed;
static uint32_t u_xact_a_saved;

static const uint8_t ls_dev_desc[18] = {
	18, 1, 0x10, 0x01, 0, 0, 0, 8, 0xc4, 0x04, 0x01, 0x20, 0x00, 0x01, 1, 2, 0, 1
};
static const uint8_t hub_dev_desc[18] = {
	18, 1, 0x00, 0x02, 9, 0, 0, 64, 0xe3, 0x05, 0x08, 0x06, 0x00, 0x01, 0, 1, 0, 1
};
static const uint8_t stick_dev_desc[18] = {
	18, 1, 0x00, 0x02, 0, 0, 0, 64, 0x81, 0x07, 0x81, 0x55, 0x00, 0x01, 1, 2, 3, 1
};
static const uint8_t kbd_cfg[34] = {
	9, 2, 34, 0, 1, 1, 0, 0xa0, 50,
	9, 4, 0, 0, 1, 3, 1, 1, 0,		/* interface 0: HID, boot, keyboard */
	9, 0x21, 0x11, 0x01, 0, 1, 0x22, 63, 0,	/* HID descriptor */
	7, 5, 0x81, 3, 8, 0, 10			/* EP1 IN, interrupt, 8 bytes, 10 ms */
};
static const uint8_t mouse_cfg[34] = {
	9, 2, 34, 0, 1, 1, 0, 0xa0, 50,
	9, 4, 0, 0, 1, 3, 1, 2, 0,		/* interface 0: HID, boot, mouse */
	9, 0x21, 0x11, 0x01, 0, 1, 0x22, 50, 0,
	7, 5, 0x81, 3, 4, 0, 10
};
static const uint8_t hub_cfg[25] = {
	9, 2, 25, 0, 1, 1, 0, 0xe0, 50,
	9, 4, 0, 0, 1, 9, 0, 0, 0,		/* interface 0: hub */
	7, 5, 0x81, 3, 1, 0, 12			/* status change, 1 byte, 255 ms */
};
static const uint8_t stick_cfg[32] = {
	9, 2, 32, 0, 1, 1, 0, 0x80, 100,
	9, 4, 0, 0, 2, 8, 6, 0x50, 0,		/* mass storage, SCSI, bulk-only */
	7, 5, 0x81, 2, 64, 0, 0,		/* bulk IN */
	7, 5, 0x02, 2, 64, 0, 0			/* bulk OUT */
};
static const uint8_t hub_desc[9] = { 9, 0x29, HUB_N, 0x00, 0x00, 50, 0, 0, 0xff };

static int md_new(int type, const char *file)
{
	for (int i = 0; i < MD_MAX; i++)
		if (md[i].type == K_NONE) {
			mdev_t *d = &md[i];
			memset(d, 0, sizeof(*d));
			d->type = type;
			d->lowspeed = type == K_KBD || type == K_MOUSE;
			d->parent = -1;
			for (int p = 0; p <= HUB_N; p++)
				d->port[p].child = -1;
			if (type == K_STICK) {
				FILE *f = fopen(file, "rb");
				if (!f) { perror(file); exit(1); }
				fseek(f, 0, SEEK_END);
				d->size = (size_t)ftell(f);
				fseek(f, 0, SEEK_SET);
				d->img = malloc(d->size);
				if (fread(d->img, 1, d->size, f) != d->size) exit(1);
				fclose(f);
				d->attention = 1;
			}
			return i;
		}
	fprintf(stderr, "usb model: too many devices\n");
	exit(2);
}

static void md_free(int i)
{
	if (i < 0)
		return;
	for (int p = 1; p <= HUB_N; p++)
		md_free(md[i].port[p].child);
	free(md[i].img);
	memset(&md[i], 0, sizeof(md[i]));
}

static int type_of(const char *s)
{
	if (!strncmp(s, "kbd", 3)) return K_KBD;
	if (!strncmp(s, "mouse", 5)) return K_MOUSE;
	if (!strncmp(s, "hub", 3)) return K_HUB;
	if (!strncmp(s, "stick", 5)) return K_STICK;
	return K_NONE;
}

static void usb_script(const char *what)
{
	if (!strncmp(what, "hub:", 4) && root_dev >= 0 && md[root_dev].type == K_HUB) {
		/* behind the hub: TYPE@N[:FILE] */
		const char *at = strchr(what + 4, '@');
		if (!at) { fprintf(stderr, "usb: %s: which port?\n", what); exit(2); }
		int p = atoi(at + 1);
		const char *file = strchr(at, ':');
		mdev_t *h = &md[root_dev];
		if (p < 1 || p > HUB_N) exit(2);
		md_free(h->port[p].child);
		h->port[p].child = -1;
		h->port[p].enabled = 0;
		h->port[p].c_conn = 1;
		int t = type_of(what + 4);
		if (t != K_NONE) {
			int c = md_new(t, file ? file + 1 : 0);
			md[c].parent = root_dev;
			md[c].pport = p;
			h->port[p].child = c;
		}
		return;
	}
	md_free(root_dev);
	root_dev = -1;
	u_enabled = 0;
	int t = type_of(what);
	if (t != K_NONE) {
		const char *file = strchr(what, ':');
		root_dev = md_new(t, file ? file + 1 : 0);
	}
	u_info[0] = u_info[1] = 0;
	u_keys[0] = u_keys[1] = 0;
}

static int usage_of(const char *k)
{
	static const struct { const char *n; int u; } tab[] = {
		{ "up", 0x52 }, { "down", 0x51 }, { "left", 0x50 }, { "right", 0x4f },
		{ "enter", 0x28 }, { "esc", 0x29 }, { "f1", 0x3a }, { "f2", 0x3b },
		{ "f5", 0x3e }, { "f8", 0x41 }, { 0, 0 } };
	for (int i = 0; tab[i].n; i++)
		if (!strcmp(k, tab[i].n)) return tab[i].u;
	if (k[0] >= 'a' && k[0] <= 'z' && !k[1]) return 0x04 + (k[0] - 'a');
	fprintf(stderr, "unknown usb key %s\n", k);
	exit(2);
}

static void usb_key(const char *k)
{
	kbd_held[0] = (uint8_t)usage_of(k);
	kbd_release_at = now_ms + 30;
}

/* the device at addr on root port 1 that would hear a packet, as the bus is now */
static int reachable(int i)
{
	if (i < 0 || md[i].type == K_NONE)
		return 0;
	if (md[i].parent < 0)
		return u_enabled;
	mdev_t *h = &md[md[i].parent];
	return reachable(md[i].parent) && h->config && h->port[md[i].pport].power &&
	       h->port[md[i].pport].enabled && now_ms >= h->port[md[i].pport].reset_until;
}

static int find_dev(int addr, int xa, int *collide)
{
	int found = -1;
	*collide = 0;
	for (int i = 0; i < MD_MAX; i++) {
		if (!reachable(i) || md[i].addr != addr)
			continue;
		/* only the right signalling reaches it */
		int want = !md[i].lowspeed ? 0 : md[i].parent < 0 ? 3 : 5;
		if (xa != want) {
			if (found < 0)
				n_wrong_speed++;
			continue;
		}
		if (found >= 0)
			*collide = 1;
		found = i;
	}
	return found;
}

/* the auto-poll engine: a keyboard's report lands in its compat block */
static void usb_poll_engine(void)
{
	if (kbd_held[0] && now_ms >= kbd_release_at)
		kbd_held[0] = 0;
	int first_kbd = 1;
	for (int s = 0; s < 4; s++) {
		uint32_t a = u_polla[s], b = u_pollb[s];
		if (!(a & (1u << 30)))
			continue;
		int c, i = find_dev((int)(a & 0x7f), (int)((a >> 11) & 7), &c);
		int mode = (b >> 11) & 7, blk = (b >> 14) & 3;
		if (i < 0 || md[i].type != K_KBD || mode != 3 || ((a >> 7) & 0xf) != 1 || !md[i].config)
			continue;
		u_info[blk & 1] = (u_info[blk & 1] & 0x03000000u);
		u_keys[blk & 1] = first_kbd ? (uint32_t)kbd_held[0] << 24 : 0;
		first_kbd = 0;
	}
}

static void put16(uint8_t *p, int v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

static void setup_packet(mdev_t *d, const uint8_t *s)
{
	int bm = s[0], req = s[1], val = s[2] | s[3] << 8, idx = s[4] | s[5] << 8, len = s[6] | s[7] << 8;
	const uint8_t *src = 0;
	int n = 0;
	d->resp_len = d->resp_pos = 0;
	if (bm == 0x80 && req == 6) {
		int t = val >> 8;
		if (t == 1) {
			src = d->type == K_HUB ? hub_dev_desc : d->type == K_STICK ? stick_dev_desc : ls_dev_desc;
			n = 18;
		} else if (t == 2) {
			src = d->type == K_KBD ? kbd_cfg : d->type == K_MOUSE ? mouse_cfg : d->type == K_HUB ? hub_cfg : stick_cfg;
			n = src[2];
		}
		d->nak_once = d->lowspeed;
	} else if (bm == 0xA0 && req == 6 && d->type == K_HUB) {
		src = hub_desc;
		n = 9;
	} else if (bm == 0 && req == 5) {
		d->pending_addr = 1;
		d->new_addr = val & 0x7f;
	} else if (bm == 0 && req == 9) {
		d->config = val;
	} else if (bm == 0xA3 && req == 0 && d->type == K_HUB && idx >= 1 && idx <= HUB_N) {
		/* GET_PORT_STATUS */
		uint8_t r[4];
		int p = idx, st = 0, ch = 0;
		int conn = d->port[p].child >= 0 && d->port[p].power;
		int resetting = now_ms < d->port[p].reset_until;
		if (conn) st |= 0x0001;
		if (d->port[p].enabled && !resetting) st |= 0x0002;
		if (resetting) st |= 0x0010;
		if (d->port[p].power) st |= 0x0100;
		if (conn && md[d->port[p].child].lowspeed) st |= 0x0200;
		if (d->port[p].c_conn && d->port[p].power) ch |= 0x0001;
		if (d->port[p].c_reset && !resetting) ch |= 0x0010;
		put16(r, st);
		put16(r + 2, ch);
		memcpy(d->resp, r, 4);
		d->resp_len = len < 4 ? len : 4;
		return;
	} else if ((bm == 0x23) && (req == 3 || req == 1) && d->type == K_HUB && idx >= 1 && idx <= HUB_N) {
		int p = idx;
		if (req == 3 && val == 8) d->port[p].power = 1;
		if (req == 3 && val == 4 && d->port[p].child >= 0) {
			d->port[p].reset_until = now_ms + 12;
			d->port[p].enabled = 1;
			d->port[p].c_reset = 1;
			md[d->port[p].child].addr = 0;
			md[d->port[p].child].config = 0;
		}
		if (req == 1 && val == 16) d->port[p].c_conn = 0;
		if (req == 1 && val == 20) d->port[p].c_reset = 0;
		if (req == 1 && val == 1) d->port[p].enabled = 0;
	} else if (bm == 0x21 && req == 0xFF) {
		d->data_len = d->data_pos = 0;
		d->csw_ready = 0;
	} else if (bm == 0x02 && req == 1) {
		if (idx & 0x80) d->tog_in = 0; else d->tog_out = 0;
	}
	if (src) {
		if (n > len) n = len;
		memcpy(d->resp, src, (size_t)n);
		d->resp_len = n;
	}
}

/* -- the flash drive: bulk-only, SCSI ------------------------------------------- */

static void be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

static void scsi(mdev_t *d, const uint8_t *cbw)
{
	const uint8_t *cb = cbw + 15;
	uint32_t want = (uint32_t)cbw[8] | (uint32_t)cbw[9] << 8 | (uint32_t)cbw[10] << 16 | (uint32_t)cbw[11] << 24;
	int status = 0;
	d->data_len = d->data_pos = 0;
	switch (cb[0]) {
	case 0x00:			/* TEST UNIT READY: once UNIT ATTENTION, as many drives do */
		if (d->attention) status = 1;
		break;
	case 0x03:
		memset(d->data, 0, 18);
		d->data[0] = 0x70;
		d->data[2] = d->attention ? 6 : 0;
		d->data[12] = d->attention ? 0x28 : 0;
		d->attention = 0;
		d->data_len = 18;
		break;
	case 0x25:
		be32(d->data, (uint32_t)(d->size / 512) - 1);
		be32(d->data + 4, 512);
		d->data_len = 8;
		break;
	case 0x28: {
		uint32_t lba = (uint32_t)cb[2] << 24 | (uint32_t)cb[3] << 16 | (uint32_t)cb[4] << 8 | cb[5];
		uint32_t n = (uint32_t)cb[7] << 8 | cb[8];
		if ((lba + n) * 512ull > d->size || n * 512 > sizeof(d->data)) { status = 1; break; }
		memcpy(d->data, d->img + (size_t)lba * 512, n * 512);
		d->data_len = (int)(n * 512);
		break;
	}
	default:
		status = 1;
	}
	if ((uint32_t)d->data_len > want) d->data_len = (int)want;
	memcpy(d->csw, "USBS", 4);
	memcpy(d->csw + 4, cbw + 4, 4);
	uint32_t res = want - (uint32_t)d->data_len;
	d->csw[8] = (uint8_t)res; d->csw[9] = (uint8_t)(res >> 8); d->csw[10] = (uint8_t)(res >> 16); d->csw[11] = (uint8_t)(res >> 24);
	d->csw[12] = (uint8_t)status;
	d->csw_ready = 1;
}

static int stick_bulk(mdev_t *d, int endp, int pid, int toggle, int off, int len, int mps, int *got)
{
	int packets;
	if (pid == 2 && endp == 2) {			/* OUT */
		if (toggle != d->tog_out) { n_toggle_errors++; fprintf(stderr, "[usb model: OUT toggle %d, want %d]\n", toggle, d->tog_out); }
		packets = len ? (len + mps - 1) / mps : 1;
		d->tog_out ^= packets & 1;
		if (len == 31 && !memcmp(ubuf + off, "USBC", 4))
			scsi(d, ubuf + off);
		*got = len;
		return 0;
	}
	if (pid == 1 && endp == 1) {			/* IN */
		const uint8_t *src;
		int n;
		if (d->data_pos < d->data_len) {
			src = d->data + d->data_pos;
			n = d->data_len - d->data_pos;
			if (n > len) n = len;
			d->data_pos += n;
		} else if (d->csw_ready) {
			src = d->csw;
			n = 13 < len ? 13 : len;
			d->csw_ready = 0;
		} else {
			return 1;			/* NAK */
		}
		if (toggle != d->tog_in) { n_toggle_errors++; fprintf(stderr, "[usb model: IN toggle %d, want %d]\n", toggle, d->tog_in); }
		memcpy(ubuf + off, src, (size_t)n);
		packets = (n == len && n && n % mps == 0) ? n / mps : n / mps + 1;
		d->tog_in ^= packets & 1;
		*got = n;
		return n < len ? 6 : 0;
	}
	return 2;					/* STALL: no such endpoint */
}

static void usb_xact(uint32_t a, uint32_t b)
{
	int addr = a & 0x7f, endp = (a >> 7) & 0xf, pid = (a >> 11) & 3, xa = (a >> 13) & 7;
	int toggle = (a >> 17) & 1, mps = (a >> 19) & 0x7f;
	int off = b & 0x7ff, len = (b >> 11) & 0x7ff;
	int status = 3, got = 0, collide;	/* TIMEOUT unless a device answers */
	n_xact++;

	int i = find_dev(addr, xa, &collide);
	if (collide) {
		n_collisions++;
		fprintf(stderr, "[usb model: two devices answer address %d]\n", addr);
		u_xs = 4u << 11;
		return;
	}
	if (i >= 0 && endp != 0 && md[i].type == K_STICK && md[i].config) {
		status = stick_bulk(&md[i], endp, pid, toggle, off, len, mps ? mps : 64, &got);
		u_xs = (uint32_t)got | ((uint32_t)status << 11);
		return;
	}
	if (i >= 0 && endp == 0) {
		mdev_t *d = &md[i];
		if (pid == 0) {			/* SETUP */
			setup_packet(d, ubuf + off);
			status = 0;
		} else if (pid == 1) {		/* IN: data stage, or status stage of an OUT request */
			if (d->nak_once && d->resp_pos < d->resp_len) {
				d->nak_once = 0;
				status = 1;
			} else {
				int n = d->resp_len - d->resp_pos;
				int m0 = d->lowspeed ? 8 : 64;
				if (n > len) n = len;
				if (n > m0) n = m0;
				memcpy(ubuf + off, d->resp + d->resp_pos, (size_t)n);
				d->resp_pos += n;
				got = n;
				status = n < len ? 6 : 0;
				if (d->pending_addr) {	/* status stage of SET_ADDRESS done */
					d->pending_addr = 0;
					d->addr = d->new_addr;
				}
			}
		} else {			/* OUT: status stage of an IN request */
			status = 0;
		}
	}
	u_xs = (uint32_t)got | ((uint32_t)status << 11);
}

uint32_t usbh_rd(uint32_t off)
{
	usb_poll_engine();
	if (off >= 0x1000) {
		uint32_t o = off - 0x1000;
		return (uint32_t)ubuf[o] | (uint32_t)ubuf[o + 1] << 8 | (uint32_t)ubuf[o + 2] << 16 | (uint32_t)ubuf[o + 3] << 24;
	}
	switch (off) {
	case 0x000: return u_info[0];
	case 0x004: return u_keys[0];
	case 0x020: return u_info[1];
	case 0x024: return u_keys[1];
	case 0x100: return u_ctrl;
	case 0x104: {
		uint32_t ps = 0;
		if (root_dev >= 0) {
			ps |= 0x01;
			if (md[root_dev].lowspeed) ps |= 0x04;
			if (u_enabled) ps |= 0x02;
		}
		return ps;
	}
	case 0x118: return u_xs;
	case 0x11C: return (0x05bu << 20) | (2u << 16) | (4u << 12) | ((uint32_t)usb_roots << 8) | 2u;
	default:
		if (off >= 0x200 && off < 0x220)
			return (off & 4) ? u_pollb[(off - 0x200) / 8] : u_polla[(off - 0x200) / 8];
		return 0;
	}
}

void usbh_wr(uint32_t off, uint32_t v)
{
	if (off >= 0x1000) {
		for (int k = 0; k < 4; k++) ubuf[off - 0x1000 + k] = (uint8_t)(v >> (8 * k));
		return;
	}
	switch (off) {
	case 0x000: u_info[0] = v & 0x03000000u; break;
	case 0x020: u_info[1] = v & 0x03000000u; break;
	case 0x100:
		/* a root port 1 reset: the device is back at address 0, unconfigured */
		if ((v & (1u << 9)) && root_dev >= 0) {
			u_enabled = 1;
			md[root_dev].addr = 0;
			md[root_dev].config = 0;
		}
		u_ctrl = v;
		break;
	case 0x110:
		u_xact_a_saved = v;	/* kept until XACT_B starts the transaction */
		break;
	case 0x114:
		if (v & (1u << 31)) usb_xact(u_xact_a_saved, v);
		break;
	default:
		if (off >= 0x200 && off < 0x220) {
			if (off & 4) u_pollb[(off - 0x200) / 8] = v;
			else u_polla[(off - 0x200) / 8] = v;
		}
		break;
	}
}

uint8_t usbh_rb(uint32_t off) { return off >= 0x1000 ? ubuf[off - 0x1000] : (uint8_t)usbh_rd(off & ~3u); }
void usbh_wb(uint32_t off, uint8_t v) { if (off >= 0x1000) ubuf[off - 0x1000] = v; }

static void usb_model_report(void)
{
	if (n_collisions || n_toggle_errors || n_wrong_speed)
		fprintf(stderr, "[usb model: %d collisions, %d toggle errors, %d with the wrong speed/polarity]\n",
		        n_collisions, n_toggle_errors, n_wrong_speed);
}
