/*
 * machboot
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Boot ROM: find the firmware in flash, copy it into RAM, check it,
 * run it. Must fit in 1 KB (bootrom/link.ld). docs/boot.md.
 *
 * It looks in two places, because the same machboot image is also run
 * from where tinydfu's user alt writes it:
 *
 *   0x050000   the boot slot at 0x000000 + FW_OFFSET (installed)
 *   0x090000   tinydfu's user area at 0x040000 + FW_OFFSET
 *
 * The header (fw/flash.h, fw_header_t; written by tools/mkfw.py):
 * magic "MBFW", the register-map version it needs, length, load
 * address, entry, CRC-32 of the payload, version.
 */

#include <stdint.h>

#define FLASH(a)	(*(volatile uint32_t *)(0x10000000u + (a)))
#define SYS(o)		(*(volatile uint32_t *)(0x30000000u + (o)))
#define TEXT(i)		(*(volatile uint32_t *)(0x20000000u + ((i) << 2)))
#define UART(o)		(*(volatile uint8_t *)(0xF0000000u + (o)))

#define FW_MAGIC	0x5746424Du
#define HWVER		2
#define RAM_TOP		0x14800u	/* 88 KB RAM, 6 KB stack above */

static void uputs(const char *s)
{
	for (; *s; s++) {
		while (!(UART(0x14) & 0x20))
			;
		UART(0) = (uint8_t)*s;
	}
}

static uint32_t crc32(const uint8_t *p, uint32_t n)
{
	uint32_t c = ~0u;
	while (n--) {
		c ^= *p++;
		for (int k = 0; k < 8; k++)
			c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
	}
	return ~c;
}

static int try_base(uint32_t base)
{
	if (FLASH(base) != FW_MAGIC || FLASH(base + 4) != HWVER)
		return 0;
	uint32_t len = FLASH(base + 8);
	uint32_t load = FLASH(base + 12);
	uint32_t entry = FLASH(base + 16);
	uint32_t crc = FLASH(base + 20);
	if (load < 0x400 || load + len > RAM_TOP || entry < load || entry >= load + len)
		return 0;
	/* the payload follows the header, or (word 7 nonzero) is that many
	 * bytes away from it -- below it, for a firmware too big for the
	 * space above (tools/mkslot.py) */
	int32_t at = (int32_t)FLASH(base + 28);
	uint32_t src = base + (at ? (uint32_t)at : 32u);
	volatile uint32_t *dst = (volatile uint32_t *)load;
	for (uint32_t i = 0; i < (len + 3) / 4; i++)
		dst[i] = FLASH(src + i * 4);
	if (crc32((const uint8_t *)load, len) != crc)
		return 0;
	((void (*)(uint32_t))entry)(base);
	return 1;
}

void boot_main(void)
{
	/* the text RAM has no initial contents: blank it first */
	for (int i = 0; i < 80 * 30; i++)
		TEXT(i) = 0;

	/* 1 Mbaud 8N1 */
	UART(0x0C) = 0x83;
	UART(0x00) = 3;
	UART(0x04) = 0;
	UART(0x0C) = 0x03;
	UART(0x08) = 0x07;

	try_base(0x050000);
	try_base(0x090000);

#ifdef MB_SRAM_TEST
	/* JTAG test build: the firmware is already in RAM (docs/testing.md) */
	((void (*)(uint32_t))0x400)(0);
#endif

	static const char msg[] = "machboot: no firmware in flash (0x050000)";
	uputs(msg);
	uputs("\r\n");
	for (int i = 0; i < 80 * 30; i++)
		TEXT(i) = 0x4F20;			/* white on red */
	for (int i = 0; msg[i]; i++)
		TEXT(80 + 2 + i) = 0x4F00 | (uint8_t)msg[i];

	for (;;) {
		SYS(0x10) ^= 1;				/* blink the LED */
		for (volatile int d = 0; d < 2000000; d++)
			;
	}
}
