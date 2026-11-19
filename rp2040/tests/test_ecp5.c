/*
 * machboot supervisor
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * src/ecp5.c on a PC, against a model of an ECP5's JTAG configuration
 * logic: the IEEE 1149.1 TAP, IDCODE, ISC_ENABLE (ISC_EN), ISC_ERASE
 * (busy for a while), LSC_INIT_ADDRESS, LSC_BITSTREAM_BURST (the bits
 * collected and turned back into bytes), LSC_READ_STATUS, LSC_CHECK_BUSY,
 * ISC_DISABLE (DONE, if what came in was the bitstream, whole and in
 * order).
 *
 *   cc -Irp2040/src rp2040/tests/test_ecp5.c rp2040/src/ecp5.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "ecp5.h"
#include "jtag_io.h"
#include "djtag.h"

#include "ecp5_model.h"

/* the cell after the burst, with each byte's bits back in file order */
static int burst_is(const uint8_t *bit, uint32_t len)
{
	if (burst_bits != len * 8) return 0;
	for (uint32_t i = 0; i < len; i++) {
		uint8_t b = 0;
		for (int k = 0; k < 8; k++)		/* sent MSB first: bit k of the stream is bit 7-k */
			if (burst[i] & (1 << k)) b |= (uint8_t)(0x80 >> k);
		if (b != bit[i]) return 0;
	}
	return 1;
}

static int failures;
static void check(int c, const char *what) { printf("%s %s\n", c ? "ok  " : "FAIL", what); failures += !c; }

static int yields, abort_at;
static int yield(void) { return ++yields == abort_at; }

int main(void)
{
	uint32_t len = 300 * 1024 + 123;		/* not a multiple of the chunk */
	uint8_t *bit = malloc(len);
	for (uint32_t i = 0; i < len; i++) bit[i] = (uint8_t)(i * 37 + (i >> 8));
	bit[0] = 0xFF; bit[1] = 0xFF; bit[2] = 0xBD; bit[3] = 0xB3;

	jio_init();
	check(ecp5_idcode() == 0x41112043u, "IDCODE read: LFE5U-45F");

	check(ecp5_load(bit, len, 0x41111043u, 0) == ECP5_ERR_IDCODE, "a 25F bitstream on a 45F: refused before anything else");
	check(!isc_en && !burst_bits, "  and nothing was done");

	int r = ecp5_load(bit, len, 0x41112043u, yield);
	check(r == ECP5_OK, "a 300 KB bitstream: loaded");
	check(burst_is(bit, len), "  the FPGA got it byte for byte, each byte MSB first");
	check(!wrong_order, "  ISC_ENABLE, ISC_ERASE, LSC_INIT_ADDRESS, then the burst");
	check(done && !isc_en, "  ISC_DISABLE: DONE, configuration mode left");
	check(yields == (int)(len / 4096), "  yielded between every 4 KB chunk");
	printf("     (%llu TCK pulses for %u bytes: %.2f per bit)\n", (unsigned long long)pulses, len,
	       (double)pulses / (len * 8.0));

	/* the PC wants JTAG halfway: stopped, the TAP reset, nothing claimed */
	yields = 0; abort_at = 20; done = 0;
	r = ecp5_load(bit, len, 0x41112043u, yield);
	check(r == ECP5_ABORTED, "the PC wants JTAG during a load: it stops");
	check(st == RTI || st == TLR, "  the TAP is back in Test-Logic-Reset/Run-Test/Idle");
	check(!done, "  and the FPGA is not configured");

	/* a configuration error after the burst */
	abort_at = -1;
	fail_cnf = 1;
	r = ecp5_load(bit, len, 0x41112043u, 0);
	check(r == ECP5_ERR_CONFIG, "the FPGA rejects the bitstream: reported");
	fail_cnf = 0;

	printf(failures ? "FAILED\n" : "all passed\n");
	return failures != 0;
}
