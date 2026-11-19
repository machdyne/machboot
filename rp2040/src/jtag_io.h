/*
 * machboot supervisor
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The JTAG pins, for djtag.c: jtag_rp2040.c on the board,
 * tests/jtag_model.c on a PC.
 */

#ifndef SV_JTAG_IO_H
#define SV_JTAG_IO_H

#include <stdint.h>

void jio_init(void);
void jio_set(uint8_t mask, uint8_t val);	/* SIG_* bits */
uint8_t jio_get(void);				/* the outputs as set, and SIG_TDO */
void jio_freq(uint32_t khz);
/* TCK pulses with TMS and TDI held */
void jio_clk(int tms, int tdi, uint32_t n);
/* bits on TDI with TMS held, TDO read before each rising edge: bit i is
 * byte i/8, bit 0x80 >> (i % 8) on both sides; tdo may be 0 */
void jio_shift(const uint8_t *tdi, uint8_t *tdo, uint32_t bits);
void jio_bootloader(void);

#endif
