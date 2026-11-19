/*
 * machboot
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Included by sim_main.cc for the Sergei build (sim-xl): the RP2040's
 * own card-reader code -- rp2040/src/xlink.c and msc.c, compiled in
 * here -- driving the XA-XD pins of the simulated SoC, SPI at 6.25 MHz
 * in simulated time, as rp2040/src/xlink_rp2040.c does on the board.
 * On the other side: rtl/mb_xlink.v, the Sechzig firmware (fw/xlink.c)
 * on the CPU, and the SD controller with tests/sdmodel.c's card.
 */

extern "C" {
#include "xlink.h"
#include "xlink_io.h"
#include "msc.h"
#include "cache.h"
}

static const uint64_t XL_HALF_PS = 56000;	/* 8.93 MHz, as rp2040/src/xlink_rp2040.c */
static int xl_attached;
static uint64_t xl_t_xfer, xl_t_idle, xl_t_sel, xl_n_frames, xl_n_idle;

static void xl_wait_ps(uint64_t ps)
{
	uint64_t end = now_ps + ps;
	while (now_ps < end)
		step_one();
}

extern "C" void xio_attach(int on)
{
	xl_attached = on;
	top.p_xl__cs__n.set<bool>(true);
	top.p_xl__sck.set<bool>(false);
}

extern "C" void xio_select(int on)
{
	uint64_t t0 = now_ps;
	xl_wait_ps(1000000);			/* 1 us, as busy_wait_us_32(1) */
	top.p_xl__cs__n.set<bool>(!on);
	xl_wait_ps(1000000);
	xl_t_sel += now_ps - t0;
	if (on) xl_n_frames++;
}

extern "C" void xio_xfer(const uint8_t *out, uint8_t *in, int n)
{
	uint64_t t0 = now_ps;
	for (int k = 0; k < n; k++) {
		uint8_t o = out ? out[k] : 0, r = 0;
		for (int i = 7; i >= 0; i--) {
			top.p_xl__mosi.set<bool>((o >> i) & 1);
			xl_wait_ps(XL_HALF_PS);
			top.p_xl__sck.set<bool>(true);
			/* MISO as the RP2040 sees it: driven only while CS is low */
			int m = top.p_xl__miso__oe.get<bool>() ? top.p_xl__miso.get<bool>() : 1;
			r = (uint8_t)(r << 1 | m);
			xl_wait_ps(XL_HALF_PS);
			top.p_xl__sck.set<bool>(false);
		}
		if (in)
			in[k] = r;
	}
	xl_t_xfer += now_ps - t0;
}

extern "C" uint32_t xio_ms(void)
{
	return (uint32_t)(sys_cycles / 48000);
}

extern "C" void xio_idle(void)
{
	uint64_t t0 = now_ps;
	xl_wait_ps(5000000);			/* 5 us */
	xl_t_idle += now_ps - t0;
	xl_n_idle++;
}
