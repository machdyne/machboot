/*
 * machboot
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The firmware on a PC: fw/hw.h's functions backed by arrays, a flash
 * image file, a virtual millisecond clock, and a script of key presses.
 * Used by tests/ to drive the menu and save its screens.
 *
 *   mbhost FLASH.bin SCRIPT [OUTDIR [CARD.img]]
 *
 * The flash is written back to FLASH.bin.out at the end if the firmware
 * wrote to it (installs, settings), so a test can carry on from it.
 *
 * SCRIPT is whitespace-separated tokens, run in order:
 *
 *   up down left right enter esc tab f1 f2 f8 del   a key press
 *   'x                                              the character x
 *   wait:N                                          N ms pass, no key
 *   snap:NAME                                       save the screen as OUTDIR/NAME.cells
 *   board:N idcode:HEX fwbase:HEX                   machine setup (before the first key)
 *   sechzig                                         a Sergei/Mozart SRAM build: system at 0,
 *                                                   the whole flash locked, firmware in RAM
 *   ports:N                                         USB host ports (Konfekt: 1)
 *   xl:...                                          the RP2040 on the XA-XD link: fw/host/xlink_host.c
 *   installer                                       the installer gateware: the fence protects
 *                                                   0x100000 up instead of the boot slot
 *   usb:kbd  usb:mouse  usb:unplug                  plug a device into USB port 1 (or remove it)
 *   u:NAME                                          a key on the USB keyboard: up down left right
 *                                                   enter esc f1 f2 f5 f8, or a letter
 *   usbd                                            the gateware has the USB device (setup)
 *   ud:...                                          the USB host on the USB-C port: fw/host/usbd_host.c
 *   cut:N                                           the power fails during the Nth flash write
 *                                                   from now (the flash is saved as it was)
 *
 * At the end of the script, or on a RECONFIG write, it exits.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../hw.h"
#include "../../tests/sdmodel.h"

int main_fw(uint32_t fw_base);

static uint8_t *flash;
static size_t flash_size;
static uint16_t cells[30 * 80];
static uint32_t now_ms;
static uint32_t reads, lsr_reads;
static uint32_t board = 1, idcode = 0x41111043, fwbase = 0x050000;
static int installer;
static int sechzig;		/* a Sechzig SRAM build: system at 0, the whole flash locked */
static int usb_roots = 2;	/* the host controller's ports (Konfekt: 1) */
extern int ud_present;
extern int xl_present;
void xl_script(const char *cmd);
int xl_busy(void);
void ud_script(const char *cmd);
int ud_busy(void);
static const char *outdir = ".";
static const char *flash_path;
static int flash_written;
static long cut_after = -1;		/* flash operations until the power fails */

static void save_flash(void);
static void usb_model_report(void);
static void usb_script(const char *what);
static void usb_key(const char *k);

/* pending input bytes */
static uint8_t inbuf[256];
static int in_head, in_tail;

static char **tok;
static int ntok, itok;
static uint32_t wait_until;

static void snap(const char *name)
{
	char path[512];
	snprintf(path, sizeof(path), "%s/%s.cells", outdir, name);
	FILE *f = fopen(path, "wb");
	if (!f) { perror(path); exit(1); }
	fwrite(cells, sizeof(cells), 1, f);
	fclose(f);
	fprintf(stderr, "[snap %s]\n", name);
}

static void push(const char *s)
{
	while (*s)
		inbuf[in_tail++ & 255] = (uint8_t)*s++;
}

/* feed the next script step when the input is empty and time allows */
static void script_step(void)
{
	if (in_head != in_tail || now_ms < wait_until)
		return;
	if (itok >= ntok) {
		fprintf(stderr, "[end of script]\n");
		save_flash();
		exit(0);
	}
	const char *t = tok[itok++];
	if (!strncmp(t, "wait:", 5)) { wait_until = now_ms + (uint32_t)atoi(t + 5); return; }
	if (!strncmp(t, "snap:", 5)) { snap(t + 5); return; }
	if (!strncmp(t, "cut:", 4)) { cut_after = atol(t + 4); return; }
	if (!strncmp(t, "usb:", 4)) { usb_script(t + 4); return; }
	if (!strcmp(t, "ud:wait")) { if (ud_busy()) itok--; return; }
	if (!strncmp(t, "ud:", 3)) { ud_script(t + 3); return; }
	if (!strcmp(t, "xl:wait")) { if (xl_busy()) itok--; return; }
	if (!strncmp(t, "xl:", 3)) { xl_script(t + 3); return; }
	if (!strncmp(t, "u:", 2)) { usb_key(t + 2); wait_until = now_ms + 60; return; }
	if (!strcmp(t, "up")) push("\x1b[A");
	else if (!strcmp(t, "down")) push("\x1b[B");
	else if (!strcmp(t, "right")) push("\x1b[C");
	else if (!strcmp(t, "left")) push("\x1b[D");
	else if (!strcmp(t, "enter")) push("\r");
	else if (!strcmp(t, "esc")) { push("\x1b"); wait_until = now_ms + 50; return; }
	else if (!strcmp(t, "tab")) push("\t");
	else if (!strcmp(t, "f1")) push("\x1bOP");
	else if (!strcmp(t, "f2")) push("\x1bOQ");
	else if (!strcmp(t, "f3")) push("\x1bOR");
	else if (!strcmp(t, "f4")) push("\x1bOS");
	else if (!strcmp(t, "f5")) push("\x1b[15~");
	else if (!strcmp(t, "f8")) push("\x1b[19~");
	else if (!strcmp(t, "del")) push("\x1b[3~");
	else if (t[0] == '\'') { char s[2] = { t[1], 0 }; push(s); }
	else { fprintf(stderr, "unknown script token %s\n", t); exit(2); }
	/* the menu redraws after every key: give it a moment before the next */
	wait_until = now_ms + 10;
}

uint32_t sys_rd(uint32_t off)
{
	switch (off) {
	case SYS_MAGIC: return SYS_MAGIC_VAL;
	case SYS_HWVER: return MB_HWVER;
	case SYS_MS:
		/* time moves as the firmware looks at it */
		if (++reads % 4 == 0)
			now_ms++;
		script_step();
		return now_ms;
	case SYS_CYCLES: return now_ms * 48000;
	case SYS_BOARD: return board;
	case SYS_FEATURES: return FEAT_VIDEO | FEAT_USBH | FEAT_SD | FEAT_PROGN | (installer ? FEAT_INSTALLER : 0) | (ud_present ? FEAT_USBD : 0) | (xl_present ? FEAT_RPLINK : 0);
	case SYS_IDCODE: return idcode;
	case SYS_SLOT: return sechzig ? 0 : 0x060000;
	case SYS_RECONFIG: return 0x5A520001;
	default: return 0;
	}
}

void sys_wr(uint32_t off, uint32_t v)
{
	if (off == SYS_RECONFIG && v == RECONFIG_KEY) {
		fprintf(stderr, "[RECONFIG: the FPGA would load 0x%06x]\n", (installer || sechzig) ? 0 : 0x060000);
		snap("reconfig");
		save_flash();
		exit(0);
	}
}

void text_wr(int idx, uint16_t cell)
{
	if (idx >= 0 && idx < 30 * 80)
		cells[idx] = cell;
}

uint8_t flash_rd8(uint32_t a) { return a < flash_size ? flash[a] : 0xff; }
uint32_t flash_rd32(uint32_t a)
{
	return (uint32_t)flash_rd8(a) | (uint32_t)flash_rd8(a + 1) << 8 |
	       (uint32_t)flash_rd8(a + 2) << 16 | (uint32_t)flash_rd8(a + 3) << 24;
}
/* -- the flash controller's write side: rtl/zeitlos/spiflash.v's rules -- */

#define LOCK_END  0x060000u
#define WIN_START 0x05E000u
#define WIN_END   0x060000u

static uint32_t fl_addr, fl_len, fl_status, fl_armed;
static uint8_t fl_pbuf[256];

static uint8_t *card_img;
static size_t card_size;
static const char *card_path;

static void save_flash(void)
{
	usb_model_report();
	/* the card too, if the card reader wrote to it: CARD.out */
	if (card_img && card_path && sdm_writes()) {
		char cp[512];
		snprintf(cp, sizeof(cp), "%s.out", card_path);
		FILE *cf = fopen(cp, "wb");
		fwrite(card_img, 1, card_size, cf);
		fclose(cf);
	}
	if (!flash_written || !flash_path)
		return;
	char p[512];
	snprintf(p, sizeof(p), "%s.out", flash_path);
	FILE *f = fopen(p, "wb");
	fwrite(flash, 1, flash_size, f);
	fclose(f);
}

static void power_cut_check(void)
{
	if (cut_after >= 0 && cut_after-- == 0) {
		fprintf(stderr, "[POWER CUT]\n");
		save_flash();
		exit(0);
	}
}

static int locked(uint32_t a)
{
	if (installer)
		return a >= 0x100000u;
	if (sechzig)
		return 1;
	return a < LOCK_END && !(a >= WIN_START && a < WIN_END);
}

uint32_t flash_reg(uint32_t off)
{
	if (off == FL_ID)
		return flash_size >= (4u << 20) ? 0xEF4016 : 0xEF4015;
	if (off == FL_STATUS)
		return fl_status;
	if (off == FL_LOCK)
		return (installer || sechzig) ? 0 : LOCK_END;
	if (off == FL_LOCK_FROM)
		return installer ? 0x100000u : sechzig ? 0 : 0x1000000u;
	return 0;
}

void flash_reg_wr(uint32_t off, uint32_t v)
{
	if (off >= FL_PBUF && off < FL_PBUF + 256) {
		for (int k = 0; k < 4; k++)
			fl_pbuf[off - FL_PBUF + k] = (uint8_t)(v >> (8 * k));
		return;
	}
	switch (off) {
	case FL_ADDR: fl_addr = v & 0xffffff; break;
	case FL_LEN: fl_len = v; break;
	case FL_ARM: fl_armed = (v == FL_ARM_KEY); break;
	case FL_CMD: {
		uint32_t target = (v == 1) ? (fl_addr & ~0xfffu) : fl_addr;
		fl_status = 0;
		if (!fl_armed) { fl_status = 0x08; break; }
		fl_armed = 0;
		if (locked(target)) { fl_status = 0x04; break; }
		if (v == 1) {
			power_cut_check();
			if (target + 4096 <= flash_size)
				memset(flash + target, 0xff, 4096);
			flash_written = 1;
			fl_status = FL_ST_DONE;
		} else if (v == 2) {
			if (fl_len == 0 || fl_len > 256 || (fl_addr & 0xff) + fl_len > 256) { fl_status = 0x10; break; }
			power_cut_check();
			for (uint32_t i = 0; i < fl_len && fl_addr + i < flash_size; i++)
				flash[fl_addr + i] &= fl_pbuf[i];	/* NOR: programming only clears bits */
			flash_written = 1;
			fl_status = FL_ST_DONE;
		}
		break;
	}
	}
}

/* -- SD card: the SPI master in front of tests/sdmodel.c -- */

static uint32_t sd_ctrl = 59u << 8, sd_rx;

uint32_t sd_rd(uint32_t off)
{
	if (off == SD_MAGIC) return SD_MAGIC_VAL;
	if (off == SD_DATA) return sd_rx;
	if (off == SD_CTRL) return sd_ctrl;
	return 0;
}

void sd_wr(uint32_t off, uint32_t v)
{
	if (off == SD_CTRL) {
		if ((sd_ctrl & SD_CTRL_CS) && !(v & SD_CTRL_CS))
			sdm_deselect();
		sd_ctrl = v;
		return;
	}
	if (off != SD_DATA)
		return;
	int sel = (sd_ctrl & SD_CTRL_CS) && card_img;
	if (sd_ctrl & SD_CTRL_X32) {
		sd_rx = 0;
		for (int k = 0; k < 4; k++) {
			uint8_t b = sel ? sdm_xchg((uint8_t)(v >> (8 * k))) : 0xff;
			sd_rx |= (uint32_t)b << (8 * k);
		}
	} else {
		sd_rx = sel ? sdm_xchg((uint8_t)v) : 0xff;
	}
}

#include "usbh_model.inc"

uint8_t uart_reg_rd(uint32_t off)
{
	if (off == UART_LSR) {
		/* a polling loop also lets time pass -- slowly, so that printing
		   (one LSR read per character) does not outrun the script */
		if (++lsr_reads % 64 == 0)
			now_ms++;
		script_step();
		return 0x20 | (in_head != in_tail ? 1 : 0);
	}
	if (off == UART_DATA && in_head != in_tail)
		return inbuf[in_head++ & 255];
	return 0;
}

void uart_reg_wr(uint32_t off, uint8_t v)
{
	if (off == UART_DATA)
		if (v != '\r') fputc(v, stdout);
}

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: mbhost FLASH.bin SCRIPT [OUTDIR]\n");
		return 2;
	}
	FILE *f = fopen(argv[1], "rb");
	if (!f) { perror(argv[1]); return 1; }
	fseek(f, 0, SEEK_END);
	flash_size = (size_t)ftell(f);
	fseek(f, 0, SEEK_SET);
	flash = malloc(flash_size);
	if (fread(flash, 1, flash_size, f) != flash_size) return 1;
	fclose(f);
	if (argc > 3)
		outdir = argv[3];
	flash_path = argv[1];
	if (argc > 4) {
		FILE *c = fopen(argv[4], "rb");
		if (!c) { perror(argv[4]); return 1; }
		card_path = argv[4];
		fseek(c, 0, SEEK_END);
		card_size = (size_t)ftell(c);
		fseek(c, 0, SEEK_SET);
		card_img = malloc(card_size);
		if (fread(card_img, 1, card_size, c) != card_size) return 1;
		fclose(c);
		sdm_init(card_img, card_size, 1);
	}

	/* the script: argv[2] is a file */
	f = fopen(argv[2], "rb");
	if (!f) { perror(argv[2]); return 1; }
	static char text[65536];
	size_t n = fread(text, 1, sizeof(text) - 1, f);
	text[n] = 0;
	fclose(f);
	/* comments: '#' to the end of the line */
	for (char *c = text; *c; c++)
		if (*c == '#')
			while (*c && *c != '\n')
				*c++ = ' ';
	tok = malloc(sizeof(char *) * 4096);
	for (char *p = strtok(text, " \t\r\n"); p; p = strtok(NULL, " \t\r\n")) {
		tok[ntok++] = p;
	}
	/* setup tokens first */
	while (itok < ntok) {
		const char *t = tok[itok];
		if (!strncmp(t, "board:", 6)) board = (uint32_t)strtoul(t + 6, 0, 0);
		else if (!strncmp(t, "idcode:", 7)) idcode = (uint32_t)strtoul(t + 7, 0, 16);
		else if (!strncmp(t, "fwbase:", 7)) fwbase = (uint32_t)strtoul(t + 7, 0, 16);
		else if (!strcmp(t, "installer")) installer = 1;
		else if (!strcmp(t, "sechzig")) { sechzig = 1; fwbase = 0; xl_present = 1; }
		else if (!strncmp(t, "ports:", 6)) usb_roots = atoi(t + 6);
		else if (!strcmp(t, "usbd")) ud_present = 1;
		else break;
		itok++;
	}
	return main_fw(fwbase);
}
