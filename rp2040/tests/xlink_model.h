/*
 * machboot supervisor
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The XA-XD link for the PC tests: rtl/mb_xlink.v's state at the byte
 * level, the FPGA firmware's own fw/xlink.c behind it (with what it
 * needs of the rest of the firmware), and the RP2040 side's pins
 * (xlink_io.h). Included once per test; the test provides now_ms,
 * xio_ms() and ms_now().
 */

#include <stdarg.h>
#include "xlink.h"
#include "xlink_io.h"
#include "usbd.h"
#include "hw.h"
#include "mb.h"
#include "sd.h"
#include "settings.h"
#include "fat.h"
#include "flash.h"
#include "console.h"
#include "systems.h"

static int fpga_machboot = 1;		/* 0: another image, nobody answers */
static int fw_delay;			/* polls the firmware sleeps through (a slow SD card) */
static int fw_hold;			/* the firmware is busy for good (until cleared) */

static uint8_t rxbuf[2048], txbuf[2048];
static int rx_full, rx_len, rx_seq, tx_armed, tx_len, flags;

uint32_t xl_rd(uint32_t off)
{
	if (off >= 0x1000 && off < 0x1800) {
		uint32_t o = off - 0x1000;
		return rxbuf[o] | rxbuf[o + 1] << 8 | rxbuf[o + 2] << 16 | (uint32_t)rxbuf[o + 3] << 24;
	}
	switch (off) {
	case 0x0: return 0x4B4E4C58u;
	case 0x4: return (uint32_t)(rx_len & 0x7ff) << 16 | (uint32_t)(rx_seq & 0xff) << 8 | (uint32_t)rx_full;
	case 0x8: return (uint32_t)tx_armed;
	case 0xC: return (uint32_t)flags;
	}
	return 0;
}

void xl_wr(uint32_t off, uint32_t v)
{
	if (off >= 0x1800 && off < 0x2000) {
		uint32_t o = off - 0x1800;
		for (int k = 0; k < 4; k++) txbuf[o + k] = (uint8_t)(v >> (8 * k));
		return;
	}
	if (off == 0x4 && (v & 1)) rx_full = 0;
	if (off == 0x8) { tx_armed = (v >> 31) & 1; tx_len = v & 0x7ff; }
	if (off == 0xC) flags = v & 0xff;
}

/* what fw/xlink.c needs from the rest of the firmware */
mb_t mb;
settings_t cfg;
int settings_remote, settings_dirty, kbd_layout;
int mb_snprintf(char *b, size_t n, const char *f, ...)
{
	va_list ap;
	va_start(ap, f);
	int r = vsnprintf(b, n, f, ap);
	va_end(ap);
	return r;
}
sd_info_t sd;
static uint8_t disk[4096 * 512];
static int disk_writes, disk_reads;
void con_puts(const char *s) { (void)s; }
int sd_read(uint32_t lba, uint8_t *buf) { disk_reads++; if (lba >= 4096) return -1; memcpy(buf, disk + lba * 512, 512); return 0; }
int sd_write(uint32_t lba, const uint8_t *buf) { if (lba >= 4096) return -1; memcpy(disk + lba * 512, buf, 512); disk_writes++; return 0; }

/* a PUT reads its bitstream from the card (fw/xlink.c): a file in memory */
static const uint8_t *file_data;
static uint32_t file_size;
static int file_reads;
static uint32_t file_cancel_at;		/* Esc, when the copy has come this far */
static uint32_t file_usb_at;		/* the PC's USB, in the middle of a copy (test_systems.c) */
static void (*file_usb)(void);
void fat_open(fat_vol_t *v, const fat_entry_t *e, fat_file_t *f) { (void)v; (void)e; memset(f, 0, sizeof(*f)); f->size = file_size; }
int fat_read(fat_file_t *f, uint8_t *buf, uint32_t n)
{
	if (f->pos + n > file_size) n = file_size - f->pos;
	memcpy(buf, file_data + f->pos, n);
	f->pos += n;
	file_reads++;
	if (file_cancel_at && f->pos >= file_cancel_at)
		rc_cancel();
	if (file_usb && file_usb_at && f->pos >= file_usb_at) {
		file_usb_at = 0;
		file_usb();
	}
	return (int)n;
}
int fat_seek0(fat_file_t *f) { f->pos = 0; return 0; }
void con_printf(const char *fmt, ...) { (void)fmt; }
void bit_parse(uint8_t (*get)(void *ctx, uint32_t off), void *ctx, uint32_t limit, slot_info_t *si)
{
	memset(si, 0, sizeof(*si));
	for (uint32_t i = 0; i + 8 <= limit; i++)
		if (get(ctx, i) == 0xE2 && !get(ctx, i + 1) && !get(ctx, i + 2) && !get(ctx, i + 3)) {
			si->valid = 1;
			si->idcode = (uint32_t)get(ctx, i + 4) << 24 | get(ctx, i + 5) << 16 | get(ctx, i + 6) << 8 | get(ctx, i + 7);
			return;
		}
}
/* the list of systems is fw/systems.c's: here only the text it adds */
static char sys_text[512];
void systems_clear(void) { sys_text[0] = 0; }
void systems_line(const char *line, uint32_t len) { strncat(sys_text, line, len < 100 ? len : 100); strcat(sys_text, "\n"); }
int systems_text(char *b, int cap) { int n = (int)strlen(sys_text); if (n >= cap) n = 0; memcpy(b, sys_text, (size_t)n); return n; }

static void firmware(void)
{
	if (!fpga_machboot || fw_hold)
		return;
	if (fw_delay > 0) { fw_delay--; return; }
	usbd_poll();
}

/* -- the wire: frames ------------------------------------------------------ */

static int attached, cs_low, nbyte, cmd, take, give, sent;
static int frames_total;		/* every frame the RP2040 started */

void xio_attach(int on) { attached = on; }
void xio_idle(void) { now_ms++; firmware(); }

void xio_select(int on)
{
	if (!attached) { printf("  FAIL a frame on released pins\n"); fails++; }
	if (on) {
		frames_total++;
		cs_low = 1; nbyte = 0; cmd = 0; take = !rx_full; give = tx_armed; sent = 0;
		return;
	}
	cs_low = 0;
	if (!fpga_machboot)
		return;
	if (cmd == XL_C_SEND && take && nbyte > 1) {
		rx_full = 1; rx_len = (nbyte - 1) & 0x7ff; rx_seq++;
	}
	if (cmd == XL_C_RECV && give && sent >= (tx_len ? tx_len : 2048))
		tx_armed = 0;
	/* the firmware does not run here: on the board the RP2040's next STATUS
	   comes microseconds after a SEND, before machboot has looked */
}

static uint8_t byte_out(int i)		/* the slave's byte i of the frame */
{
	if (i == 0) return XL_SYNC;
	if (cmd == XL_C_STATUS) {
		switch (i) {
		case 1: return (uint8_t)flags;
		case 2: return (uint8_t)(tx_armed << 1 | rx_full);
		case 3: return (uint8_t)tx_len;
		case 4: return (uint8_t)(tx_len >> 8 & 7);
		case 5: return (uint8_t)rx_seq;
		}
		return 0;
	}
	if (cmd == XL_C_RECV) return txbuf[(i - 1) & 2047];
	return 0;
}

void xio_xfer(const uint8_t *out, uint8_t *in, int n)
{
	for (int k = 0; k < n; k++) {
		uint8_t o = out ? out[k] : 0;
		if (nbyte == 0) cmd = o;
		uint8_t r = fpga_machboot ? byte_out(nbyte) : 0xFF;
		if (cmd == XL_C_SEND && nbyte >= 1 && take && nbyte <= 2048) rxbuf[nbyte - 1] = o;
		if (cmd == XL_C_RECV && nbyte >= 1) sent++;
		if (in) in[k] = r;
		nbyte++;
	}
}

