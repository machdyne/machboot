/*
 * machboot supervisor
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * src/djtag.c on a PC, against a JTAG TAP model (IEEE 1149.1 state
 * machine, an ECP5's 8-bit IR and IDCODE), with a host side that sends
 * what openFPGALoader's dirtyJtag cable driver sends: INFO, FREQ, TMS
 * moves as SETSIG pairs, shifts as XFER (with EXTEND and NO_READ), the
 * last bit of a shift with SETSIG/GETSIG, CLK runs. The USB packets are
 * cut at 64 bytes, as a full-speed bulk endpoint delivers them.
 *
 *   cc -I../src test_djtag.c ../src/djtag.c -o test_djtag && ./test_djtag
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "djtag.h"
#include "jtag_io.h"

/* ---- the target: a TAP ------------------------------------------------- */

enum { TLR, RTI, SEL_DR, CAP_DR, SH_DR, EX1_DR, PA_DR, EX2_DR, UP_DR,
       SEL_IR, CAP_IR, SH_IR, EX1_IR, PA_IR, EX2_IR, UP_IR };
static int st = TLR;
static uint8_t ir = 0xE0, ir_sh;	/* ECP5: READ_ID 0xE0 */
#define IDCODE 0x41111043u		/* LFE5U-25F */
static uint64_t dr_sh;			/* capture/shift register */
static int dr_len;
static uint8_t burst[8192];		/* LSC_BITSTREAM_BURST 0x7A: what it took */
static uint32_t burst_bits;
static uint64_t loop;			/* 0x55: a 64-bit register, shifted back */
static int tck, tms, tdi, tdo;
static uint32_t pulses;

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

static void update_tdo(void)
{
	if (st == SH_IR)
		tdo = ir_sh & 1;
	else if (st == SH_DR)
		tdo = (ir == 0x7A) ? 0 : (int)(dr_sh & 1);
	else
		tdo = 1;
}

static void rising(void)
{
	pulses++;
	/* shift on the rising edge, in the shift states */
	if (st == SH_IR) {
		ir_sh = (uint8_t)((ir_sh >> 1) | (tdi << 7));
	} else if (st == SH_DR) {
		if (ir == 0x7A) {
			if (tdi) burst[burst_bits >> 3] |= (uint8_t)(1 << (burst_bits & 7));
			burst_bits++;
		} else {
			dr_sh = (dr_sh >> 1) | ((uint64_t)tdi << (dr_len - 1));
		}
	}
	int ns = next(st, tms);
	if (ns == TLR) ir = 0xE0;
	if (ns == CAP_IR) ir_sh = 0x01;			/* IR capture: ...01 */
	if (ns == CAP_DR) {
		if (ir == 0xE0) { dr_sh = IDCODE; dr_len = 32; }
		else if (ir == 0x55) { dr_sh = loop; dr_len = 64; }
		else if (ir == 0x7A) { dr_len = 1; }
		else { dr_sh = 0; dr_len = 1; }		/* BYPASS */
	}
	if (ns == UP_IR) ir = ir_sh;
	if (ns == UP_DR && ir == 0x55) loop = dr_sh;
	st = ns;
}

static void falling(void) { update_tdo(); }

/* ---- jtag_io.h on the model ---------------------------------------------- */

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
static int to_bootloader;
void jio_bootloader(void) { to_bootloader = 1; }

/* ---- the USB side ---------------------------------------------------------- */

static uint8_t in_ep[4096];
static uint32_t in_n, flushes;
void djtag_reply(const uint8_t *p, uint32_t n) { memcpy(in_ep + in_n, p, n); in_n += n; }
void djtag_flush(void) { flushes++; }

/* a bulk OUT transfer: 64-byte packets */
static void bulk_out(const uint8_t *p, uint32_t n)
{
	while (n) {
		uint32_t k = n < 64 ? n : 64;
		djtag_feed(p, k);
		p += k;
		n -= k;
	}
}

static uint32_t bulk_in(uint8_t *p)
{
	uint32_t n = in_n;
	memcpy(p, in_ep, n);
	in_n = 0;
	return n;
}

/* ---- the host: what openFPGALoader's cable driver does ------------------- */

static int failures;
static void check(int c, const char *what) { printf("%s %s\n", c ? "ok  " : "FAIL", what); failures += !c; }

static void tms_seq(const char *bits, int tdi_v)
{
	uint8_t b[512];
	uint32_t k = 0;
	size_t n = strlen(bits);
	for (size_t i = 0; i < n; i++) {
		uint8_t v = (uint8_t)((tdi_v ? SIG_TDI : 0) | (bits[i] == '1' ? SIG_TMS : 0));
		uint8_t m = SIG_TCK | SIG_TMS | SIG_TDI;
		b[k++] = CMD_SETSIG; b[k++] = m; b[k++] = v;
		b[k++] = CMD_SETSIG; b[k++] = m; b[k++] = (uint8_t)(v | SIG_TCK);
		if (i == n - 1) { b[k++] = CMD_SETSIG; b[k++] = m; b[k++] = v; }
	}
	b[k++] = CMD_STOP;
	bulk_out(b, k);
}

/* shift len bits (LSB first in tx, as JTAG wants); the last with TMS=1 if end */
static void shift(const uint8_t *tx, uint8_t *rx, uint32_t len, int end)
{
	uint32_t real = len - (end ? 1 : 0), done = 0;
	while (done < real) {
		uint32_t n = real - done > 496 ? 496 : real - done;
		uint8_t b[2 + 64];
		memset(b, 0, sizeof(b));
		b[0] = (uint8_t)(CMD_XFER | (rx ? 0 : XFER_NO_READ) | (n > 255 ? XFER_EXTEND : 0));
		b[1] = (uint8_t)(n > 255 ? n - 256 : n);
		for (uint32_t i = 0; i < n; i++)
			if (tx[(done + i) >> 3] & (1 << ((done + i) & 7)))
				b[2 + (i >> 3)] |= (uint8_t)(0x80 >> (i & 7));
		bulk_out(b, 2 + (n + 7) / 8);
		if (rx) {
			uint8_t r[512];
			uint32_t got = bulk_in(r);
			if (got < (n + 7) / 8) { check(0, "XFER reply long enough"); return; }
			for (uint32_t i = 0; i < n; i++)
				if (r[i >> 3] & (0x80 >> (i & 7)))
					rx[(done + i) >> 3] |= (uint8_t)(1 << ((done + i) & 7));
		}
		done += n;
	}
	if (end) {
		uint32_t pos = len - 1;
		int d = (tx[pos >> 3] >> (pos & 7)) & 1;
		uint8_t m = SIG_TMS | SIG_TDI | SIG_TCK, v = (uint8_t)(SIG_TMS | (d ? SIG_TDI : 0));
		uint8_t b[] = { CMD_SETSIG, m, v, CMD_SETSIG, m, (uint8_t)(v | SIG_TCK), CMD_GETSIG, CMD_STOP };
		/* GETSIG after the rising edge: the target changes TDO only on
		 * the falling edge, so it still shows the last bit */
		bulk_out(b, sizeof(b));
		uint8_t s;
		bulk_in(&s);
		if (rx && (s & SIG_TDO)) rx[pos >> 3] |= (uint8_t)(1 << (pos & 7));
		uint8_t c[] = { CMD_SETSIG, m, v, CMD_STOP };
		bulk_out(c, sizeof(c));
	}
}

int main(void)
{
	uint8_t r[64];
	djtag_init();

	/* getVersion */
	uint8_t info[] = { CMD_INFO, CMD_STOP };
	bulk_out(info, 2);
	uint32_t n = bulk_in(r);
	check(n == 7 && !memcmp(r, "DJTAG2\n", 7), "INFO: DJTAG2");

	/* setClkFreq 6 MHz */
	uint8_t fq[] = { CMD_FREQ, 6000 >> 8, 6000 & 0xff, CMD_STOP };
	bulk_out(fq, 4);
	check(in_n == 0, "FREQ: no reply");

	/* reset, Run-Test/Idle, IR <- 0xE0 (READ_ID), DR: 32 bits */
	tms_seq("111110", 0);
	check(st == RTI, "TMS moves: Test-Logic-Reset, then Run-Test/Idle");
	tms_seq("1100", 0);
	check(st == SH_IR, "to Shift-IR");
	uint8_t irv = 0xE0, ircap = 0;
	shift(&irv, &ircap, 8, 1);
	check(ircap == 0x01, "IR capture reads 0x01");
	tms_seq("10", 0);				/* Update-IR, Run-Test/Idle */
	check(st == RTI && ir == 0xE0, "IR = READ_ID");
	tms_seq("100", 0);
	check(st == SH_DR, "to Shift-DR");
	uint8_t zero[4] = { 0 }, id[4] = { 0 };
	shift(zero, id, 32, 1);
	uint32_t idv = (uint32_t)id[0] | id[1] << 8 | id[2] << 16 | (uint32_t)id[3] << 24;
	printf("     IDCODE %08x\n", idv);
	check(idv == IDCODE, "IDCODE read through XFER and the last bit by GETSIG");
	tms_seq("10", 0);

	/* a 64-bit register, written then read back: XFER both ways */
	tms_seq("1100", 0);
	irv = 0x55;
	shift(&irv, 0, 8, 1);
	tms_seq("10", 0);
	uint8_t pat[8] = { 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef }, back[8] = { 0 };
	tms_seq("100", 0);
	shift(pat, 0, 64, 1);
	tms_seq("10", 0);
	tms_seq("100", 0);
	shift(zero, back, 64, 1);			/* zero is 4 bytes: the rest is 0 anyway */
	tms_seq("10", 0);
	check(!memcmp(pat, back, 8), "a 64-bit register written (NO_READ) and read back");

	/* a long burst, as a bitstream load: 4000 bits per call, many XFERs with
	 * EXTEND_LENGTH and NO_READ, packets cut at 64 bytes */
	tms_seq("1100", 0);
	irv = 0x7A;
	shift(&irv, 0, 8, 1);
	tms_seq("10", 0);
	tms_seq("100", 0);
	static uint8_t bit[4096];
	srand(3);
	for (unsigned i = 0; i < sizeof(bit); i++) bit[i] = (uint8_t)rand();
	uint32_t total = sizeof(bit) * 8;
	shift(bit, 0, total, 1);
	tms_seq("10", 0);
	check(burst_bits == total && !memcmp(burst, bit, sizeof(bit)), "32768 bits of burst, byte for byte");

	/* CLK: Run-Test/Idle cycles (ECP5 waits) */
	uint32_t p0 = pulses;
	uint8_t clk[] = { CMD_CLK, 0, 64, CMD_CLK, 0, 36, CMD_STOP };
	bulk_out(clk, sizeof(clk));
	check(pulses - p0 == 100 && st == RTI, "CLK: 100 pulses with TMS low, still in Run-Test/Idle");

	/* a reply split by packet boundaries: an XFER straddling two packets */
	tms_seq("100", 0);			/* Shift-DR of the burst register */
	uint8_t straddle[64 + 40];
	memset(straddle, CMD_STOP, sizeof(straddle));
	straddle[63] = CMD_XFER | XFER_NO_READ | XFER_EXTEND;	/* the last byte of packet 1 */
	straddle[64] = 300 - 256;				/* the first of packet 2 */
	for (int i = 0; i < 38; i++) straddle[65 + i] = 0xA5;
	uint32_t b0 = burst_bits;
	bulk_out(straddle, 64 + 39);
	check(burst_bits - b0 == 300, "an XFER header at the end of one packet, its data in the next");
	tms_seq("110", 0);

	/* the bootloader */
	uint8_t bl[] = { CMD_GOTOBOOTLOADER };
	bulk_out(bl, 1);
	check(to_bootloader, "GOTOBOOTLOADER");

	printf(failures ? "FAIL: %d\n" : "all passed\n", failures);
	return failures != 0;
}
