/*
 * machboot supervisor
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * A model of an ECP5's JTAG configuration logic, and jtag_io.h on it,
 * for the PC tests (test_ecp5.c, test_sv.c). Included once per test.
 */

#include <stdlib.h>
#include <string.h>
#include "jtag_io.h"
#include "djtag.h"

enum { TLR, RTI, SEL_DR, CAP_DR, SH_DR, EX1_DR, PA_DR, EX2_DR, UP_DR,
       SEL_IR, CAP_IR, SH_IR, EX1_IR, PA_IR, EX2_IR, UP_IR };

static uint32_t model_idcode = 0x41112043u;	/* LFE5U-45F */
static int st = TLR;
static uint8_t ir = 0xE0, ir_sh;
static uint64_t dr_sh;
static int dr_len;
static int isc_en, done, busy_polls, erased, fail_cnf, wrong_order;
static int refuse_enable;		/* ISC_ENABLEs to ignore (busy with its own flash) */
static uint8_t *burst;
static uint32_t burst_bits, burst_cap;
static int tck, tms, tdi, tdo;
static uint64_t pulses;

static int next(int s, int m)
{
	static const int t[16][2] = {
		{RTI, TLR}, {RTI, SEL_DR}, {CAP_DR, SEL_IR}, {SH_DR, EX1_DR}, {SH_DR, EX1_DR},
		{PA_DR, UP_DR}, {PA_DR, EX2_DR}, {SH_DR, UP_DR}, {RTI, SEL_DR},
		{CAP_IR, TLR}, {SH_IR, EX1_IR}, {SH_IR, EX1_IR}, {PA_IR, UP_IR}, {PA_IR, EX2_IR},
		{SH_IR, UP_IR}, {RTI, SEL_DR},
	};
	return t[s][m];
}

static uint32_t status(void)
{
	return (uint32_t)(done << 8 | isc_en << 9 | (busy_polls > 0) << 12 | fail_cnf << 23);
}

static void update_tdo(void)
{
	if (st == SH_IR) tdo = ir_sh & 1;
	else if (st == SH_DR) tdo = (ir == 0x7A) ? 0 : (int)(dr_sh & 1);
	else tdo = 1;
}

static void rising(void)
{
	pulses++;
	if (st == SH_IR) {
		ir_sh = (uint8_t)((ir_sh >> 1) | (tdi << 7));
	} else if (st == SH_DR) {
		if (ir == 0x7A) {
			if (!isc_en || !erased) wrong_order = 1;
			if (burst_bits >= burst_cap * 8) { burst_cap = burst_cap ? burst_cap * 2 : 65536; burst = realloc(burst, burst_cap); memset(burst + burst_cap / 2, 0, burst_cap / 2); }
			if (tdi) burst[burst_bits >> 3] |= (uint8_t)(1 << (burst_bits & 7));
			burst_bits++;
		} else {
			dr_sh = (dr_sh >> 1) | ((uint64_t)tdi << (dr_len - 1));
		}
	}
	int ns = next(st, tms);
	if (ns == TLR) ir = 0xE0;
	if (ns == CAP_IR) ir_sh = 0x01;
	if (ns == CAP_DR) {
		switch (ir) {
		case 0xE0: dr_sh = model_idcode; dr_len = 32; break;
		case 0x3C: dr_sh = status(); dr_len = 32; break;
		case 0xF0: dr_sh = busy_polls > 0 ? 0x01 : 0x00; dr_len = 8; if (busy_polls > 0) busy_polls--; break;
		case 0xC6: case 0x0E: dr_sh = 0; dr_len = 8; break;
		case 0x7A: dr_len = 1; break;
		default: dr_sh = 0; dr_len = 1;
		}
	}
	if (ns == UP_IR) {
		ir = ir_sh;
		if (ir == 0x26) {			/* ISC_DISABLE: wake up */
			if (isc_en && burst_bits && !fail_cnf && !wrong_order) done = 1;
			isc_en = 0;
			busy_polls = 2;
		}
		if (ir == 0x46 && !isc_en) wrong_order = 1;
	}
	if (ns == UP_DR) {
		if (ir == 0xC6 && (dr_sh & 0xff) == 0x00) { if (refuse_enable > 0) refuse_enable--; else { isc_en = 1; busy_polls = 2; done = 0; } }
		if (ir == 0x0E && (dr_sh & 0xff) == 0x01) { if (!isc_en) wrong_order = 1; erased = 1; busy_polls = 5; burst_bits = 0; }
	}
	st = ns;
}

static void falling(void) { update_tdo(); }

static uint8_t outs;
void jio_init(void) { outs = 0; tck = tms = tdi = 0; update_tdo(); }
void jio_freq(uint32_t khz) { (void)khz; }
void jio_set(uint8_t mask, uint8_t val)
{
	outs = (uint8_t)((outs & ~mask) | (val & mask));
	tms = (outs & SIG_TMS) != 0;
	tdi = (outs & SIG_TDI) != 0;
	int ntck = (outs & SIG_TCK) != 0;
	if (ntck && !tck) rising();
	if (!ntck && tck) falling();
	tck = ntck;
}
uint8_t jio_get(void) { return (uint8_t)((outs & ~SIG_TDO) | (tdo ? SIG_TDO : 0)); }
void jio_clk(int m, int d, uint32_t n)
{
	jio_set(SIG_TMS | SIG_TDI | SIG_TCK, (m ? SIG_TMS : 0) | (d ? SIG_TDI : 0));
	while (n--) { jio_set(SIG_TCK, SIG_TCK); jio_set(SIG_TCK, 0); }
}
void jio_shift(const uint8_t *in, uint8_t *out, uint32_t bits)
{
	for (uint32_t i = 0; i < bits; i++) {
		uint8_t m = (uint8_t)(0x80u >> (i & 7));
		jio_set(SIG_TDI | SIG_TCK, (in[i >> 3] & m) ? SIG_TDI : 0);
		if (out && tdo) out[i >> 3] |= m;
		jio_set(SIG_TCK, SIG_TCK);
		jio_set(SIG_TCK, 0);
	}
}
void jio_bootloader(void) {}

