/*
 * machboot
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 */

#include "console.h"
#include "hw.h"
#include "lib.h"
#include "usb.h"
#include "usbd.h"

uint32_t ms_now(void)
{
	return sys_rd(SYS_MS);
}

void ms_sleep(uint32_t ms)
{
	uint32_t t0 = ms_now();
	while (ms_now() - t0 < ms)
		;
}

void con_init(void)
{
	/* 48 MHz / (16 * 3) = 1 Mbaud, 8N1, as Zeitlos's console */
	uart_reg_wr(UART_LCR, 0x83);
	uart_reg_wr(UART_DATA, 3);	/* divisor latch low */
	uart_reg_wr(UART_IER, 0);	/* divisor latch high */
	uart_reg_wr(UART_LCR, 0x03);
	uart_reg_wr(UART_FCR, 0x07);
	uart_reg_wr(UART_IER, 0);
}

void con_putc(char c)
{
	while (!(uart_reg_rd(UART_LSR) & 0x20))
		;
	uart_reg_wr(UART_DATA, (uint8_t)c);
	usbd_cdc_putc(c);
}

void con_puts(const char *s)
{
	while (*s) {
		if (*s == '\n')
			con_putc('\r');
		con_putc(*s++);
	}
}

void con_printf(const char *fmt, ...)
{
	char buf[160];
	va_list ap;
	va_start(ap, fmt);
	mb_vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	con_puts(buf);
}

static int pushback = -1;

int con_getc(void)
{
	if (pushback >= 0) {
		int c = pushback;
		pushback = -1;
		return c;
	}
	if (uart_reg_rd(UART_LSR) & 0x01)
		return uart_reg_rd(UART_DATA);
	usbd_poll();
	return usbd_cdc_getc();
}

/* the next byte of an escape sequence, waiting a little for it */
static int seq_getc(void)
{
	uint32_t t0 = ms_now();
	int c;
	while ((c = con_getc()) < 0)
		if (ms_now() - t0 > 30)
			return -1;
	return c;
}

static int tilde_key(int n)
{
	switch (n) {
	case 3: return KEY_DEL;
	case 11: return KEY_F1;
	case 12: return KEY_F2;
	case 13: return KEY_F3;
	case 14: return KEY_F4;
	case 15: return KEY_F5;
	case 17: return KEY_F6;
	case 18: return KEY_F7;
	case 19: return KEY_F8;
	case 20: return KEY_F9;
	case 21: return KEY_F10;
	case 23: return KEY_F11;
	case 24: return KEY_F12;
	default: return KEY_NONE;
	}
}

/* -- USB keyboards ----------------------------------------------------------
 *
 * The controller keeps each keyboard's current boot report in its compat
 * block: modifiers in INFO[7:0], up to four held keys in KEYS
 * (key1 in [31:24] ... key4 in [7:0]). A key that appears is a press;
 * the newest held key repeats after 400 ms, every 50 ms.
 */

int kbd_layout;			/* 0 US, 1 German (settings.h KBD_*) */

static uint8_t held[USB_PORTS][4];
static int kq[16];
static unsigned kq_head, kq_tail;
static uint8_t rep_usage, rep_mods;
static int rep_port;
static uint32_t rep_t;

static void kq_push(int k)
{
	if (k != KEY_NONE && kq_tail - kq_head < 16)
		kq[kq_tail++ & 15] = k;
}

static int usage_key(uint8_t u, uint8_t mods)
{
	int shift = (mods & 0x22) != 0;
	if (u >= 0x04 && u <= 0x1d) {
		int c = 'a' + (u - 0x04);
		if (kbd_layout == 1 && c == 'y') c = 'z';
		else if (kbd_layout == 1 && c == 'z') c = 'y';
		return shift ? c - 32 : c;
	}
	if (u >= 0x1e && u <= 0x26)
		return '1' + (u - 0x1e);
	if (u >= 0x3a && u <= 0x45)
		return KEY_F1 + (u - 0x3a);
	switch (u) {
	case 0x27: return '0';
	case 0x28: case 0x58: return KEY_ENTER;
	case 0x29: return KEY_ESC;
	case 0x2a: return KEY_BKSP;
	case 0x2b: return KEY_TAB;
	case 0x2c: return ' ';
	case 0x4c: return KEY_DEL;
	case 0x4f: return KEY_RIGHT;
	case 0x50: return KEY_LEFT;
	case 0x51: return KEY_DOWN;
	case 0x52: return KEY_UP;
	default: return KEY_NONE;
	}
}

static int in4(const uint8_t *a, uint8_t u)
{
	return a[0] == u || a[1] == u || a[2] == u || a[3] == u;
}

static void usb_keys(void)
{
	usb_step();
	for (int p = 0; p < USB_PORTS; p++) {
		int blk = usb_keyboard_block(p);
		if (blk < 0) {
			memset(held[p], 0, 4);
			continue;
		}
		uint32_t info = usbh_rd(blk ? 0x020 : 0x000);
		uint32_t keys = usbh_rd(blk ? 0x024 : 0x004);
		uint8_t mods = (uint8_t)info;
		uint8_t cur[4] = { (uint8_t)(keys >> 24), (uint8_t)(keys >> 16), (uint8_t)(keys >> 8), (uint8_t)keys };
		for (int i = 0; i < 4; i++) {
			uint8_t u = cur[i];
			if (u < 4 || in4(held[p], u))
				continue;
			kq_push(usage_key(u, mods));
			rep_usage = u;
			rep_mods = mods;
			rep_port = p;
			rep_t = ms_now() + 400;
		}
		memcpy(held[p], cur, 4);
	}
	if (rep_usage) {
		if (!in4(held[rep_port], rep_usage)) {
			rep_usage = 0;
		} else if ((int32_t)(ms_now() - rep_t) >= 0) {
			kq_push(usage_key(rep_usage, rep_mods));
			rep_t = ms_now() + 50;
		}
	}
}

/*
 * Terminal keys: VT100/xterm sequences, which is what minicom, picocom,
 * screen and PuTTY send by default.
 *
 *   ESC [ A..D        arrows           ESC O P..S     F1-F4
 *   ESC [ n ~         F1-F12, Delete   ESC alone      Esc (after 30 ms)
 */
int key_poll(void)
{
	usb_keys();
	if (kq_head != kq_tail)
		return kq[kq_head++ & 15];

	int c = con_getc();
	if (c < 0)
		return KEY_NONE;
	if (c == '\r' || c == '\n')
		return KEY_ENTER;
	if (c == '\t')
		return KEY_TAB;
	if (c == 0x7f || c == 0x08)
		return KEY_BKSP;
	if (c != 0x1b)
		return (c >= 0x20 && c < 0x7f) ? c : KEY_NONE;

	int c1 = seq_getc();
	if (c1 < 0)
		return KEY_ESC;
	if (c1 == 0x1b) {
		/* Esc, then the start of something else: keep that for next time */
		pushback = c1;
		return KEY_ESC;
	}
	if (c1 == 'O') {
		int c2 = seq_getc();
		if (c2 >= 'P' && c2 <= 'S')
			return KEY_F1 + (c2 - 'P');
		return KEY_NONE;
	}
	if (c1 != '[')
		return KEY_NONE;
	int c2 = seq_getc();
	switch (c2) {
	case 'A': return KEY_UP;
	case 'B': return KEY_DOWN;
	case 'C': return KEY_RIGHT;
	case 'D': return KEY_LEFT;
	default: break;
	}
	int n = 0;
	while (c2 >= '0' && c2 <= '9') {
		n = n * 10 + (c2 - '0');
		c2 = seq_getc();
	}
	if (c2 == '~')
		return tilde_key(n);
	return KEY_NONE;
}

const char *key_name(int k)
{
	switch (k) {
	case KEY_ENTER: return "Enter";
	case KEY_ESC: return "Esc";
	case KEY_UP: return "Up";
	case KEY_DOWN: return "Down";
	case KEY_LEFT: return "Left";
	case KEY_RIGHT: return "Right";
	case KEY_F1: return "F1";
	case KEY_F8: return "F8";
	default: return "?";
	}
}
