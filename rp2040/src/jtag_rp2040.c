/*
 * machboot supervisor
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The JTAG pins on the RP2040, bit by bit through the SIO (a PIO
 * engine is a later speed-up; docs/rp2040.md). TDI and TMS change
 * while TCK is low, TDO is read just before TCK rises -- the target
 * changes TDO on the falling edge.
 */

#include "jtag_io.h"
#include "djtag.h"
#include "board.h"
#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "hardware/clocks.h"
#include "hardware/structs/sio.h"

static uint8_t outs;			/* SIG_* as last set */
static uint32_t half;			/* busy-wait loops per half period */

#define M(p) (1u << (p))

static inline void __not_in_flash_func(wait)(uint32_t n)
{
	while (n--)
		__asm volatile ("nop");
}

void jio_init(void)
{
	const uint pins[] = { PIN_TCK, PIN_TMS, PIN_TDI };
	for (unsigned i = 0; i < 3; i++) {
		gpio_init(pins[i]);
		gpio_put(pins[i], 0);
		gpio_set_dir(pins[i], GPIO_OUT);
	}
	gpio_init(PIN_TDO);
	gpio_set_dir(PIN_TDO, GPIO_IN);
	gpio_pull_up(PIN_TDO);
	outs = 0;
	jio_freq(6000);
}

void jio_freq(uint32_t khz)
{
	if (!khz)
		khz = 1;
	/* about 4 cycles per wait loop and ~12 more per half period */
	uint32_t cyc = clock_get_hz(clk_sys) / 1000u / khz / 2u;
	half = cyc > 12 ? (cyc - 12) / 4 : 0;
}

void jio_set(uint8_t mask, uint8_t val)
{
	outs = (uint8_t)((outs & ~mask) | (val & mask));
	uint32_t set = 0, clr = 0;
	if (mask & SIG_TCK) { if (val & SIG_TCK) set |= M(PIN_TCK); else clr |= M(PIN_TCK); }
	if (mask & SIG_TMS) { if (val & SIG_TMS) set |= M(PIN_TMS); else clr |= M(PIN_TMS); }
	if (mask & SIG_TDI) { if (val & SIG_TDI) set |= M(PIN_TDI); else clr |= M(PIN_TDI); }
	sio_hw->gpio_clr = clr;
	sio_hw->gpio_set = set;
}

uint8_t jio_get(void)
{
	return (uint8_t)((outs & ~SIG_TDO) | (gpio_get(PIN_TDO) ? SIG_TDO : 0));
}

void __not_in_flash_func(jio_clk)(int tms, int tdi, uint32_t n)
{
	jio_set(SIG_TMS | SIG_TDI | SIG_TCK, (tms ? SIG_TMS : 0) | (tdi ? SIG_TDI : 0));
	uint32_t h = half;
	while (n--) {
		sio_hw->gpio_set = M(PIN_TCK);
		wait(h);
		sio_hw->gpio_clr = M(PIN_TCK);
		wait(h);
	}
}

void __not_in_flash_func(jio_shift)(const uint8_t *tdi, uint8_t *tdo, uint32_t bits)
{
	uint32_t h = half;
	sio_hw->gpio_clr = M(PIN_TCK);
	for (uint32_t i = 0; i < bits; i++) {
		uint8_t m = (uint8_t)(0x80u >> (i & 7));
		if (tdi[i >> 3] & m)
			sio_hw->gpio_set = M(PIN_TDI);
		else
			sio_hw->gpio_clr = M(PIN_TDI);
		wait(h);
		if (tdo && (sio_hw->gpio_in & M(PIN_TDO)))
			tdo[i >> 3] |= m;
		sio_hw->gpio_set = M(PIN_TCK);
		wait(h);
		sio_hw->gpio_clr = M(PIN_TCK);
	}
	outs &= (uint8_t)~SIG_TCK;
	if (bits) {
		uint8_t last = tdi[(bits - 1) >> 3] & (0x80u >> ((bits - 1) & 7));
		outs = (uint8_t)((outs & ~SIG_TDI) | (last ? SIG_TDI : 0));
	}
}

void jio_bootloader(void)
{
	reset_usb_boot(1u << PIN_LED, 0);	/* the LED shows the boot ROM's activity */
}
