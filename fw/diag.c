/*
 * machboot
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Diagnostics: the menu's own RAM and the flash.
 */

#include "mb.h"
#include "hw.h"
#include "lib.h"
#include "console.h"
#include "sd.h"
#include "card.h"
#include "fat.h"

/*
 * The test covers the menu's free RAM: from the end of the firmware's
 * data to 0x14800, where the 6 KB the stack may use begin (fw/link.ld).
 * Nothing lives there, so the test can write all of it.
 */
#ifdef MB_HOST
#define TEST_WORDS 4096
static volatile uint32_t test_area[TEST_WORDS];
#define testbuf test_area
static uint32_t test_words(void) { return TEST_WORDS; }
#else
extern char _end[];
#define testbuf ((volatile uint32_t *)(((uintptr_t)_end + 3) & ~(uintptr_t)3))
static uint32_t test_words(void) { return (0x14800u - (uint32_t)(uintptr_t)testbuf) / 4; }
#endif

/*
 * March C- (van de Goor), over 32-bit words with an all-zeros/all-ones
 * background, then a walking-ones data-line pass and an address-line
 * pass (each word holds its own address).
 *
 *   up(w0); up(r0,w1); up(r1,w0); down(r0,w1); down(r1,w0); up(r0)
 */
int diag_memory_run(void (*progress)(int permille))
{
	const uint32_t TEST_WORDS_N = test_words();
	int errs = 0;
	const int steps = 8;
	int step = 0;
	uint32_t i;

#define STEP() do { if (progress) progress(++step * 1000 / steps); } while (0)

	for (i = 0; i < TEST_WORDS_N; i++) testbuf[i] = 0;
	STEP();
	for (i = 0; i < TEST_WORDS_N; i++) { if (testbuf[i] != 0) errs++; testbuf[i] = ~0u; }
	STEP();
	for (i = 0; i < TEST_WORDS_N; i++) { if (testbuf[i] != ~0u) errs++; testbuf[i] = 0; }
	STEP();
	for (i = TEST_WORDS_N; i-- > 0;) { if (testbuf[i] != 0) errs++; testbuf[i] = ~0u; }
	STEP();
	for (i = TEST_WORDS_N; i-- > 0;) { if (testbuf[i] != ~0u) errs++; testbuf[i] = 0; }
	STEP();
	for (i = 0; i < TEST_WORDS_N; i++) if (testbuf[i] != 0) errs++;
	STEP();

	/* data lines */
	for (int b = 0; b < 32; b++) {
		testbuf[0] = 1u << b;
		if (testbuf[0] != (1u << b)) errs++;
	}
	STEP();

	/* address lines */
	for (i = 0; i < TEST_WORDS_N; i++) testbuf[i] = (uint32_t)(uintptr_t)&testbuf[i];
	for (i = 0; i < TEST_WORDS_N; i++) if (testbuf[i] != (uint32_t)(uintptr_t)&testbuf[i]) errs++;
	STEP();

#undef STEP
	return errs;
}

/* read 64 KB of the boot slot through the window, twice, and compare */
int diag_flash_run(uint32_t *kbps)
{
	uint32_t t0 = ms_now();
	uint32_t a = 0, b = 0;
	for (uint32_t off = 0; off < 0x10000; off += 4)
		a = a * 31 + flash_rd32(BOOT_SLOT + off);
	uint32_t el = ms_now() - t0;
	for (uint32_t off = 0; off < 0x10000; off += 4)
		b = b * 31 + flash_rd32(BOOT_SLOT + off);
	*kbps = el ? 64000 / el : 0;
	flash_info_t fi;
	flash_identify(&fi);
	return a == b && fi.jedec != 0 && fi.jedec != 0xffffff;
}

/* read 1 MB from the card (2048 sectors from 0); 1: no card, -1: error */
int diag_sd_run(uint32_t *kbps)
{
	uint8_t *s = fat_scratch();
	if (!card.sd_ok && sd_init())
		return 1;
	card.sd_ok = 1;
	uint32_t n = sd.sectors && sd.sectors < 2048 ? sd.sectors : 2048;
	uint32_t t0 = ms_now();
	for (uint32_t i = 0; i < n; i++)
		if (sd_read(i, s))
			return -1;
	uint32_t el = ms_now() - t0;
	*kbps = el ? n / 2 * 1000 / el : 0;
	return 0;
}
