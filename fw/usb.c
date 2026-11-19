/*
 * machboot
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * USB on Zeitlos's USB host controller (rtl/usb/): keyboards, hubs and
 * flash drives (docs/usb.md).
 *
 * A keyboard needs nothing but enumeration: reset its port, read the
 * descriptors, give it an address, choose its configuration, put its
 * boot interface into boot protocol, and hand its interrupt endpoint to
 * the controller's auto-poll. From then on the hardware polls it and
 * keeps the current report in a "compat block", which fw/console.c
 * reads.
 *
 * A hub is a device whose ports are served by control transfers to it
 * (USB 2.0 chapter 11): power them, read each port's status, and when
 * something is plugged in, reset that port through the hub and
 * enumerate what is behind it like any other device. A low-speed
 * device behind a full-speed hub is reached with PRE and normal line
 * polarity -- not the inverted polarity of a direct low-speed attach.
 *
 * A flash drive (mass storage, SCSI, bulk-only) is read a sector at a
 * time for the FAT code, exactly as the SD card is (fw/card.c).
 *
 * This is a much smaller sibling of Zeitlos's sw/os/usb/usbh.c and
 * usbh_hub.c, and follows their rules, each a bug found on hardware
 * there (Zeitlos docs/usb_host.md):
 *
 *   - one packet per transaction in a control data stage, paced here;
 *     a NAK re-issues the same packet, unchanged, after a delay;
 *   - a short packet is success, not an error;
 *   - the status stage's own result is checked;
 *   - a device gets 2 ms after SET_ADDRESS before it is spoken to;
 *   - directly attached low speed is LOWSPEED|INVERTED; behind a hub,
 *     LOWSPEED|PRE;
 *   - SET_PROTOCOL(boot) and SET_IDLE(0) are sent explicitly, and a
 *     STALL on either is not fatal;
 *   - the compat block's type is set BEFORE the poll slot is enabled;
 *   - the transaction engine has one owner at a time;
 *   - only one device is on address 0 at a time, from its port reset
 *     until SET_ADDRESS succeeds; a device behind a hub that fails for
 *     good has its hub port disabled, so it cannot answer on address 0;
 *   - each device has one address, kept across retries.
 *
 * Limits: 6 devices (two on the root ports, four behind hubs), 2 hubs,
 * 2 keyboards (the controller's two compat blocks), 1 flash drive.
 */

#include "usb.h"
#include "hw.h"
#include "lib.h"
#include "console.h"

/* -- the controller's registers (Zeitlos sw/apps/usb/usbh_hw.h, as offsets) -- */

#define U_HID_INFO(b)	((b) ? 0x020u : 0x000u)
#define U_CTRL		0x100u
#define U_PORTSTAT	0x104u
#define U_IRQSTAT	0x108u
#define U_IRQEN		0x10Cu
#define U_XACT_A	0x110u
#define U_XACT_B	0x114u
#define U_XACT_S	0x118u
#define U_CONFIG	0x11Cu
#define U_POLL_A(n)	(0x200u + (n) * 8u)
#define U_POLL_B(n)	(0x204u + (n) * 8u)
#define U_BUF		0x1000u

#define CTRL_FRAME_EN	(1u << 1)
#define CTRL_POLL_EN	(1u << 2)
#define CTRL_PORT_EN(p)	(1u << (8 + (p) * 8))
#define CTRL_PORT_RST(p) (1u << (9 + (p) * 8))

#define PS(r, p)	(((r) >> ((p) * 8)) & 0xffu)
#define PS_CONNECTED	0x01u
#define PS_ENABLED	0x02u
#define PS_LOWSPEED	0x04u

#define PID_SETUP	0
#define PID_IN		1
#define PID_OUT		2

/* XACT_A bits 15:13 and POLL_A bits 13:11: */
#define XA_LOW		0x1		/* low-speed bit rate */
#define XA_INV		0x2		/* inverted polarity: low speed, directly attached */
#define XA_PRE		0x4		/* a PRE first: low speed behind a hub */

#define XS_PENDING	(1u << 16)
#define XS_LEN(s)	((s) & 0x7ffu)
#define XS_STATUS(s)	(((s) >> 11) & 0xfu)
#define ST_OK		0
#define ST_NAK		1
#define ST_STALL	2
#define ST_SHORT	6

#define MODE_BOOT_KBD	3
#define TYP_KBD		1
#define NAK_BUDGET	3		/* hardware retries per transaction */
#define SOFT_NAKS	500		/* software retries of a NAKed packet */
#define CFG_MAX		255
#define POLL_SLOTS	4
#define HUBS_MAX	2
#define HUB_PORTS	7

/* the packet buffer: poll slots' data, the one control scratch, mass storage */
#define POLL_OFF(n)	(0x380u + (unsigned)(n) * 16u)
#define SCR		0x400u		/* SETUP at +0, data at +8 (up to 255) */
#define MSC_CBW		0x580u
#define MSC_CSW		0x5C0u
#define MSC_DATA	0x600u		/* 512 */

/* -- requests -- */

#define DIR_IN		0x80
#define REQ_GET_STATUS	0x00
#define REQ_CLEAR_FEAT	0x01
#define REQ_SET_FEAT	0x03
#define REQ_GET_DESC	0x06
#define REQ_SET_ADDR	0x05
#define REQ_SET_CONFIG	0x09
#define REQ_SET_IDLE	0x0a
#define REQ_SET_PROTO	0x0b
#define HID_OUT_IFACE	0x21
#define HUB_CLASS_IN	0xA0		/* device-to-host, class, device */
#define PORT_IN		0xA3		/* device-to-host, class, other (a port) */
#define PORT_OUT	0x23
#define DESC_DEVICE	1
#define DESC_CONFIG	2
#define DESC_IFACE	4
#define DESC_EP		5
#define DESC_HUB	0x29

/* hub port features (USB 2.0 table 11-17) */
#define F_PORT_ENABLE	1
#define F_PORT_RESET	4
#define F_PORT_POWER	8
#define F_C_CONNECTION	16
/* port status bits (table 11-21, 11-22) */
#define P_CONNECTION	0x0001u
#define P_ENABLE	0x0002u
#define P_LOW_SPEED	0x0200u
#define P_C_CONNECTION	0x0001u
#define P_C_RESET	0x0010u

/* -- devices -- */

enum {
	E_EMPTY, E_DEBOUNCE, E_RESET, E_RESET_WAIT,
	E_HRESET, E_HRESET_POLL, E_HRESET_CLEAR, E_RECOVER,
	E_DESC8, E_SET_ADDR, E_SETTLE, E_DESC18, E_CFG9, E_CFG_ALL, E_SET_CONFIG,
	E_SET_PROTO, E_SET_IDLE,
	E_HUB_DESC, E_HUB_POWER, E_HUB_POWER_WAIT,
	E_RUN, E_IGNORED, E_FAILED, E_RETRY,
};

enum { C_IDLE, C_SETUP, C_DATA, C_STATUS, C_FINAL, C_DONE, C_ERROR };

typedef struct {
	int used;
	int state;
	int parent;		/* -1: on a root port */
	int root;		/* root port */
	int hport;		/* behind a hub: the hub's port, from 1 */
	int depth;
	uint32_t deadline;
	uint8_t addr, mps0, xa;
	uint16_t vid, pid;
	uint8_t dclass;
	uint8_t cfg_val;
	int on_addr0;		/* reset, and not yet at its own address */
	int retries;
	int kind;
	int fail_state, fail_status;

	/* a keyboard */
	int blk, slot;
	int iface, ep, ep_mps, interval;

	/* a hub */
	int hub;		/* index into hubs[], or -1 */

	/* a flash drive */
	uint8_t ep_in, ep_out, tog_in, tog_out;
	uint16_t bulk_mps;

	int what;		/* during enumeration: W_* */
	uint32_t t0;		/* a hub port reset: when it began */
} dev_t;

enum { W_NONE, W_KEYBOARD, W_DRIVE, W_HUB };

typedef struct {
	int dev;		/* the hub's device, or -1 */
	int nports;
	int pgood;		/* ms from power on to power good */
	int scan;		/* the port read next */
	uint32_t next;		/* when */
	int child[HUB_PORTS + 1];	/* device behind each port, or -1 */
	int disable;		/* a port to disable (a child that failed for good), or 0 */
	int step;		/* the port-service sequence (hub_service) */
	uint16_t st, ch;	/* the port's status and changes, as last read */
	uint16_t clr;		/* changes still to acknowledge */
	int full_said;		/* "no room behind this hub" was said */
} hub_t;

static dev_t devs[USB_DEVS];
static hub_t hubs[HUBS_MAX];
static int present;
static int roots;		/* the controller's ports: 2, or 1 (Konfekt) */
static int xowner = -1;		/* who has the transaction engine: a device, XO_DRIVE, or -1 */
#define XO_DRIVE 100
static int blk_used[2];
static int slot_used[POLL_SLOTS];
static uint8_t cfg[CFG_MAX];
static uint16_t cfg_len;

/* the control transfer in progress (one at a time: the engine's owner) */
static int c_tgt;			/* to this device */
static int c_state, c_pend, c_tgl, c_dir_in, c_naks;
static uint16_t c_len, c_done;
static uint32_t c_wait;

/* the flash drive */
static int drive = -1;			/* its device, or -1 */
static int drive_mounted, drive_changed;
static uint32_t drive_sectors, drive_tag;

static uint32_t now(void) { return ms_now(); }

static void xact(dev_t *d, int endp, int pid, int toggle, uint16_t off, uint16_t len, uint8_t mps, int cont)
{
	uint32_t a = (uint32_t)(d->addr & 0x7f) |
		((uint32_t)(endp & 0xf) << 7) |
		((uint32_t)(pid & 3) << 11) |
		((uint32_t)d->xa << 13) |
		((uint32_t)(d->root & 1) << 16) |
		((uint32_t)(mps & 0x7f) << 19);
	if (toggle)
		a |= 1u << 17;
	if (cont)
		a |= 1u << 18;
	usbh_wr(U_XACT_A, a);
	usbh_wr(U_XACT_B, (uint32_t)(off & 0x7ff) | ((uint32_t)(len & 0x7ff) << 11) |
		((uint32_t)NAK_BUDGET << 22) | (1u << 31));
}

/* -- control transfers ------------------------------------------------- */

static void ctrl_begin(int tgt, uint8_t bm, uint8_t req, uint16_t val, uint16_t idx, uint16_t len)
{
	uint32_t s = U_BUF + SCR;
	usbh_wb(s + 0, bm);
	usbh_wb(s + 1, req);
	usbh_wb(s + 2, (uint8_t)val);
	usbh_wb(s + 3, (uint8_t)(val >> 8));
	usbh_wb(s + 4, (uint8_t)idx);
	usbh_wb(s + 5, (uint8_t)(idx >> 8));
	usbh_wb(s + 6, (uint8_t)len);
	usbh_wb(s + 7, (uint8_t)(len >> 8));
	c_tgt = tgt;
	c_state = C_SETUP;
	c_pend = 0;
	c_len = len;
	c_done = 0;
	c_dir_in = (bm & DIR_IN) != 0;
	c_naks = 0;
	c_wait = now();
}

/* one step of the control transfer; returns C_DONE, C_ERROR or "still going" */
static int ctrl_step(void)
{
	dev_t *d = &devs[c_tgt];
	uint32_t s;
	int st;

	if (c_state == C_DONE || c_state == C_ERROR)
		return c_state;
	if (usbh_rd(U_XACT_S) & XS_PENDING)
		return c_state;
	if ((int32_t)(now() - c_wait) < 0)
		return c_state;

	s = usbh_rd(U_XACT_S);
	st = (int)XS_STATUS(s);

	switch (c_state) {
	case C_SETUP:
		/* SETUP is always DATA0; its result is judged by the next state */
		xact(d, 0, PID_SETUP, 0, SCR, 8, d->mps0, 0);
		c_tgl = 1;
		c_done = 0;
		c_pend = -1;		/* -1: the outstanding packet is the SETUP */
		c_state = c_len ? C_DATA : C_STATUS;
		break;

	case C_DATA:
		if (c_pend == -1) {
			if (st != ST_OK) { c_state = C_ERROR; break; }
			c_pend = 0;
		} else if (c_pend == 1) {
			c_pend = 0;
			if (st == ST_NAK) {
				if (++c_naks > SOFT_NAKS) { c_state = C_ERROR; break; }
				c_wait = now() + 1;
				break;		/* re-issued, unchanged, next time */
			}
			if (st != ST_OK && st != ST_SHORT) { c_state = C_ERROR; break; }
			uint16_t got = (uint16_t)XS_LEN(s);
			c_done += got;
			c_tgl ^= 1;
			if (got < d->mps0 || c_done >= c_len) {
				/* data over: status stage, opposite direction, DATA1 */
				xact(d, 0, c_dir_in ? PID_OUT : PID_IN, 1, SCR + 8, 0, d->mps0, 0);
				c_pend = 1;
				c_state = C_FINAL;
				break;
			}
		}
		{
			uint16_t want = c_len - c_done;
			if (want > d->mps0)
				want = d->mps0;
			xact(d, 0, c_dir_in ? PID_IN : PID_OUT, c_tgl, (uint16_t)(SCR + 8 + c_done), want, d->mps0, 0);
			c_pend = 1;
		}
		break;

	case C_STATUS:
		/* no data stage: the SETUP's result, then the status IN */
		if (c_pend == -1 && st != ST_OK) { c_state = C_ERROR; break; }
		xact(d, 0, PID_IN, 1, SCR + 8, 0, d->mps0, 0);
		c_pend = 1;
		c_state = C_FINAL;
		break;

	case C_FINAL:
		if (!c_pend) {
			xact(d, 0, c_dir_in ? PID_OUT : PID_IN, 1, SCR + 8, 0, d->mps0, 0);
			c_pend = 1;
			break;
		}
		c_pend = 0;
		if (st == ST_NAK && ++c_naks <= SOFT_NAKS) {
			c_wait = now() + 1;
			break;
		}
		c_state = (st == ST_OK || st == ST_SHORT) ? C_DONE : C_ERROR;
		break;
	}
	return c_state;
}

/* a whole control transfer, waiting (mass storage's recovery only) */
static int ctrl_sync(int tgt, uint8_t bm, uint8_t req, uint16_t val, uint16_t idx, uint16_t len)
{
	uint32_t t0 = now();
	ctrl_begin(tgt, bm, req, val, idx, len);
	for (;;) {
		int r = ctrl_step();
		if (r == C_DONE || r == C_ERROR)
			return r == C_DONE ? 0 : -1;
		if (now() - t0 > 1000)
			return -1;
	}
}

static uint8_t data_byte(int i)
{
	return usbh_rb(U_BUF + SCR + 8 + (uint32_t)i);
}

/* -- descriptors ----------------------------------------------------------- */

/* a boot keyboard: interface number and its interrupt IN endpoint */
static int find_keyboard(dev_t *d)
{
	int i = 0, in_kbd = 0;
	d->iface = -1;
	while (i + 1 < cfg_len) {
		int len = cfg[i], type = cfg[i + 1];
		if (len < 2)
			break;
		if (type == DESC_IFACE && i + 8 < cfg_len) {
			in_kbd = cfg[i + 5] == 3 && cfg[i + 6] == 1 && cfg[i + 7] == 1;
			if (in_kbd && d->iface < 0)
				d->iface = cfg[i + 2];
			else if (d->iface >= 0)
				in_kbd = 0;
		}
		if (type == DESC_EP && in_kbd && i + 6 < cfg_len &&
		    (cfg[i + 2] & 0x80) && (cfg[i + 3] & 3) == 3) {
			d->ep = cfg[i + 2] & 0x0f;
			d->ep_mps = cfg[i + 4] > 8 ? 8 : cfg[i + 4];
			d->interval = cfg[i + 6] ? cfg[i + 6] : 10;
			return 1;
		}
		i += len;
	}
	return 0;
}

/* mass storage, SCSI, bulk-only: its two bulk endpoints */
static int find_drive(dev_t *d)
{
	int i = 0, in_msc = 0;
	d->ep_in = d->ep_out = 0;
	while (i + 1 < cfg_len) {
		int len = cfg[i], type = cfg[i + 1];
		if (len < 2)
			break;
		if (type == DESC_IFACE && i + 8 < cfg_len)
			in_msc = cfg[i + 5] == 8 && cfg[i + 6] == 6 && cfg[i + 7] == 0x50;
		if (type == DESC_EP && in_msc && i + 6 < cfg_len && (cfg[i + 3] & 3) == 2) {
			if (cfg[i + 2] & 0x80)
				d->ep_in = cfg[i + 2] & 0x0f;
			else
				d->ep_out = cfg[i + 2] & 0x0f;
			d->bulk_mps = cfg[i + 4] ? cfg[i + 4] : 64;
		}
		i += len;
	}
	return d->ep_in && d->ep_out;
}

/* -- the device table ------------------------------------------------------ */

static int dev_new(int parent, int hport)
{
	for (int i = USB_PORTS; i < USB_DEVS; i++)
		if (!devs[i].used) {
			dev_t *d = &devs[i];
			memset(d, 0, sizeof(*d));
			d->used = 1;
			d->parent = parent;
			d->root = devs[parent].root;
			d->depth = devs[parent].depth + 1;
			d->hport = hport;
			d->blk = d->slot = d->hub = -1;
			d->kind = USB_BUSY;
			d->state = E_DEBOUNCE;
			d->deadline = now() + 100;
			return i;
		}
	return -1;
}

static void teardown(int i)
{
	dev_t *d = &devs[i];
	if (!d->used)
		return;
	/* what is behind it first */
	if (d->hub >= 0) {
		hub_t *h = &hubs[d->hub];
		for (int p = 1; p <= HUB_PORTS; p++)
			if (h->child[p] >= 0)
				teardown(h->child[p]);
		h->dev = -1;
	}
	if (d->parent >= 0 && devs[d->parent].hub >= 0)
		hubs[devs[d->parent].hub].child[d->hport] = -1;
	if (d->slot >= 0) {
		usbh_wr(U_POLL_A(d->slot), 0);
		slot_used[d->slot] = 0;
	}
	if (d->blk >= 0) {
		usbh_wr(U_HID_INFO(d->blk), 0);
		blk_used[d->blk] = 0;
	}
	if (drive == i) {
		drive = -1;
		drive_mounted = 0;
		drive_changed = 1;
		con_puts("usb: the flash drive is gone\n");
	}
	if (xowner == i)
		xowner = -1;
	if (i < USB_PORTS) {
		usbh_wr(U_CTRL, usbh_rd(U_CTRL) & ~CTRL_PORT_RST(i));
		memset(d, 0, sizeof(*d));
		d->used = 1;
		d->parent = -1;
		d->root = i;
		d->blk = d->slot = d->hub = -1;
		d->state = E_EMPTY;
		d->kind = USB_NONE;
	} else {
		memset(d, 0, sizeof(*d));
	}
}

/* "1" on root port 1, "1.3" behind a hub's port 3 */
static const char *loc(const dev_t *d)
{
	static char s[8];
	if (d->hport)
		mb_snprintf(s, sizeof(s), "%d.%d", d->root + 1, d->hport);
	else
		mb_snprintf(s, sizeof(s), "%d", d->root + 1);
	return s;
}

static void release(int i)
{
	if (xowner == i)
		xowner = -1;
}

static void fail(int i)
{
	dev_t *d = &devs[i];
	d->fail_state = d->state;
	d->fail_status = (int)XS_STATUS(usbh_rd(U_XACT_S));
	release(i);
	if (d->retries < 3) {
		d->retries++;
		d->state = E_RETRY;
		d->deadline = now() + 200;
		d->kind = USB_BUSY;
		return;
	}
	d->state = E_FAILED;
	d->kind = USB_FAILED;
	con_printf("usb %s: the device did not answer (step %d, status %d)\n",
	           loc(d), d->fail_state, d->fail_status);
	/* behind a hub, a device that never left address 0 must be silenced */
	if (d->parent >= 0 && d->on_addr0 && devs[d->parent].hub >= 0)
		hubs[devs[d->parent].hub].disable = d->hport;
	d->on_addr0 = 0;
}

static int addr0_busy(int except)
{
	for (int i = 0; i < USB_DEVS; i++)
		if (i != except && devs[i].used && devs[i].on_addr0)
			return 1;
	return 0;
}

static void bind_keyboard(int i)
{
	dev_t *d = &devs[i];
	int b = !blk_used[0] ? 0 : !blk_used[1] ? 1 : -1;
	int s = -1;
	for (int k = 0; k < POLL_SLOTS && s < 0; k++)
		if (!slot_used[k])
			s = k;
	release(i);
	if (b < 0 || s < 0) {
		d->kind = USB_NOROOM;
		d->state = E_IGNORED;
		con_printf("usb %s: a third keyboard: only two are used\n", loc(d));
		return;
	}
	blk_used[b] = 1;
	slot_used[s] = 1;
	d->blk = b;
	d->slot = s;
	usbh_wr(U_HID_INFO(b), (uint32_t)TYP_KBD << 24);
	usbh_wr(U_POLL_B(s), POLL_OFF(s) | ((uint32_t)MODE_BOOT_KBD << 11) | ((uint32_t)b << 14));
	usbh_wr(U_POLL_A(s), (uint32_t)(d->addr & 0x7f) | ((uint32_t)(d->ep & 0xf) << 7) |
		((uint32_t)d->xa << 11) | ((uint32_t)(d->root & 1) << 14) |
		((uint32_t)(d->ep_mps & 0x7f) << 15) | ((uint32_t)(d->interval & 0xff) << 22) |
		(1u << 30));
	d->kind = USB_KEYBOARD;
	d->state = E_RUN;
	con_printf("usb %s: keyboard %04x:%04x%s\n", loc(d), d->vid, d->pid,
	           d->xa & XA_PRE ? " (low speed, behind a hub)" : d->xa ? " (low speed)" : "");
}

static void bind_drive(int i)
{
	dev_t *d = &devs[i];
	release(i);
	if (drive >= 0) {
		d->kind = USB_NOROOM;
		d->state = E_IGNORED;
		con_printf("usb %s: a second flash drive: only one is used\n", loc(d));
		return;
	}
	drive = i;
	drive_mounted = 0;
	drive_changed = 1;
	d->tog_in = d->tog_out = 0;
	d->kind = USB_DRIVE;
	d->state = E_RUN;
	con_printf("usb %s: flash drive %04x:%04x\n", loc(d), d->vid, d->pid);
}


/* -- enumeration: one device's state machine -------------------------------- */

static int hub_alloc(int i)
{
	for (int h = 0; h < HUBS_MAX; h++)
		if (hubs[h].dev < 0) {
			memset(&hubs[h], 0, sizeof(hubs[h]));
			hubs[h].dev = i;
			for (int p = 0; p <= HUB_PORTS; p++)
				hubs[h].child[p] = -1;
			return h;
		}
	return -1;
}

static void dev_step(int i, uint32_t ps)
{
	dev_t *d = &devs[i];
	hub_t *h = d->hub >= 0 ? &hubs[d->hub] : 0;
	int r;

	switch (d->state) {
	case E_EMPTY:
		d->kind = USB_BUSY;
		d->deadline = now() + 100;
		d->state = E_DEBOUNCE;
		break;

	case E_RETRY:
	case E_DEBOUNCE:
		if ((int32_t)(now() - d->deadline) < 0 || xowner >= 0 || addr0_busy(i))
			break;
		xowner = i;
		d->state = d->parent < 0 ? E_RESET : E_HRESET;
		break;

	/* -- a root port: the controller resets it -- */
	case E_RESET:
		usbh_wr(U_CTRL, usbh_rd(U_CTRL) | CTRL_PORT_RST(i));
		d->deadline = now() + 100;
		d->state = E_RESET_WAIT;
		break;

	case E_RESET_WAIT:
		if (ps & PS_ENABLED) {
			usbh_wr(U_CTRL, usbh_rd(U_CTRL) & ~CTRL_PORT_RST(i));
			d->xa = (ps & PS_LOWSPEED) ? (XA_LOW | XA_INV) : 0;
			d->addr = 0;
			d->mps0 = 8;
			d->on_addr0 = 1;
			ctrl_begin(i, DIR_IN, REQ_GET_DESC, DESC_DEVICE << 8, 0, 8);
			d->state = E_DESC8;
		} else if ((int32_t)(now() - d->deadline) > 0) {
			fail(i);
		}
		break;

	/* -- behind a hub: the hub resets its port, and says at what speed -- */
	case E_HRESET:
		d->on_addr0 = 1;
		d->t0 = now();
		ctrl_begin(d->parent, PORT_OUT, REQ_SET_FEAT, F_PORT_RESET, (uint16_t)d->hport, 0);
		d->state = E_HRESET_POLL;
		d->deadline = 0;
		break;

	case E_HRESET_POLL:
		r = ctrl_step();
		if (r == C_ERROR) { fail(i); break; }
		if (r != C_DONE) break;
		if (d->deadline && c_len == 4) {
			uint16_t st = (uint16_t)(data_byte(0) | data_byte(1) << 8);
			uint16_t ch = (uint16_t)(data_byte(2) | data_byte(3) << 8);
			if (ch & P_C_RESET) {
				if (!(st & P_ENABLE)) { fail(i); break; }
				d->xa = (st & P_LOW_SPEED) ? (XA_LOW | XA_PRE) : 0;
				ctrl_begin(d->parent, PORT_OUT, REQ_CLEAR_FEAT, 20 /* C_PORT_RESET */, (uint16_t)d->hport, 0);
				d->state = E_HRESET_CLEAR;
				break;
			}
			if (now() - d->t0 > 500) { fail(i); break; }
		}
		/* (again) in a few ms: the reset takes the hub 10-20 ms */
		d->deadline = now() + 5;
		ctrl_begin(d->parent, PORT_IN, REQ_GET_STATUS, 0, (uint16_t)d->hport, 4);
		c_wait = d->deadline;
		break;

	case E_HRESET_CLEAR:
		r = ctrl_step();
		if (r == C_ERROR) { fail(i); break; }
		if (r != C_DONE) break;
		d->deadline = now() + 10;	/* reset recovery */
		d->state = E_RECOVER;
		break;

	case E_RECOVER:
		if ((int32_t)(now() - d->deadline) < 0)
			break;
		d->addr = 0;
		d->mps0 = 8;
		ctrl_begin(i, DIR_IN, REQ_GET_DESC, DESC_DEVICE << 8, 0, 8);
		d->state = E_DESC8;
		break;

	/* -- every device -- */
	case E_DESC8:
		r = ctrl_step();
		if (r == C_ERROR) { fail(i); break; }
		if (r != C_DONE) break;
		d->mps0 = data_byte(7);
		d->dclass = data_byte(4);
		if (d->mps0 < 8 || d->mps0 > 64)
			d->mps0 = 8;
		ctrl_begin(i, 0, REQ_SET_ADDR, (uint16_t)(i + 1), 0, 0);
		d->state = E_SET_ADDR;
		break;

	case E_SET_ADDR:
		r = ctrl_step();
		if (r == C_ERROR) { fail(i); break; }
		if (r != C_DONE) break;
		d->addr = (uint8_t)(i + 1);	/* one address per device slot, kept across retries */
		d->on_addr0 = 0;
		d->deadline = now() + 3;
		d->state = E_SETTLE;
		break;

	case E_SETTLE:
		if ((int32_t)(now() - d->deadline) < 0)
			break;
		ctrl_begin(i, DIR_IN, REQ_GET_DESC, DESC_DEVICE << 8, 0, 18);
		d->state = E_DESC18;
		break;

	case E_DESC18:
		r = ctrl_step();
		if (r == C_ERROR) { fail(i); break; }
		if (r != C_DONE) break;
		d->vid = (uint16_t)(data_byte(8) | data_byte(9) << 8);
		d->pid = (uint16_t)(data_byte(10) | data_byte(11) << 8);
		ctrl_begin(i, DIR_IN, REQ_GET_DESC, DESC_CONFIG << 8, 0, 9);
		d->state = E_CFG9;
		break;

	case E_CFG9:
		r = ctrl_step();
		if (r == C_ERROR) { fail(i); break; }
		if (r != C_DONE) break;
		cfg_len = (uint16_t)(data_byte(2) | data_byte(3) << 8);
		if (cfg_len > CFG_MAX)
			cfg_len = CFG_MAX;
		ctrl_begin(i, DIR_IN, REQ_GET_DESC, DESC_CONFIG << 8, 0, cfg_len);
		d->state = E_CFG_ALL;
		break;

	case E_CFG_ALL:
		r = ctrl_step();
		if (r == C_ERROR) { fail(i); break; }
		if (r != C_DONE) break;
		if (c_done < cfg_len)
			cfg_len = c_done;
		for (int k = 0; k < cfg_len; k++)
			cfg[k] = data_byte(k);
		d->cfg_val = cfg[5];
		if (d->dclass == 9) {
			if (d->depth >= 2 || (d->hub = hub_alloc(i)) < 0) {
				d->kind = USB_NOROOM;
				d->state = E_IGNORED;
				release(i);
				con_printf("usb %s: a hub too many (two, one deep, are served)\n", loc(d));
				break;
			}
			d->what = W_HUB;
		} else if (find_keyboard(d)) {
			d->what = W_KEYBOARD;
		} else if (find_drive(d)) {
			d->what = W_DRIVE;
		} else {
			d->kind = USB_OTHER;
			d->state = E_IGNORED;
			release(i);
			con_printf("usb %s: %04x:%04x is not a keyboard, hub or flash drive\n",
			           loc(d), d->vid, d->pid);
			break;
		}
		ctrl_begin(i, 0, REQ_SET_CONFIG, d->cfg_val, 0, 0);
		d->state = E_SET_CONFIG;
		break;

	case E_SET_CONFIG:
		r = ctrl_step();
		if (r == C_ERROR) { fail(i); break; }
		if (r != C_DONE) break;
		if (d->what == W_HUB) {
			ctrl_begin(i, HUB_CLASS_IN, REQ_GET_DESC, DESC_HUB << 8, 0, 9);
			d->state = E_HUB_DESC;
		} else if (d->what == W_DRIVE) {
			bind_drive(i);
		} else {
			ctrl_begin(i, HID_OUT_IFACE, REQ_SET_PROTO, 0, (uint16_t)d->iface, 0);
			d->state = E_SET_PROTO;
		}
		break;

	case E_SET_PROTO:
		/* a STALL is not fatal: plenty of keyboards are in boot protocol anyway */
		r = ctrl_step();
		if (r != C_DONE && r != C_ERROR) break;
		ctrl_begin(i, HID_OUT_IFACE, REQ_SET_IDLE, 0, (uint16_t)d->iface, 0);
		d->state = E_SET_IDLE;
		break;

	case E_SET_IDLE:
		r = ctrl_step();
		if (r != C_DONE && r != C_ERROR) break;
		bind_keyboard(i);
		break;

	/* -- a hub: its descriptor, then power to every port -- */
	case E_HUB_DESC:
		r = ctrl_step();
		if (r == C_ERROR) { fail(i); break; }
		if (r != C_DONE) break;
		h->nports = data_byte(2);
		if (h->nports > HUB_PORTS)
			h->nports = HUB_PORTS;
		h->pgood = data_byte(5) * 2;
		h->scan = 1;
		if (!h->nports) { fail(i); break; }
		ctrl_begin(i, PORT_OUT, REQ_SET_FEAT, F_PORT_POWER, 1, 0);
		d->state = E_HUB_POWER;
		break;

	case E_HUB_POWER:
		r = ctrl_step();
		if (r == C_ERROR) { fail(i); break; }
		if (r != C_DONE) break;
		if (++h->scan <= h->nports) {
			ctrl_begin(i, PORT_OUT, REQ_SET_FEAT, F_PORT_POWER, (uint16_t)h->scan, 0);
			break;
		}
		d->deadline = now() + (uint32_t)h->pgood + 100;
		d->state = E_HUB_POWER_WAIT;
		break;

	case E_HUB_POWER_WAIT:
		if ((int32_t)(now() - d->deadline) < 0)
			break;
		h->scan = 1;
		h->next = now();
		h->step = 0;
		d->kind = USB_HUB;
		d->state = E_RUN;
		release(i);
		con_printf("usb %s: hub %04x:%04x, %d ports\n", loc(d), d->vid, d->pid, h->nports);
		break;

	default:
		break;
	}
}

/* -- a hub's ports: read one at a time, act on changes ------------------------- */

static void hub_act(int i)
{
	dev_t *d = &devs[i];
	hub_t *h = &hubs[d->hub];
	int p = h->scan, c = h->child[p];

	if (c >= 0 && (!(h->st & P_CONNECTION) || (h->ch & P_C_CONNECTION))) {
		teardown(c);			/* unplugged, or replugged */
		c = -1;
	}
	if (c < 0 && (h->st & P_CONNECTION)) {
		c = dev_new(i, p);
		if (c >= 0) {
			h->child[p] = c;
		} else if (!h->full_said) {
			h->full_said = 1;
			con_puts("usb: no room for another device behind the hub\n");
		}
	}
	h->scan = h->scan % h->nports + 1;
	h->next = now() + (h->scan == 1 ? 100u : 2u);
}

static void hub_next_clear(int i)
{
	hub_t *h = &hubs[devs[i].hub];
	for (int b = 0; b < 5; b++)
		if (h->clr & (1u << b)) {
			h->clr &= (uint16_t)~(1u << b);
			ctrl_begin(i, PORT_OUT, REQ_CLEAR_FEAT, (uint16_t)(F_C_CONNECTION + b), (uint16_t)h->scan, 0);
			h->step = 2;
			return;
		}
	hub_act(i);
	release(i);
	h->step = 0;
}

static void hub_service(int i)
{
	hub_t *h = &hubs[devs[i].hub];
	int r;

	switch (h->step) {
	case 0:
		if (xowner >= 0)
			return;
		if (h->disable) {
			xowner = i;
			ctrl_begin(i, PORT_OUT, REQ_CLEAR_FEAT, F_PORT_ENABLE, (uint16_t)h->disable, 0);
			h->step = 3;
			return;
		}
		if ((int32_t)(now() - h->next) < 0)
			return;
		xowner = i;
		ctrl_begin(i, PORT_IN, REQ_GET_STATUS, 0, (uint16_t)h->scan, 4);
		h->step = 1;
		return;

	case 1:		/* the port's status and changes */
		r = ctrl_step();
		if (r != C_DONE && r != C_ERROR)
			return;
		if (r == C_ERROR) {
			release(i);
			h->step = 0;
			h->next = now() + 100;
			return;
		}
		h->st = (uint16_t)(data_byte(0) | data_byte(1) << 8);
		h->ch = (uint16_t)(data_byte(2) | data_byte(3) << 8);
		h->clr = h->ch & 0x1f;
		hub_next_clear(i);	/* acknowledge each change, then act */
		return;

	case 2:
		r = ctrl_step();
		if (r != C_DONE && r != C_ERROR)
			return;
		hub_next_clear(i);
		return;

	case 3:		/* a failed child's port disabled */
		r = ctrl_step();
		if (r != C_DONE && r != C_ERROR)
			return;
		h->disable = 0;
		release(i);
		h->step = 0;
		return;
	}
}

/* -- the flash drive: bulk-only transport, SCSI (synchronous) ------------------ */

static int xwait(void)
{
	uint32_t t0 = now();
	while (usbh_rd(U_XACT_S) & XS_PENDING)
		if (now() - t0 > 2000)
			return -1;
	return (int)XS_STATUS(usbh_rd(U_XACT_S));
}

/* one bulk transfer (the controller splits it into packets); the data
 * toggle is kept here: it flips once per packet */
static int bulk(dev_t *d, int in, uint16_t off, uint16_t len, uint16_t *got)
{
	uint8_t mps = (uint8_t)(d->bulk_mps > 64 ? 64 : d->bulk_mps);
	for (int naks = 0; naks < 2000; naks++) {
		xact(d, in ? d->ep_in : d->ep_out, in ? PID_IN : PID_OUT, in ? d->tog_in : d->tog_out, off, len, mps, 1);
		int st = xwait();
		if (st == ST_NAK) {
			uint32_t t = now();
			while (now() == t)
				;
			continue;
		}
		if (st != ST_OK && st != ST_SHORT)
			return st < 0 ? 3 : st;
		uint16_t n = (uint16_t)XS_LEN(usbh_rd(U_XACT_S));
		if (got)
			*got = n;
		uint32_t packets;
		if (!in)
			packets = len ? (len + mps - 1u) / mps : 1u;
		else
			packets = (n == len && n && n % mps == 0) ? n / mps : n / mps + 1u;
		if (packets & 1) {
			if (in) d->tog_in ^= 1; else d->tog_out ^= 1;
		}
		return ST_OK;
	}
	return ST_NAK;
}

static void clear_halt(int i, int in)
{
	dev_t *d = &devs[i];
	ctrl_sync(i, 0x02, REQ_CLEAR_FEAT, 0, (uint16_t)(in ? (0x80 | d->ep_in) : d->ep_out), 0);
	if (in) d->tog_in = 0; else d->tog_out = 0;
}

static void reset_recovery(int i)
{
	dev_t *d = &devs[i];
	ctrl_sync(i, 0x21, 0xFF, 0, (uint16_t)d->iface, 0);	/* bulk-only mass storage reset */
	clear_halt(i, 1);
	clear_halt(i, 0);
}

/* a SCSI command; data to or from MSC_DATA; returns the CSW status, or -1 */
static int bot(const uint8_t *cb, int cblen, int in, uint16_t len)
{
	int i = drive;
	dev_t *d = &devs[i];
	uint32_t a = U_BUF + MSC_CBW;
	static const uint8_t sig[4] = { 'U', 'S', 'B', 'C' };
	drive_tag++;
	for (int k = 0; k < 31; k++) {
		uint8_t v = 0;
		if (k < 4) v = sig[k];
		else if (k < 8) v = (uint8_t)(drive_tag >> (8 * (k - 4)));
		else if (k < 12) v = (uint8_t)(len >> (8 * (k - 8)));
		else if (k == 12) v = in ? 0x80 : 0;
		else if (k == 14) v = (uint8_t)cblen;
		else if (k >= 15 && k - 15 < cblen) v = cb[k - 15];
		usbh_wb(a + (uint32_t)k, v);
	}
	if (bulk(d, 0, MSC_CBW, 31, 0) != ST_OK) {
		reset_recovery(i);
		return -1;
	}
	if (len) {
		uint16_t got;
		int st = bulk(d, in, MSC_DATA, len, &got);
		if (st == ST_STALL)
			clear_halt(i, in);
		else if (st != ST_OK) {
			reset_recovery(i);
			return -1;
		}
	}
	uint16_t n = 0;
	int st = bulk(d, 1, MSC_CSW, 13, &n);
	if (st == ST_STALL) {
		clear_halt(i, 1);
		st = bulk(d, 1, MSC_CSW, 13, &n);
	}
	a = U_BUF + MSC_CSW;
	if (st != ST_OK || n != 13 || usbh_rb(a) != 'U' || usbh_rb(a + 1) != 'S' || usbh_rb(a + 2) != 'B' ||
	    usbh_rb(a + 3) != 'S' || usbh_rb(a + 4) != (uint8_t)drive_tag) {
		reset_recovery(i);
		return -1;
	}
	return usbh_rb(a + 12);
}

static int drive_take(void)
{
	uint32_t t0 = now();
	while (xowner >= 0 && xowner != XO_DRIVE) {
		usb_step();
		if (now() - t0 > 3000 || drive < 0)
			return -1;
	}
	if (drive < 0)
		return -1;
	xowner = XO_DRIVE;
	return 0;
}

static void drive_give(void)
{
	if (xowner == XO_DRIVE)
		xowner = -1;
}

int usb_drive_ready(void)
{
	if (drive < 0)
		return 0;
	if (drive_mounted)
		return 1;
	if (drive_take())
		return 0;
	static const uint8_t tur[6] = { 0x00 }, sense[6] = { 0x03, 0, 0, 0, 18, 0 };
	static const uint8_t cap[10] = { 0x25 };
	int ok = 0;
	/* a drive may take a while after it is configured: ask for up to 3 s */
	for (int k = 0; k < 30 && !ok; k++) {
		int s = bot(tur, 6, 0, 0);
		if (s == 0) {
			ok = 1;
			break;
		}
		if (s > 0)
			bot(sense, 6, 1, 18);		/* clears a UNIT ATTENTION */
		uint32_t t = now();
		while (now() - t < 100)
			;
	}
	if (ok && bot(cap, 10, 1, 8) == 0) {
		uint32_t a = U_BUF + MSC_DATA;
		uint32_t last = (uint32_t)usbh_rb(a) << 24 | (uint32_t)usbh_rb(a + 1) << 16 |
			(uint32_t)usbh_rb(a + 2) << 8 | usbh_rb(a + 3);
		uint32_t bs = (uint32_t)usbh_rb(a + 4) << 24 | (uint32_t)usbh_rb(a + 5) << 16 |
			(uint32_t)usbh_rb(a + 6) << 8 | usbh_rb(a + 7);
		if (bs == 512) {
			drive_sectors = last + 1;
			drive_mounted = 1;
			con_printf("usb: flash drive: %u MB\n", drive_sectors / 2048);
		} else {
			con_printf("usb: flash drive: %u-byte sectors are not read\n", bs);
		}
	}
	drive_give();
	return drive_mounted;
}

uint32_t usb_drive_sectors(void)
{
	return drive_mounted ? drive_sectors : 0;
}

int usb_drive_changed(void)
{
	int c = drive_changed;
	drive_changed = 0;
	return c;
}

int usb_drive_read(uint32_t lba, uint8_t *buf)
{
	if (!drive_mounted || drive_take())
		return -1;
	uint8_t cb[10] = { 0x28, 0, (uint8_t)(lba >> 24), (uint8_t)(lba >> 16), (uint8_t)(lba >> 8), (uint8_t)lba,
	                   0, 0, 1, 0 };
	int r = -1;
	for (int tries = 0; tries < 2 && r; tries++)
		if (bot(cb, 10, 1, 512) == 0) {
			for (int k = 0; k < 512; k++)
				buf[k] = usbh_rb(U_BUF + MSC_DATA + (uint32_t)k);
			r = 0;
		}
	drive_give();
	return r;
}

/* -- the whole thing ------------------------------------------------------------ */

int usb_init(void)
{
	uint32_t c = usbh_rd(U_CONFIG);
	present = ((c >> 20) & 0xfff) == 0x05b;
	if (!present)
		return 0;
	roots = (int)((c >> 8) & 0xf);
	if (roots < 1 || roots > USB_PORTS)
		roots = USB_PORTS;
	memset(devs, 0, sizeof(devs));
	for (int h = 0; h < HUBS_MAX; h++)
		hubs[h].dev = -1;
	for (int p = 0; p < USB_PORTS; p++) {
		devs[p].used = 1;
		devs[p].parent = -1;
		devs[p].root = p;
		devs[p].blk = devs[p].slot = devs[p].hub = -1;
	}
	for (int s = 0; s < POLL_SLOTS; s++)
		usbh_wr(U_POLL_A(s), 0);
	xowner = -1;
	drive = -1;
	usbh_wr(U_IRQEN, 0);
	usbh_wr(U_IRQSTAT, 0x3f);
	usbh_wr(U_CTRL, CTRL_FRAME_EN | CTRL_POLL_EN | CTRL_PORT_EN(0) | (roots > 1 ? CTRL_PORT_EN(1) : 0));
	return 1;
}

void usb_step(void)
{
	if (!present)
		return;
	uint32_t psr = usbh_rd(U_PORTSTAT);
	for (int p = 0; p < roots; p++) {
		uint32_t ps = PS(psr, p);
		if (!(ps & PS_CONNECTED)) {
			if (devs[p].state != E_EMPTY || devs[p].kind != USB_NONE)
				teardown(p);
			continue;
		}
		dev_step(p, ps);
	}
	for (int i = USB_PORTS; i < USB_DEVS; i++)
		if (devs[i].used)
			dev_step(i, 0);
	for (int i = 0; i < USB_DEVS; i++)
		if (devs[i].used && devs[i].kind == USB_HUB && devs[i].state == E_RUN)
			hub_service(i);
}

/* tree order: a root port, then what is behind it, depth first */
static int order(int *out)
{
	int n = 0;
	int stack[USB_DEVS], sp = 0;
	for (int p = roots - 1; p >= 0; p--)
		stack[sp++] = p;
	while (sp) {
		int i = stack[--sp];
		out[n++] = i;
		if (devs[i].hub >= 0) {
			hub_t *h = &hubs[devs[i].hub];
			for (int q = HUB_PORTS; q >= 1; q--)
				if (h->child[q] >= 0 && sp < USB_DEVS)
					stack[sp++] = h->child[q];
		}
	}
	return n;
}

int usb_dev_info(int n, usb_dev_info_t *di)
{
	int o[USB_DEVS];
	memset(di, 0, sizeof(*di));
	if (!present)
		return 0;
	int cnt = order(o);
	if (n < 0 || n >= cnt)
		return 0;
	dev_t *d = &devs[o[n]];
	di->kind = d->kind;
	di->vid = d->vid;
	di->pid = d->pid;
	di->lowspeed = (d->xa & XA_LOW) != 0;
	di->root = d->root;
	di->depth = d->depth;
	di->hub_port = d->hport;
	di->fail_state = d->fail_state;
	di->fail_status = d->fail_status;
	di->sectors = (o[n] == drive) ? usb_drive_sectors() : 0;
	return 1;
}

int usb_keyboard_block(int blk)
{
	if (!present || blk < 0 || blk > 1 || !blk_used[blk])
		return -1;
	return blk;
}
