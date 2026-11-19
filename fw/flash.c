/*
 * machboot
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * -- What an ECP5 bitstream looks like at its start --
 *
 * From Lattice TN1260 (ECP5 sysCONFIG), "Bitstream Format" and the
 * configuration command set:
 *
 *   - optionally, a .bit file header: 0xFF 0x00, then NUL-terminated
 *     comment strings ("Part: LFE5U-25F-6CABGA256", ...), ended by 0xFF.
 *     The FPGA ignores it; openFPGALoader strips it, dfu-util does not.
 *   - dummy bytes 0xFF, then the 16-bit preamble 0xBDB3;
 *   - commands, each an opcode byte and three operand bytes. Among the
 *     first is VERIFY_ID (0xE2) with operands 00 00 00, followed by the
 *     32-bit IDCODE (big-endian) the bitstream was built for. The FPGA
 *     refuses a bitstream whose IDCODE is not its own.
 *
 * So a slot is "valid" when the preamble appears within its first 4 KB,
 * and it is "for this FPGA" when VERIFY_ID's IDCODE matches.
 */

#include "flash.h"
#include "hw.h"
#include "lib.h"

void flash_identify(flash_info_t *fi)
{
	/* the controller reads the ID at reset; wait until it has */
	for (int i = 0; i < 100000 && (flash_reg(FL_STATUS) & 1); i++)
		;
	fi->jedec = flash_reg(FL_ID) & 0x00ffffffu;
	uint8_t cap = fi->jedec & 0xff;
	fi->size = (cap >= 0x10 && cap <= 0x20) ? (1u << cap) : 0;
}

static uint32_t flash_wait(void)
{
	uint32_t st;
	while ((st = flash_reg(FL_STATUS)) & FL_ST_BUSY)
		;
	return st;
}

/*
 * The controller (rtl/zeitlos/spiflash.v, Zeitlos docs/spiflash.md)
 * needs ARM before each erase or program, and refuses anything below
 * the fence (0x060000) except the settings window (0x05E000-0x05FFFF).
 */
int flash_erase4k(uint32_t addr)
{
	flash_wait();
	flash_reg_wr(FL_ADDR, addr & ~0xfffu);
	flash_reg_wr(FL_ARM, FL_ARM_KEY);
	flash_reg_wr(FL_CMD, 1);
	uint32_t st = flash_wait();
	return (st & FL_ST_REFUSED) || !(st & FL_ST_DONE) ? -1 : 0;
}

int flash_program(uint32_t addr, const uint8_t *p, uint32_t n)
{
	if (n == 0 || n > 256 || (addr & 0xff) + n > 256)
		return -1;
	flash_wait();
	for (uint32_t i = 0; i < n; i += 4) {
		uint32_t w = 0xffffffffu;
		for (uint32_t k = 0; k < 4 && i + k < n; k++)
			w = (w & ~(0xffu << (8 * k))) | ((uint32_t)p[i + k] << (8 * k));
		flash_reg_wr(FL_PBUF + i, w);
	}
	flash_reg_wr(FL_ADDR, addr);
	flash_reg_wr(FL_LEN, n);
	flash_reg_wr(FL_ARM, FL_ARM_KEY);
	flash_reg_wr(FL_CMD, 2);
	uint32_t st = flash_wait();
	return (st & FL_ST_REFUSED) || !(st & FL_ST_DONE) ? -1 : 0;
}

/* the start of a bitstream, from any source of bytes */
void bit_parse(uint8_t (*get)(void *ctx, uint32_t off), void *ctx, uint32_t limit, slot_info_t *si)
{
	memset(si, 0, sizeof(*si));

	uint32_t p = 0;
	uint32_t end = limit < 4096 ? limit : 4096;

	/* a .bit header? */
	if (end > 2 && get(ctx, p) == 0xFF && get(ctx, p + 1) == 0x00) {
		si->has_header = 1;
		p += 2;
		while (p < end) {
			uint8_t c = get(ctx, p);
			if (c == 0xFF)
				break;
			/* one comment string */
			char s[48];
			int n = 0;
			while (p < end && (c = get(ctx, p)) != 0 && c != 0xFF) {
				if (n < (int)sizeof(s) - 1)
					s[n++] = (char)c;
				p++;
			}
			s[n] = 0;
			if (c == 0)
				p++;
			if (n > 6 && memcmp(s, "Part: ", 6) == 0) {
				int k = 0;
				for (const char *q = s + 6; *q && k < (int)sizeof(si->part) - 1; q++)
					si->part[k++] = *q;
				si->part[k] = 0;
			}
		}
	}

	/* the preamble */
	uint8_t prev = 0;
	for (; p < end; p++) {
		uint8_t c = get(ctx, p);
		if (prev == 0xBD && c == 0xB3) {
			si->valid = 1;
			si->preamble = p - 1;
			p++;
			break;
		}
		prev = c;
	}
	if (!si->valid)
		return;

	/* VERIFY_ID among the first commands */
	for (uint32_t q = p; q < p + 128 && q + 8 < limit; q++) {
		if (get(ctx, q) == 0xE2 && get(ctx, q + 1) == 0 &&
		    get(ctx, q + 2) == 0 && get(ctx, q + 3) == 0) {
			si->idcode = (uint32_t)get(ctx, q + 4) << 24 |
			             (uint32_t)get(ctx, q + 5) << 16 |
			             (uint32_t)get(ctx, q + 6) << 8 |
			             (uint32_t)get(ctx, q + 7);
			break;
		}
	}
}

static uint8_t flash_get(void *ctx, uint32_t off)
{
	return flash_rd8(*(uint32_t *)ctx + off);
}

void flash_slot(uint32_t addr, slot_info_t *si)
{
	bit_parse(flash_get, &addr, 4096 + 256, si);
	if (si->valid)
		si->preamble += addr;
}

const char *die_name(uint32_t idcode)
{
	switch (idcode) {
	case 0x21111043: return "ECP5-12F";
	case 0x41111043: return "ECP5-25F";
	case 0x41112043: return "ECP5-45F";
	case 0x41113043: return "ECP5-85F";
	default: return "unknown FPGA";
	}
}
