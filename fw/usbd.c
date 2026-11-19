/*
 * machboot
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The USB device (docs/usb-device.md), written from the USB 2.0
 * specification (chapter 9), the CDC 1.2 / PSTN 1.2 class specifications
 * and the DFU 1.1 specification. The hardware (rtl/mb_usbd.v) gives
 * packet buffers; everything else is here.
 *
 * One configuration, three interfaces:
 *
 *   0  CDC communication (ACM)   endpoint 2 IN, interrupt (never sent)
 *   1  CDC data                  endpoint 1 OUT and IN, bulk, 64 bytes
 *   2  DFU, in DFU mode          alt 0: the system slot, and the
 *                                Zeitlos region (tinydfu's layout)
 *   3  mass storage (SCSI,       endpoint 3 OUT and IN, bulk: the SD card,
 *      bulk-only)                when the Card reader page shares it
 *
 * The console: what the serial port prints goes to a terminal on the
 * USB port too, once it has the port open (DTR), and what the terminal
 * types are keys, exactly as on the serial port.
 *
 * DFU, alt 0, takes what tinydfu's alt 0 took, so `dfu-util -a 0 -D`
 * with Zeitlos's `-dfu.bin` release file works as before. tinydfu
 * wrote it at 0x040000; machboot maps it:
 *
 *   file 0x000000-0x09FFFF  ->  0x060000-0x0FFFFF   the system slot
 *   file 0x0A0000-0x0BFFFF      must be empty (0xFF): the slot is 640 KB
 *   file 0x0C0000-...       ->  0x100000-...        the Zeitlos region
 *
 * A plain bitstream (`dfu-util -a 0 -D zeitlos.bit`) is just the first
 * part. As for an install from the card (fw/install.c): the slot's first
 * sector is erased before anything else is written and written last,
 * only sectors that differ are written, and each is read back.
 * wTransferSize is 4096, so each DFU block is one flash sector.
 */

#include "usbd.h"
#include "hw.h"
#include "lib.h"
#include "mb.h"
#include "flash.h"
#include "install.h"
#include "settings.h"
#include "console.h"
#include "sd.h"

usbd_state_t usbd;

#define VID		0x16D0		/* Machdyne's, as tinydfu and Zeitlos's console */
#define PID		0x116D

#define IF_COMM		0
#define IF_DATA		1
#define IF_DFU		2
#define IF_MSC		3
#define EP_DATA		1
#define EP_NOTIFY	2
#define EP_MSC		3
#define DFU_XFER	4096

#define CFG_LEN		116

static const uint8_t dev_desc[18] = {
	18, 1, 0x00, 0x02,		/* USB 2.0 */
	0xEF, 0x02, 0x01,		/* composite, with interface association */
	64,
	VID & 0xff, VID >> 8, PID & 0xff, PID >> 8,
	MB_VERSION & 0xff, (MB_VERSION >> 8) & 0xff,
	1, 2, 0,			/* manufacturer, product, no serial number */
	1,
};

static const uint8_t cfg_desc[CFG_LEN] = {
	9, 2, CFG_LEN, 0, 4, 1, 0, 0x80, 250,	/* bus powered, 500 mA: it powers the board */

	/* the console: an interface association, then the two interfaces */
	8, 0x0B, IF_COMM, 2, 0x02, 0x02, 0x01, 3,
	9, 4, IF_COMM, 0, 1, 0x02, 0x02, 0x01, 3,
	5, 0x24, 0x00, 0x10, 0x01,		/* header, CDC 1.10 */
	5, 0x24, 0x01, 0x00, IF_DATA,		/* call management */
	4, 0x24, 0x02, 0x02,			/* ACM: line coding and serial state */
	5, 0x24, 0x06, IF_COMM, IF_DATA,	/* union */
	7, 5, 0x80 | EP_NOTIFY, 0x03, 8, 0, 255,
	9, 4, IF_DATA, 0, 2, 0x0A, 0x00, 0x00, 0,
	7, 5, EP_DATA, 0x02, 64, 0, 0,
	7, 5, 0x80 | EP_DATA, 0x02, 64, 0, 0,

	/* DFU, in DFU mode (protocol 2) */
	9, 4, IF_DFU, 0, 0, 0xFE, 0x01, 0x02, 4,
	9, 0x21, 0x07,				/* download, upload, manifestation tolerant */
	0xE8, 0x03,				/* detach timeout 1000 ms (not used) */
	DFU_XFER & 0xff, DFU_XFER >> 8,
	0x10, 0x01,				/* DFU 1.1 */

	/* mass storage: SCSI transparent command set, bulk-only transport */
	9, 4, IF_MSC, 0, 2, 0x08, 0x06, 0x50, 5,
	7, 5, EP_MSC, 0x02, 64, 0, 0,
	7, 5, 0x80 | EP_MSC, 0x02, 64, 0, 0,
};

static const char *const strings[] = {
	0,
	"Machdyne",
	"machboot",
	"machboot console",
	"System slot + Zeitlos region",
	"machboot card reader",
};

/* ------------------------------------------------------------------------- */
/* endpoint buffers                                                          */
/* ------------------------------------------------------------------------- */

static void ep_send(int ep, const uint8_t *p, uint32_t n)
{
	for (uint32_t i = 0; i < n; i += 4) {
		uint32_t w = 0;
		for (uint32_t k = 0; k < 4 && i + k < n; k++)
			w |= (uint32_t)p[i + k] << (8 * k);
		usbd_wr(UD_IBUF(ep) + i, w);
	}
	usbd_wr(UD_IN(ep), n | 0x100);
}

static void ep_send_flash(int ep, uint32_t addr, uint32_t n)
{
	for (uint32_t i = 0; i < n; i += 4)
		usbd_wr(UD_IBUF(ep) + i, flash_rd32(addr + i));
	usbd_wr(UD_IN(ep), n | 0x100);
}

static uint32_t ep_recv(int ep, uint8_t *p, uint32_t max)
{
	uint32_t n = (usbd_rd(UD_OUT(ep)) >> 8) & 0x7f;
	if (n > max)
		n = max;
	for (uint32_t i = 0; i < n; i += 4) {
		uint32_t w = usbd_rd(UD_OBUF(ep) + i);
		for (uint32_t k = 0; k < 4 && i + k < n; k++)
			p[i + k] = (uint8_t)(w >> (8 * k));
	}
	return n;
}

static int ep_armed(int ep)
{
	return (usbd_rd(UD_IN(ep)) & 0x100) != 0;
}

/* ------------------------------------------------------------------------- */
/* control transfers on endpoint 0                                           */
/* ------------------------------------------------------------------------- */

enum { C_IDLE, C_IN, C_OUT, C_STATUS_IN };

static int ctl;
static uint8_t rq[8];
static uint8_t ctl_buf[96];
static const uint8_t *tx_p;
static uint32_t tx_flash;		/* nonzero: tx_p is a flash address */
static uint32_t tx_left;
static int tx_zlp;
static uint8_t *rx_p;
static uint32_t rx_left;
static void (*rx_done)(void);
static int set_addr = -1;
static int in0_busy;

static void stall0(void)
{
	usbd_wr(UD_STALL(0), 1);
	ctl = C_IDLE;
}

static void in0_next(void)
{
	uint32_t n = tx_left < 64 ? tx_left : 64;
	if (tx_flash) {
		ep_send_flash(0, tx_flash, n);
		tx_flash += n;
	} else {
		ep_send(0, tx_p, n);
		tx_p += n;
	}
	tx_left -= n;
	in0_busy = 1;
}

/* an IN data stage: n bytes of what the host asked wlen of */
static void ctl_send(const uint8_t *p, uint32_t flash_addr, uint32_t n, uint32_t wlen)
{
	if (n > wlen)
		n = wlen;
	tx_p = p;
	tx_flash = flash_addr;
	tx_left = n;
	tx_zlp = n && (n < wlen) && (n % 64 == 0);	/* a short transfer ends with a short packet */
	ctl = C_IN;
	in0_next();
}

static void status_in(void)
{
	ep_send(0, 0, 0);
	in0_busy = 1;
	ctl = C_STATUS_IN;
}

/* an OUT data stage into p; done() runs when it is all there */
static void ctl_recv(uint8_t *p, uint32_t n, void (*done)(void))
{
	rx_p = p;
	rx_left = n;
	rx_done = done;
	ctl = C_OUT;
}

static void in0_done(void)
{
	if (ctl == C_IN) {
		if (tx_left)
			in0_next();
		else if (tx_zlp) {
			tx_zlp = 0;
			ep_send(0, 0, 0);
			in0_busy = 1;
		} else {
			ctl = C_IDLE;		/* the host's status OUT follows */
		}
	} else if (ctl == C_STATUS_IN) {
		if (set_addr >= 0) {
			usbd_wr(UD_CTRL, 1 | ((uint32_t)set_addr << 8));
			set_addr = -1;
		}
		ctl = C_IDLE;
	}
}

/* ------------------------------------------------------------------------- */
/* the console                                                               */
/* ------------------------------------------------------------------------- */

static uint8_t line_coding[7] = { 0x40, 0x42, 0x0F, 0x00, 0, 0, 8 };	/* 1000000 8N1 */
static uint8_t txq[768];
static uint32_t txq_h, txq_t;
static uint8_t rxq[256];
static uint32_t rxq_h, rxq_t;
static int polling;

static void cdc_tx(void)
{
	if (!usbd.configured || ep_armed(EP_DATA) || txq_h == txq_t)
		return;
	uint8_t b[64];
	uint32_t n = 0;
	while (n < 64 && txq_t != txq_h)
		b[n++] = txq[txq_t++ % sizeof(txq)];
	ep_send(EP_DATA, b, n);
}

static void cdc_rx(void)
{
	uint32_t o = usbd_rd(UD_OUT(EP_DATA));
	if (!(o & 1))
		return;
	uint32_t n = (o >> 8) & 0x7f;
	if (sizeof(rxq) - (rxq_h - rxq_t) < n)
		return;			/* no room: the endpoint NAKs until there is */
	uint8_t b[64];
	ep_recv(EP_DATA, b, sizeof(b));
	usbd_wr(UD_OUT(EP_DATA), 0);
	for (uint32_t i = 0; i < n; i++)
		rxq[rxq_h++ % sizeof(rxq)] = b[i];
}

void usbd_cdc_putc(char c)
{
	if (!usbd.cdc_open)
		return;
	if (txq_h - txq_t >= sizeof(txq)) {
		/* full: send what we can, for a while; a terminal that stops
		 * reading is dropped until it opens the port again */
		uint32_t t0 = ms_now();
		while (txq_h - txq_t >= sizeof(txq)) {
			if (polling || ms_now() - t0 > 50) {
				if (!polling)
					usbd.cdc_open = 0;
				return;
			}
			cdc_tx();
		}
	}
	txq[txq_h++ % sizeof(txq)] = (uint8_t)c;
}

int usbd_cdc_getc(void)
{
	if (rxq_h == rxq_t)
		return -1;
	return rxq[rxq_t++ % sizeof(rxq)];
}

static void line_coding_done(void)
{
	status_in();
}

/* ------------------------------------------------------------------------- */
/* DFU 1.1                                                                   */
/* ------------------------------------------------------------------------- */

enum {
	appIDLE = 0, appDETACH, dfuIDLE, dfuDNLOAD_SYNC, dfuDNBUSY, dfuDNLOAD_IDLE,
	dfuMANIFEST_SYNC, dfuMANIFEST, dfuMANIFEST_WAIT_RESET, dfuUPLOAD_IDLE, dfuERROR,
};
enum {
	OK = 0, errTARGET, errFILE, errWRITE, errERASE, errCHECK_ERASED, errPROG,
	errVERIFY, errADDRESS, errNOTDONE, errFIRMWARE, errVENDOR, errUSBR, errPOR,
	errUNKNOWN, errSTALLEDPKT,
};

#define MAP_SLOT_END	0x0A0000u	/* file offsets: the slot, 640 KB */
#define MAP_REGION	0x0C0000u	/* the Zeitlos region, at 0x100000 */

static int dfu_state = dfuIDLE;
static int dfu_status = OK;
static uint8_t dl_buf[DFU_XFER];
static uint32_t dl_block, dl_len, dl_next;
static uint32_t dl_crc, keep_len;

static void dfu_fail(int status, const char *why)
{
	dfu_state = dfuERROR;
	dfu_status = status;
	usbd.dfu = UD_DFU_FAILED;
	usbd.dfu_error = why;
	usbd.dfu_events++;
	con_printf("usb: dfu: %s\n", why);
}

static uint8_t dl_get(void *ctx, uint32_t off) { (void)ctx; return dl_buf[off]; }

static void dfu_block(void)
{
	uint32_t off = dl_block * DFU_XFER, n = dl_len;

	if (dl_block != dl_next) {
		dfu_fail(errADDRESS, "blocks out of order");
		return;
	}
	dl_next++;

	if (off == 0) {
		slot_info_t si;
		bit_parse(dl_get, 0, n, &si);
		if (!si.valid) {
			dfu_fail(errFILE, "not a bitstream");
			return;
		}
		if (si.idcode && si.idcode != mb.idcode) {
			dfu_fail(errTARGET, "for another FPGA");
			return;
		}
		/* the slot is invalid from here until the end */
		if (flash_erase4k(SYSTEM_SLOT)) {
			dfu_fail(errERASE, "the flash refused");
			return;
		}
		memcpy(sector_buf, dl_buf, n);
		keep_len = n;
		dl_crc = mb_crc32(0, dl_buf, n);
		usbd.dfu = UD_DFU_RECEIVING;
		usbd.dfu_bytes = n;
		usbd.dfu_slot_bytes = n;
		usbd.dfu_region_bytes = 0;
		usbd.dfu_events++;
		con_printf("usb: dfu: receiving a bitstream for the system slot\n");
		return;
	}

	uint32_t dest;
	if (off < MAP_SLOT_END) {
		dest = SYSTEM_SLOT + off;
		dl_crc = mb_crc32(dl_crc, dl_buf, n);
		usbd.dfu_slot_bytes += n;
	} else if (off < MAP_REGION) {
		for (uint32_t i = 0; i < n; i++)
			if (dl_buf[i] != 0xff) {
				dfu_fail(errADDRESS, "the gateware is larger than the slot (640 KB)");
				return;
			}
		usbd.dfu_bytes += n;
		return;
	} else {
		dest = off - MAP_REGION + ZEITLOS_REGION;
		if (!mb.flash.size || dest + n > mb.flash.size) {
			dfu_fail(errADDRESS, "larger than the flash");
			return;
		}
		usbd.dfu_region_bytes += n;
	}
	if (!same_as_flash(dest, dl_buf, n) && write_sector(dest, dl_buf, n)) {
		dfu_fail(errVERIFY, "the flash could not be written");
		return;
	}
	usbd.dfu_bytes += n;
	usbd.dfu_events++;
}

static void dfu_manifest(void)
{
	if (!keep_len) {
		dfu_fail(errNOTDONE, "nothing was received");
		return;
	}
	if (write_sector(SYSTEM_SLOT, sector_buf, keep_len)) {
		dfu_fail(errVERIFY, "the flash could not be written");
		return;
	}
	keep_len = 0;
	/* the name: Zeitlos's -dfu.bin says so; a bare bitstream is taken to
	 * be a new version of what was there */
	char name[24];
	const char *n = usbd.dfu_region_bytes ? "Zeitlos" : cfg.sys_name[0] ? cfg.sys_name :
	                mb.zeitlos_region ? "Zeitlos" : "System";
	memset(name, 0, sizeof(name));
	memcpy(name, n, strlen(n) < sizeof(name) - 1 ? strlen(n) : sizeof(name) - 1);
	settings_set_system(name, "", dl_crc, usbd.dfu_slot_bytes);
	mb_probe_system();
	usbd.dfu = UD_DFU_DONE;
	usbd.dfu_events++;
	con_printf("usb: dfu: done, %u KB into the system slot, %u KB into the Zeitlos region\n",
	           usbd.dfu_slot_bytes / 1024, usbd.dfu_region_bytes / 1024);
}

static void dl_done(void)
{
	dfu_state = dfuDNLOAD_IDLE;
	dfu_block();
	status_in();
}

/* writing: only when machboot runs from 0x000000 (not install mode, not
 * the JTAG test build), as for installs from the card */
static int dfu_can_write(void)
{
	return mb.fw_base == FW_OFFSET;
}

static void dfu_request(uint8_t req, uint16_t value, uint16_t len)
{
	switch (req) {
	case 1:		/* DNLOAD */
		if (dfu_state != dfuIDLE && dfu_state != dfuDNLOAD_IDLE) {
			stall0();
			return;
		}
		if (len == 0) {
			if (dfu_state != dfuDNLOAD_IDLE) {
				dfu_fail(errNOTDONE, "an empty download");
				stall0();
				return;
			}
			dfu_state = dfuMANIFEST_SYNC;
			status_in();
			return;
		}
		if (len > DFU_XFER) {
			stall0();
			return;
		}
		if (dfu_state == dfuIDLE) {
			if (usbd.msc_shared) {
				dfu_fail(errTARGET, "the card reader is on; leave its page first");
				stall0();
				return;
			}
			if (!dfu_can_write()) {
				dfu_fail(errTARGET, "install machboot first (not in install mode or the JTAG test build)");
				stall0();
				return;
			}
			dl_next = 0;
			keep_len = 0;
		}
		dl_block = value;
		dl_len = len;
		ctl_recv(dl_buf, len, dl_done);
		return;
	case 2: {	/* UPLOAD: the system slot */
		if (dfu_state != dfuIDLE && dfu_state != dfuUPLOAD_IDLE) {
			stall0();
			return;
		}
		uint32_t off = (uint32_t)value * DFU_XFER;
		uint32_t left = off < MAP_SLOT_END ? MAP_SLOT_END - off : 0;
		uint32_t n = len < left ? len : left;
		dfu_state = n < len ? dfuIDLE : dfuUPLOAD_IDLE;
		ctl_send(0, SYSTEM_SLOT + off, n, len);	/* n = 0: one zero-length packet */
		return;
	}
	case 3: {	/* GETSTATUS */
		if (dfu_state == dfuMANIFEST_SYNC) {
			dfu_manifest();
			if (dfu_state != dfuERROR)
				dfu_state = dfuIDLE;
		}
		ctl_buf[0] = (uint8_t)dfu_status;
		ctl_buf[1] = 0; ctl_buf[2] = 0; ctl_buf[3] = 0;	/* poll again at once: the work is done */
		ctl_buf[4] = (uint8_t)dfu_state;
		ctl_buf[5] = 0;
		ctl_send(ctl_buf, 0, 6, len);
		return;
	}
	case 4:		/* CLRSTATUS */
		if (dfu_state == dfuERROR) {
			dfu_state = dfuIDLE;
			dfu_status = OK;
		}
		status_in();
		return;
	case 5:		/* GETSTATE */
		ctl_buf[0] = (uint8_t)dfu_state;
		ctl_send(ctl_buf, 0, 1, len);
		return;
	case 6:		/* ABORT */
		if (dfu_state == dfuDNLOAD_IDLE) {
			usbd.dfu = UD_DFU_FAILED;
			usbd.dfu_error = "aborted by the host";
			usbd.dfu_events++;
		}
		dfu_state = dfuIDLE;
		dfu_status = OK;
		status_in();
		return;
	case 0:		/* DETACH: already in DFU mode */
		status_in();
		return;
	default:
		stall0();
	}
}

/* ------------------------------------------------------------------------- */
/* mass storage: the SD card (USB Mass Storage Bulk-Only Transport 1.0;     */
/* SCSI SPC-2/SBC-2 commands, the few a PC sends a card reader)             */
/* ------------------------------------------------------------------------- */

enum { M_CBW, M_IN, M_OUT, M_CSW };
enum { MS_OK = 0, MS_FAIL = 1 };

static int m_state;
static uint8_t m_cbw[31];
static uint32_t m_tag, m_len, m_done;		/* the host's length, bytes moved */
static int m_status;
static uint8_t m_sense[3];			/* key, ASC, ASCQ */
#define m_buf dl_buf			/* the card reader and DFU never run together */
static uint32_t m_buf_len, m_buf_pos;
static uint32_t m_lba, m_count;			/* READ/WRITE: sectors left */
static int m_op;				/* 0x28 read, 0x2A write, 0: m_buf only */
static int m_changed;				/* tell the host once: the medium changed */

static void put32be(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static void sense(uint8_t key, uint8_t asc, uint8_t ascq)
{
	m_sense[0] = key; m_sense[1] = asc; m_sense[2] = ascq;
}

void usbd_before_start(void)
{
}

void usbd_msc_share(int on)
{
	if (on == usbd.msc_shared)
		return;
	if (on && dfu_state != dfuIDLE)
		return;			/* dfu-util is writing: not now */
	usbd.msc_shared = on;
	m_changed = 1;
	usbd.msc_events++;
}

static int msc_ready(void)
{
	if (!usbd.msc_shared || !sd.present) {
		sense(0x02, 0x3A, 0x00);	/* NOT READY, medium not present */
		return 0;
	}
	if (m_changed) {
		m_changed = 0;
		sense(0x06, 0x28, 0x00);	/* UNIT ATTENTION, medium may have changed */
		return 0;
	}
	return 1;
}

/* answer with n bytes of m_buf, at most what the host asked for */
static void m_reply(uint32_t n)
{
	m_buf_len = n < m_len ? n : m_len;
	m_buf_pos = 0;
	m_op = 0;
	m_state = M_IN;
}

static void m_fail(uint8_t key, uint8_t asc)
{
	sense(key, asc, 0);
	m_status = MS_FAIL;
	if (m_len && (m_cbw[12] & 0x80)) {
		/* the host expects data: send it zeros, all it asked for
		 * (BOT 6.7.2 allows padding), then fail in the CSW */
		m_buf_len = 0;
		m_buf_pos = 0;
		m_op = -1;
		m_state = M_IN;
	} else if (m_len) {
		m_op = -1;		/* take and drop the data */
		m_state = M_OUT;
	} else {
		m_state = M_CSW;
	}
}

static void msc_command(void)
{
	const uint8_t *cb = m_cbw + 15;
	m_status = MS_OK;
	m_done = 0;
	switch (cb[0]) {
	case 0x00:		/* TEST UNIT READY */
		if (!msc_ready()) { m_status = MS_FAIL; }
		m_state = M_CSW;
		return;
	case 0x03:		/* REQUEST SENSE */
		memset(m_buf, 0, 18);
		m_buf[0] = 0x70;
		m_buf[2] = m_sense[0];
		m_buf[7] = 10;
		m_buf[12] = m_sense[1];
		m_buf[13] = m_sense[2];
		sense(0, 0, 0);
		m_reply(18);
		return;
	case 0x12: {		/* INQUIRY */
		static const char id[] = "Machdynemachboot card    " "0.3 ";
		memset(m_buf, 0, 36);
		m_buf[1] = 0x80;	/* removable */
		m_buf[2] = 0x04;	/* SPC-2 */
		m_buf[3] = 0x02;
		m_buf[4] = 31;
		memcpy(m_buf + 8, id, 28);
		m_reply(36);
		return;
	}
	case 0x1A:		/* MODE SENSE (6) */
		memset(m_buf, 0, 4);
		m_buf[0] = 3;
		m_reply(4);
		return;
	case 0x5A:		/* MODE SENSE (10) */
		memset(m_buf, 0, 8);
		m_buf[1] = 6;
		m_reply(8);
		return;
	case 0x1E:		/* PREVENT/ALLOW MEDIUM REMOVAL */
	case 0x35:		/* SYNCHRONIZE CACHE */
	case 0x2F:		/* VERIFY */
		m_state = M_CSW;
		return;
	case 0x1B:		/* START STOP UNIT: eject gives the card back */
		if ((cb[4] & 0x03) == 0x02)
			usbd_msc_share(0);
		m_state = M_CSW;
		return;
	case 0x25:		/* READ CAPACITY (10) */
		if (!msc_ready()) { m_fail(m_sense[0], m_sense[1]); return; }
		put32be(m_buf, sd.sectors - 1);
		put32be(m_buf + 4, 512);
		m_reply(8);
		return;
	case 0x23:		/* READ FORMAT CAPACITIES */
		if (!msc_ready()) { m_fail(m_sense[0], m_sense[1]); return; }
		memset(m_buf, 0, 12);
		m_buf[3] = 8;
		put32be(m_buf + 4, sd.sectors);
		m_buf[8] = 0x02;	/* formatted */
		m_buf[10] = 0x02;	/* 512 */
		m_reply(12);
		return;
	case 0x28:		/* READ (10) */
	case 0x2A:		/* WRITE (10) */
		if (!msc_ready()) { m_fail(m_sense[0], m_sense[1]); return; }
		m_lba = (uint32_t)cb[2] << 24 | (uint32_t)cb[3] << 16 | (uint32_t)cb[4] << 8 | cb[5];
		m_count = (uint32_t)cb[7] << 8 | cb[8];
		if (m_lba + m_count > sd.sectors || m_count * 512 != m_len) {
			m_fail(0x05, 0x21);	/* ILLEGAL REQUEST, LBA out of range */
			return;
		}
		m_op = cb[0];
		m_buf_len = 0;
		m_buf_pos = 0;
		m_state = cb[0] == 0x28 ? M_IN : M_OUT;
		return;
	default:
		m_fail(0x05, 0x20);		/* ILLEGAL REQUEST, invalid command */
		return;
	}
}

static void msc_poll(void)
{
	if (m_state == M_CBW) {
		uint32_t o = usbd_rd(UD_OUT(EP_MSC));
		if (!(o & 1))
			return;
		uint32_t n = ep_recv(EP_MSC, m_cbw, sizeof(m_cbw));
		usbd_wr(UD_OUT(EP_MSC), 0);
		if (n != 31 || m_cbw[0] != 'U' || m_cbw[1] != 'S' || m_cbw[2] != 'B' || m_cbw[3] != 'C')
			return;		/* not a CBW: ignore it */
		m_tag = (uint32_t)m_cbw[4] | (uint32_t)m_cbw[5] << 8 | (uint32_t)m_cbw[6] << 16 | (uint32_t)m_cbw[7] << 24;
		m_len = (uint32_t)m_cbw[8] | (uint32_t)m_cbw[9] << 8 | (uint32_t)m_cbw[10] << 16 | (uint32_t)m_cbw[11] << 24;
		msc_command();
		return;
	}
	if (m_state == M_IN) {
		if (ep_armed(EP_MSC))
			return;
		if (m_buf_pos >= m_buf_len) {
			/* the next sector, padding, or the end */
			if (m_op == 0x28 && m_count && sd_read(m_lba, m_buf)) {
				sense(0x03, 0x11, 0);	/* MEDIUM ERROR, unrecovered read */
				m_status = MS_FAIL;
				m_op = -1;		/* the rest is zeros: the host gets all it asked for */
			}
			if (m_op == 0x28 && m_count) {
				m_lba++;
				m_count--;
				m_buf_len = 512;
				m_buf_pos = 0;
				usbd.msc_read += 512;
			} else if (m_op == -1 && m_done < m_len) {
				m_buf_len = m_len - m_done < 512 ? m_len - m_done : 512;
				memset(m_buf, 0, m_buf_len);
				m_buf_pos = 0;
			} else {
				m_state = M_CSW;
				return;
			}
		}
		uint32_t n = m_buf_len - m_buf_pos < 64 ? m_buf_len - m_buf_pos : 64;
		ep_send(EP_MSC, m_buf + m_buf_pos, n);
		m_buf_pos += n;
		m_done += n;
		return;
	}
	if (m_state == M_OUT) {
		uint32_t o = usbd_rd(UD_OUT(EP_MSC));
		if (!(o & 1))
			return;
		uint32_t n = ep_recv(EP_MSC, m_buf + m_buf_pos, sizeof(m_buf) - m_buf_pos);
		usbd_wr(UD_OUT(EP_MSC), 0);
		m_buf_pos += n;
		m_done += n;
		if (m_op == 0x2A && m_buf_pos >= 512) {
			if (sd_write(m_lba, m_buf)) {
				sense(0x03, 0x0C, 0);	/* MEDIUM ERROR, write error */
				m_status = MS_FAIL;
				m_op = -1;
			}
			m_lba++;
			m_count--;
			m_buf_pos = 0;
			usbd.msc_written += 512;
		} else if (m_op != 0x2A && m_buf_pos >= sizeof(m_buf)) {
			m_buf_pos = 0;
		}
		if (m_done >= m_len || n < 64)
			m_state = M_CSW;
		return;
	}
	if (m_state == M_CSW) {
		if (ep_armed(EP_MSC))
			return;
		uint8_t csw[13];
		uint32_t res = m_len > m_done ? m_len - m_done : 0;
		csw[0] = 'U'; csw[1] = 'S'; csw[2] = 'B'; csw[3] = 'S';
		for (int i = 0; i < 4; i++) {
			csw[4 + i] = (uint8_t)(m_tag >> (8 * i));
			csw[8 + i] = (uint8_t)(res >> (8 * i));
		}
		csw[12] = (uint8_t)m_status;
		ep_send(EP_MSC, csw, 13);
		m_state = M_CBW;
		usbd.msc_events++;
	}
}

/* ------------------------------------------------------------------------- */
/* chapter 9                                                                 */
/* ------------------------------------------------------------------------- */

static void configure(int cfg)
{
	usbd.configured = cfg;
	if (cfg) {
		/* endpoint 1 bulk IN and OUT, endpoint 2 interrupt IN */
		uint32_t bulk = (1u << EP_DATA) | (1u << EP_MSC);
		usbd_wr(UD_EPTYPE, bulk | (bulk << 4) | ((1u << EP_NOTIFY) << 8));
		usbd_wr(UD_TOGGLE, bulk | (1u << EP_NOTIFY) | (bulk << 4));
		m_state = M_CBW;
	} else {
		usbd_wr(UD_EPTYPE, 0);
		usbd.cdc_open = 0;
	}
}

static void get_descriptor(uint8_t type, uint8_t index, uint16_t len)
{
	if (type == 1) {
		ctl_send(dev_desc, 0, sizeof(dev_desc), len);
	} else if (type == 2) {
		ctl_send(cfg_desc, 0, sizeof(cfg_desc), len);
	} else if (type == 3) {
		uint32_t n;
		if (index == 0) {
			ctl_buf[2] = 0x09;	/* English (US) */
			ctl_buf[3] = 0x04;
			n = 4;
		} else if (index < sizeof(strings) / sizeof(strings[0])) {
			const char *s = strings[index];
			n = 2;
			for (; *s && n + 2 <= sizeof(ctl_buf); s++) {
				ctl_buf[n++] = (uint8_t)*s;
				ctl_buf[n++] = 0;
			}
		} else {
			stall0();
			return;
		}
		ctl_buf[0] = (uint8_t)n;
		ctl_buf[1] = 3;
		ctl_send(ctl_buf, 0, n, len);
	} else {
		stall0();		/* device qualifier and others: full speed only */
	}
}

static void setup(void)
{
	uint8_t type = (rq[0] >> 5) & 3, recip = rq[0] & 0x1f, req = rq[1];
	uint16_t value = (uint16_t)(rq[2] | rq[3] << 8);
	uint16_t index = (uint16_t)(rq[4] | rq[5] << 8);
	uint16_t len = (uint16_t)(rq[6] | rq[7] << 8);

	ctl = C_IDLE;
	in0_busy = 0;

	if (type == 0) {
		switch (req) {
		case 6:
			get_descriptor((uint8_t)(value >> 8), (uint8_t)value, len);
			return;
		case 5:
			set_addr = value & 0x7f;
			status_in();
			return;
		case 9:
			configure(value & 0xff);
			status_in();
			return;
		case 8:
			ctl_buf[0] = (uint8_t)usbd.configured;
			ctl_send(ctl_buf, 0, 1, len);
			return;
		case 0:
			ctl_buf[0] = 0;
			ctl_buf[1] = 0;
			if (recip == 2)
				ctl_buf[0] = (uint8_t)(usbd_rd(UD_STALL(index & 3)) & 1);
			ctl_send(ctl_buf, 0, 2, len);
			return;
		case 1:
		case 3:
			if (recip == 2 && value == 0) {		/* ENDPOINT_HALT */
				int ep = index & 0x0f;
				if (ep > 3) {
					stall0();
					return;
				}
				usbd_wr(UD_STALL(ep), req == 3);
				if (req == 1)
					usbd_wr(UD_TOGGLE, (index & 0x80) ? (1u << ep) : ((1u << ep) << 4));
			}
			status_in();
			return;
		case 10:
			ctl_buf[0] = 0;
			ctl_send(ctl_buf, 0, 1, len);
			return;
		case 11:
			if (value != 0) {
				stall0();
				return;
			}
			status_in();
			return;
		default:
			stall0();
			return;
		}
	}
	if (type == 1 && recip == 1) {
		if ((index & 0xff) == IF_COMM) {
			switch (req) {
			case 0x20:	/* SET_LINE_CODING */
				ctl_recv(line_coding, sizeof(line_coding), line_coding_done);
				return;
			case 0x21:	/* GET_LINE_CODING */
				ctl_send(line_coding, 0, sizeof(line_coding), len);
				return;
			case 0x22:	/* SET_CONTROL_LINE_STATE: DTR is "a terminal is open" */
				usbd.cdc_open = value & 1;
				if (!usbd.cdc_open)
					txq_t = txq_h;
				status_in();
				return;
			case 0x23:	/* SEND_BREAK */
				status_in();
				return;
			default:
				stall0();
				return;
			}
		}
		if ((index & 0xff) == IF_DFU) {
			dfu_request(req, value, len);
			return;
		}
		if ((index & 0xff) == IF_MSC) {
			if (req == 0xFE) {		/* GET MAX LUN */
				ctl_buf[0] = 0;
				ctl_send(ctl_buf, 0, 1, len);
				return;
			}
			if (req == 0xFF) {		/* bulk-only mass storage reset */
				m_state = M_CBW;
				usbd_wr(UD_OUT(EP_MSC), 0);
				status_in();
				return;
			}
		}
	}
	stall0();
}

/* ------------------------------------------------------------------------- */

static void bus_reset(void)
{
	ctl = C_IDLE;
	in0_busy = 0;
	set_addr = -1;
	configure(0);
	if (dfu_state == dfuDNLOAD_IDLE || dfu_state == dfuDNLOAD_SYNC) {
		usbd.dfu = UD_DFU_FAILED;
		usbd.dfu_error = "the host reset the device";
		usbd.dfu_events++;
	}
	dfu_state = dfuIDLE;
	dfu_status = OK;
}

int usbd_init(void)
{
	memset(&usbd, 0, sizeof(usbd));
	if (!(mb.features & FEAT_USBD) || usbd_rd(UD_MAGIC) != UD_MAGIC_VAL)
		return 0;
	usbd.present = 1;
	usbd_wr(UD_EVENTS, 1);
	usbd_wr(UD_CTRL, 1);		/* attach: the pull-up goes on */
	return 1;
}

void usbd_poll(void)
{
	if (!usbd.present || polling)
		return;
	polling = 1;

	if (usbd_rd(UD_EVENTS) & 1) {
		usbd_wr(UD_EVENTS, 1);
		bus_reset();
	}

	uint32_t o = usbd_rd(UD_OUT(0));
	if (o & 1) {
		if (o & 2) {
			memset(rq, 0, sizeof(rq));
			ep_recv(0, rq, sizeof(rq));
			usbd_wr(UD_OUT(0), 0);
			setup();
		} else if (ctl == C_OUT) {
			uint32_t n = ep_recv(0, rx_p, rx_left);
			usbd_wr(UD_OUT(0), 0);
			rx_p += n;
			rx_left -= n;
			if (rx_left == 0 || n < 64) {
				ctl = C_IDLE;
				rx_done();
			}
		} else {
			usbd_wr(UD_OUT(0), 0);	/* a status stage, or nothing we wait for */
		}
	}

	if (in0_busy && !ep_armed(0)) {
		in0_busy = 0;
		in0_done();
	}

	if (usbd.configured) {
		cdc_rx();
		cdc_tx();
		msc_poll();
	}
	polling = 0;
}
