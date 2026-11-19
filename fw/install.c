/*
 * machboot
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Install a bitstream from the card into the system slot (0x060000).
 *
 *   1. Check: the file starts like an ECP5 bitstream, its VERIFY_ID is
 *      this FPGA, it fits the slot (to 0x100000, the Zeitlos region).
 *   2. Compare, 4 KB at a time, against what the slot holds; compute
 *      the file's CRC-32 on the way. Identical: nothing is written.
 *   3. Erase the slot's FIRST sector -- the slot is now invalid, and a
 *      power cut from here on leaves "did not finish installing", never
 *      a half bitstream that looks whole.
 *   4. Write every other sector that differs, and read each back.
 *   5. Write the first sector last, and read it back.
 *   6. Record name, version, CRC and size in the settings.
 *
 * Only sectors that differ are erased (docs/install.md, "Wear").
 */

#include "install.h"
#include "flash.h"
#include "settings.h"
#include "mb.h"
#include "hw.h"
#include "lib.h"
#include "console.h"

#define SECT 4096u
#define SLOT_SIZE (ZEITLOS_REGION - SYSTEM_SLOT)

uint8_t sector_buf[SECT];		/* also fw/usbd.c's, when no install runs */
#define buf sector_buf
static uint8_t diff[SLOT_SIZE / SECT / 8];

volatile int install_cancel;

static uint8_t buf_get(void *ctx, uint32_t off) { (void)ctx; return buf[off]; }

int same_as_flash(uint32_t addr, const uint8_t *p, uint32_t n)
{
	for (uint32_t i = 0; i < n; i += 4) {
		uint32_t w = flash_rd32(addr + i);
		for (uint32_t k = 0; k < 4 && i + k < n; k++)
			if (p[i + k] != (uint8_t)(w >> (8 * k)))
				return 0;
	}
	/* the rest of the sector must be erased, or it differs too */
	for (uint32_t i = (n + 3) & ~3u; i < SECT; i += 4)
		if (flash_rd32(addr + i) != 0xffffffffu)
			return 0;
	return 1;
}

int write_sector(uint32_t addr, const uint8_t *p, uint32_t n)
{
	if (flash_erase4k(addr))
		return -1;
	for (uint32_t o = 0; o < n; o += 256) {
		uint32_t k = n - o < 256 ? n - o : 256;
		/* an all-0xFF page is already what the erase left */
		uint32_t blank = 1;
		for (uint32_t i = 0; i < k && blank; i++)
			blank = p[o + i] == 0xff;
		if (!blank && flash_program(addr + o, p + o, k))
			return -1;
	}
	return same_as_flash(addr, p, n) ? 0 : -1;
}

const char *install_error(int r)
{
	switch (r) {
	case INST_OK: return "installed";
	case INST_SAME: return "already installed";
	case INST_READ: return "the card could not be read";
	case INST_NOT_BIT: return "the file is not a bitstream";
	case INST_WRONG_FPGA: return "the file is for another FPGA";
	case INST_TOO_BIG: return "the file is too big for the slot";
	case INST_CANCELLED: return "cancelled";
	default: return "the flash could not be written";
	}
}

int install(card_entry_t *ce, void (*progress)(int, const char *))
{
	fat_file_t f;
	slot_info_t si;
	uint32_t size = ce->bit.size;
	uint32_t nsect = (size + SECT - 1) / SECT;
	uint32_t crc = 0, ndiff = 0;
	int r;

	install_cancel = 0;
	if (size > SLOT_SIZE)
		return INST_TOO_BIG;
	fat_open(ce->vol, &ce->bit, &f);

	/* 1 + 2: check, compare, CRC */
	memset(diff, 0, sizeof(diff));
	for (uint32_t s = 0; s < nsect; s++) {
		uint32_t n = size - s * SECT < SECT ? size - s * SECT : SECT;
		if (fat_read(&f, buf, n) != (int)n)
			return INST_READ;
		if (s == 0) {
			bit_parse(buf_get, 0, n, &si);
			if (!si.valid)
				return INST_NOT_BIT;
			if (si.idcode != mb.idcode)
				return INST_WRONG_FPGA;
		}
		crc = mb_crc32(crc, buf, n);
		if (!same_as_flash(SYSTEM_SLOT + s * SECT, buf, n)) {
			diff[s / 8] |= (uint8_t)(1u << (s % 8));
			ndiff++;
		}
		if (progress)
			progress((int)(300 * (s + 1) / nsect), "Checking");
	}

	if (ndiff) {
		con_printf("install: %s, %u bytes, %u of %u sectors differ\n", ce->name, size, ndiff, nsect);

		if (install_cancel)
			return INST_CANCELLED;	/* the last moment nothing is written yet */

		/* 3: invalidate the slot first */
		if (flash_erase4k(SYSTEM_SLOT))
			return INST_FLASH;
		diff[0] |= 1;

		/* 4: everything but the first sector */
		fat_seek0(&f);
		uint32_t done = 0;
		for (uint32_t s = 0; s < nsect; s++) {
			uint32_t n = size - s * SECT < SECT ? size - s * SECT : SECT;
			if (fat_read(&f, buf, n) != (int)n)
				return INST_READ;
			if (s == 0 || !(diff[s / 8] & (1u << (s % 8))))
				continue;
			if (write_sector(SYSTEM_SLOT + s * SECT, buf, n))
				return INST_FLASH;
			done++;
			if (progress)
				progress((int)(300 + 650 * done / ndiff), "Writing");
		}

		/* 5: the first sector, last */
		fat_seek0(&f);
		uint32_t n0 = size < SECT ? size : SECT;
		if (fat_read(&f, buf, n0) != (int)n0)
			return INST_READ;
		if (write_sector(SYSTEM_SLOT, buf, n0))
			return INST_FLASH;
		r = INST_OK;
	} else {
		r = INST_SAME;
	}

	/* 6 */
	settings_set_system(ce->kind == CE_SYSTEM ? ce->name : ce->name, ce->ver, crc, size);
	if (progress)
		progress(1000, "Done");
	return r;
}
