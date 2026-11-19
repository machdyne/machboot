/*
 * machboot supervisor
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * ECP5 SRAM configuration over JTAG: ecp5.h.
 *
 * TAP helpers on jtag_io.h. jio_shift() sends each byte MSB first (the
 * dirtyJTAG convention); JTAG registers go LSB first, so register
 * values are bit-reversed into bytes, while the bitstream -- which the
 * ECP5 wants each byte MSB first -- goes out as the file has it.
 */

#include <string.h>
#include "ecp5.h"
#include "jtag_io.h"
#include "djtag.h"

#define IDCODE_PUB		0xE0
/* about 20 ms of TCK at 6 MHz: before release, and after it */
#define ECP5_SETTLE_CLKS	120000u
#define PRELOAD			0x1C	/* PRELOAD/SAMPLE */
#define ISC_ENABLE		0xC6
#define ISC_ERASE		0x0E
#define ISC_DISABLE		0x26
#define LSC_INIT_ADDRESS	0x46
#define LSC_BITSTREAM_BURST	0x7A
#define LSC_READ_STATUS		0x3C
#define LSC_CHECK_BUSY		0xF0
#define BYPASS			0xFF

#define ST_DONE			(1u << 8)
#define ST_ISC_EN		(1u << 9)
#define ST_BUSY			(1u << 12)
#define ST_FAIL			(1u << 13)
#define ST_CNF_CHK		(0x0fu << 23)	/* configuration check: 0 = no error */

#define CHUNK			4096		/* bytes of burst between yields */

static uint8_t rev8(uint8_t b)
{
	b = (uint8_t)((b & 0xF0) >> 4 | (b & 0x0F) << 4);
	b = (uint8_t)((b & 0xCC) >> 2 | (b & 0x33) << 2);
	b = (uint8_t)((b & 0xAA) >> 1 | (b & 0x55) << 1);
	return b;
}

/* TMS sequences, from any state through Test-Logic-Reset where needed */
static void tms_bits(uint32_t bits, int n)
{
	for (int i = 0; i < n; i++)
		jio_clk((bits >> i) & 1, 0, 1);
}

static void tap_reset(void)
{
	tms_bits(0x1f, 5);		/* Test-Logic-Reset */
	tms_bits(0x0, 1);		/* Run-Test/Idle */
}

static void idle(uint32_t n)
{
	jio_clk(0, 0, n);
}

/*
 * Shift n bits (n >= 1) through the selected register from Shift-xR,
 * the last with TMS 1 (to Exit1), then Update and back to Run-Test/Idle.
 * in/out: MSB first per byte (jio_shift's order). If keep, stay in
 * Shift-xR after the bits instead (more to come).
 */
static void shift_bits(const uint8_t *in, uint8_t *out, uint32_t n, int keep)
{
	if (keep) {
		jio_shift(in, out, n);
		return;
	}
	if (n > 1)
		jio_shift(in, out, n - 1);
	uint32_t i = n - 1;
	uint8_t m = (uint8_t)(0x80u >> (i & 7));
	int b = (in[i >> 3] & m) != 0;
	jio_set(SIG_TMS | SIG_TDI | SIG_TCK, SIG_TMS | (b ? SIG_TDI : 0));
	if (out && (jio_get() & SIG_TDO))
		out[i >> 3] |= m;
	jio_set(SIG_TCK, SIG_TCK);	/* the last bit, into Exit1 */
	jio_set(SIG_TCK, 0);
	tms_bits(0x1, 2);		/* Update, Run-Test/Idle: 1 then 0 */
}

static void ir(uint8_t code)
{
	tms_bits(0x3, 4);		/* RTI -> Select-DR -> Select-IR -> Capture-IR -> Shift-IR: 1,1,0,0 */
	uint8_t b = rev8(code);
	shift_bits(&b, 0, 8, 0);
}

static void dr_enter(void)
{
	tms_bits(0x1, 3);		/* RTI -> Select-DR -> Capture-DR -> Shift-DR: 1,0,0 */
}

/* a register of n <= 32 bits: write v, return what came out (LSB first) */
static uint32_t dr32(uint32_t v, int n)
{
	uint8_t in[4], out[4] = { 0, 0, 0, 0 };
	for (int i = 0; i < 4; i++)
		in[i] = rev8((uint8_t)(v >> (8 * i)));
	dr_enter();
	shift_bits(in, out, (uint32_t)n, 0);
	uint32_t r = 0;
	for (int i = 0; i < 4; i++)
		r |= (uint32_t)rev8(out[i]) << (8 * i);
	return n < 32 ? r & ((1u << n) - 1) : r;
}

uint32_t ecp5_last_idcode;

uint32_t ecp5_idcode(void)
{
	tap_reset();
	ir(IDCODE_PUB);
	ecp5_last_idcode = dr32(0, 32);
	return ecp5_last_idcode;
}

uint32_t ecp5_status(void)
{
	ir(LSC_READ_STATUS);
	return dr32(0, 32);
}

static int wait_busy(void)
{
	for (int i = 0; i < 1000; i++) {
		ir(LSC_CHECK_BUSY);
		if (dr32(0, 8) == 0)	/* any bit set: still busy */
			return 0;
		idle(1000);
	}
	return -1;
}

/* the FPGA erased and left in configuration mode: every pin as on an
   unconfigured chip (the boundary-scan register all ones). The closest
   thing to switching the module off that the RP2040 can do. */
int ecp5_blank(uint32_t idcode)
{
	if (ecp5_idcode() != idcode)
		return ECP5_ERR_IDCODE;

	/* PRELOAD the boundary-scan register with ones first, as openFPGALoader
	   does: in configuration mode the pins follow that register, and left
	   as it was they may drive whatever it held (Mach86's DDR3
	   trained after openFPGALoader's load but not after ours). 4096 ones
	   are more than any ECP5's register. */
	{
		static const uint8_t ones[64] = {
			0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
			0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
			0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
			0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		};
		ir(PRELOAD);
		dr_enter();
		for (int i = 0; i < 8; i++)
			shift_bits(ones, 0, 512, i < 7);
		idle(10);
	}

	ir(ISC_ENABLE);
	dr32(0x00, 8);			/* SRAM, not the flash */
	idle(1000);
	if (wait_busy() || !(ecp5_status() & ST_ISC_EN))
		return ECP5_ERR_ENABLE;

	ir(ISC_ERASE);
	dr32(0x01, 8);			/* the SRAM */
	idle(1000);
	if (wait_busy() || (ecp5_status() & ST_FAIL))
		return ECP5_ERR_ERASE;

	return ECP5_OK;
}

int ecp5_load(const uint8_t *bit, uint32_t len, uint32_t idcode, int (*yield)(void))
{
	int r = ecp5_blank(idcode);
	if (r != ECP5_OK)
		return r;

	ir(LSC_INIT_ADDRESS);
	idle(1000);

	ir(LSC_BITSTREAM_BURST);
	idle(2);
	dr_enter();
	for (uint32_t off = 0; off < len; off += CHUNK) {
		uint32_t n = len - off < CHUNK ? len - off : CHUNK;
		shift_bits(bit + off, 0, n * 8, off + n < len);
		if (off + n < len && yield && yield()) {
			tap_reset();
			return ECP5_ABORTED;
		}
	}
	/* The end of the bitstream (the block RAMs' contents) may still be
	   being written when the last bit is in: wait until the FPGA says it
	   is done, then a while more, as openFPGALoader's USB round trips do
	   by themselves (Mach86's DDR3 trainer, whose data is in
	   block RAM, failed after our load and not after openFPGALoader's). */
	idle(1000);
	if (wait_busy())
		return ECP5_ERR_CONFIG;
	if (ecp5_status() & ST_CNF_CHK)
		return ECP5_ERR_CONFIG;
	idle(ECP5_SETTLE_CLKS);

	ir(BYPASS);
	idle(10);
	ir(ISC_DISABLE);
	idle(1000);
	if (wait_busy() || (ecp5_status() & ST_ISC_EN))
		return ECP5_ERR_DISABLE;
	uint32_t st = ecp5_status();
	ir(BYPASS);
	idle(ECP5_SETTLE_CLKS);		/* the wake-up, with TCK running as openFPGALoader's does */
	if (!(st & ST_DONE))
		return ECP5_ERR_DONE;	/* a bitstream cut short, say: the FPGA waits for the rest */
	return ECP5_OK;
}

const char *ecp5_error(int e)
{
	switch (e) {
	case ECP5_OK: return "ok";
	case ECP5_ERR_IDCODE: return "another FPGA";
	case ECP5_ERR_ENABLE: return "ISC_ENABLE failed";
	case ECP5_ERR_ERASE: return "SRAM erase failed";
	case ECP5_ERR_CONFIG: return "bitstream rejected";
	case ECP5_ERR_DISABLE: return "ISC_DISABLE failed";
	case ECP5_ERR_DONE: return "DONE stayed low";
	case ECP5_ABORTED: return "the PC took JTAG";
	default: return "?";
	}
}
