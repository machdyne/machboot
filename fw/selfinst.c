/*
 * machboot
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Install mode (docs/install-mode.md): copy the payload -- the boot-slot
 * image -- from 0x0A0000 to 0x000000, replacing tinydfu.
 *
 * Only the installer build can do this: its flash fence is turned
 * around (rtl/mb_soc.v, INSTALLER), so 0x000000-0x0FFFFF can be
 * written and the Zeitlos region cannot. The normal build's fence
 * refuses every write below 0x05E000, whatever the firmware does.
 *
 *   1. The payload's CRC-32 must match the header. Nothing is written
 *      before that.
 *   2. Sectors 0x05F000 down to 0x000000, in that order, each compared
 *      first and written only if it differs, then read back. Going down
 *      keeps tinydfu (at the bottom) whole for as long as possible: a
 *      power cut before its sectors are reached leaves tinydfu working,
 *      and dfu-util can write the installer again.
 *   3. The whole boot slot is checked against the CRC again.
 *   4. The installer's own copy is made invisible: the first sector of
 *      the system slot (0x060000, inside the installer's bitstream) and
 *      the installer's firmware header (0x090000) are erased, so the
 *      new machboot neither starts the installer as a "system" nor
 *      finds its firmware there.
 *
 * A failure leaves the firmware running from RAM: the page offers to try
 * again, which picks up where it stopped (compare first).
 */

#include "selfinst.h"
#include "flash.h"
#include "hw.h"
#include "lib.h"
#include "mb.h"
#include "console.h"

#define SECT 4096u

selfinst_t si;

void selfinst_probe(void)
{
	memset(&si, 0, sizeof(si));
	if (!mb.install_mode || !(mb.features & FEAT_INSTALLER))
		return;

	uint32_t base = mb.fw_base - FW_OFFSET;
	uint32_t *w = (uint32_t *)&si.h;
	for (uint32_t i = 0; i < sizeof(si.h) / 4; i++)
		w[i] = flash_rd32(base + SI_HEADER_OFFSET + 4 * i);

	si.state = SI_BAD;
	if (si.h.magic != SI_MAGIC || si.h.format != 1)
		return;
	if (si.h.payload_len < 0x1000 || si.h.payload_len > SETTINGS_AT ||
	    si.h.payload_off < BOOT_SLOT_SIZE || si.h.payload_off + si.h.payload_len > ZEITLOS_REGION - base)
		return;
	if (si.h.idcode != mb.idcode)
		return;
	si.src = base + si.h.payload_off;

	/* it must look like a boot slot: a bitstream for this FPGA, our firmware */
	slot_info_t s;
	flash_slot(si.src, &s);
	if (!s.valid || (s.idcode && s.idcode != mb.idcode))
		return;
	if (flash_rd32(si.src + FW_OFFSET) != FW_MAGIC)
		return;
	si.state = SI_READY;
}

static uint32_t crc_range(uint32_t addr, uint32_t len, void (*progress)(int, const char *), int p0, int p1)
{
	uint8_t b[256];
	uint32_t crc = 0;
	for (uint32_t o = 0; o < len; o += sizeof(b)) {
		uint32_t n = len - o < sizeof(b) ? len - o : sizeof(b);
		for (uint32_t i = 0; i < n; i += 4) {
			uint32_t v = flash_rd32(addr + o + i);
			b[i] = (uint8_t)v; b[i + 1] = (uint8_t)(v >> 8);
			b[i + 2] = (uint8_t)(v >> 16); b[i + 3] = (uint8_t)(v >> 24);
		}
		crc = mb_crc32(crc, b, n);
		if (progress && (o & 0xffff) == 0)
			progress(p0 + (int)((uint32_t)(p1 - p0) * (o >> 8) / (len >> 8)), "Checking");
	}
	return crc;
}

/* the word the boot slot should hold at off */
static uint32_t want32(uint32_t off)
{
	if (off >= si.h.payload_len)
		return 0xffffffffu;
	uint32_t v = flash_rd32(si.src + off);
	if (off + 4 > si.h.payload_len) {
		/* the last word: bytes past the end are erased */
		uint32_t keep = si.h.payload_len - off;
		v |= 0xffffffffu << (8 * keep);
	}
	return v;
}

static int sector_ok(uint32_t off)
{
	for (uint32_t i = 0; i < SECT; i += 4)
		if (flash_rd32(BOOT_SLOT + off + i) != want32(off + i))
			return 0;
	return 1;
}

static int write_sector(uint32_t off)
{
	uint8_t page[256];
	if (flash_erase4k(BOOT_SLOT + off))
		return -1;
	for (uint32_t p = 0; p < SECT; p += 256) {
		int blank = 1;
		for (uint32_t i = 0; i < 256; i += 4) {
			uint32_t v = want32(off + p + i);
			page[i] = (uint8_t)v; page[i + 1] = (uint8_t)(v >> 8);
			page[i + 2] = (uint8_t)(v >> 16); page[i + 3] = (uint8_t)(v >> 24);
			if (v != 0xffffffffu)
				blank = 0;
		}
		if (!blank && flash_program(BOOT_SLOT + off + p, page, 256))
			return -1;
	}
	return sector_ok(off) ? 0 : -1;
}

int selfinst_run(void (*progress)(int permille, const char *what))
{
	if (si.state != SI_READY)
		return SI_ERR_IMAGE;

	/* 1 */
	if (crc_range(si.src, si.h.payload_len, progress, 0, 100) != si.h.payload_crc) {
		con_puts("install mode: the payload's CRC does not match; nothing written\n");
		si.state = SI_BAD;
		return SI_ERR_IMAGE;
	}

	/* 2: from the top of the boot slot down */
	uint32_t nsect = BOOT_SLOT_SIZE / SECT, written = 0;
	for (uint32_t k = 0; k < nsect; k++) {
		uint32_t off = (nsect - 1 - k) * SECT;
		if (!sector_ok(off)) {
			int r = write_sector(off);
			if (r)
				r = write_sector(off);		/* once more */
			if (r) {
				si.fail_at = BOOT_SLOT + off;
				con_printf("install mode: writing 0x%06x failed\n", si.fail_at);
				return SI_ERR_FLASH;
			}
			written++;
		}
		if (progress)
			progress(100 + (int)(800 * (k + 1) / nsect), "Installing machboot");
	}

	/* 3 */
	if (crc_range(BOOT_SLOT, si.h.payload_len, progress, 900, 980) != si.h.payload_crc) {
		si.fail_at = BOOT_SLOT;
		return SI_ERR_FLASH;
	}

	/* 4 */
	uint32_t base = mb.fw_base - FW_OFFSET;
	if (flash_erase4k(SYSTEM_SLOT) || flash_erase4k(base + FW_OFFSET) ||
	    flash_erase4k(base + SI_HEADER_OFFSET)) {
		si.fail_at = SYSTEM_SLOT;
		return SI_ERR_FLASH;
	}
	con_printf("install mode: machboot installed at 0x000000 (%u of %u sectors written)\n", written, nsect);
	if (progress)
		progress(1000, "Done");
	return SI_OK;
}
