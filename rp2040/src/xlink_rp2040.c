/*
 * machboot supervisor
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The XA-XD link's pins on the RP2040 (xlink_io.h): SPI1 as master,
 * mode 0, 8.93 MHz (docs/xlink.md: the gateware samples at 48 MHz and
 * works to 10.4 MHz in simulation).
 *
 *   XB  GP10  SPI1 SCK
 *   XA  GP11  SPI1 TX   (MOSI)
 *   XC  GP8   SPI1 RX   (MISO)    never driven by us
 *   XD  GP9   CS, a GPIO (SPI1's own CSn would rise between bytes)
 *
 * Released, all four are inputs without pulls: the FPGA's pull-ups
 * (the boards/ pin files) hold CS high, and whatever else is loaded may use the
 * pins as it likes.
 */

#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "board.h"
#include "xlink_io.h"

#define XL_SPI		spi1
#define XL_BAUD		8928571		/* 125 MHz / 14 */

void xio_attach(int on)
{
	if (on) {
		gpio_init(PIN_XD);
		gpio_put(PIN_XD, 1);
		gpio_set_dir(PIN_XD, GPIO_OUT);
		spi_init(XL_SPI, XL_BAUD);
		spi_set_format(XL_SPI, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
		gpio_set_function(PIN_XB, GPIO_FUNC_SPI);
		gpio_set_function(PIN_XA, GPIO_FUNC_SPI);
		gpio_set_function(PIN_XC, GPIO_FUNC_SPI);
		gpio_disable_pulls(PIN_XC);
	} else {
		spi_deinit(XL_SPI);
		const uint pins[4] = { PIN_XA, PIN_XB, PIN_XC, PIN_XD };
		for (int i = 0; i < 4; i++) {
			gpio_init(pins[i]);		/* SIO, input */
			gpio_disable_pulls(pins[i]);
		}
	}
}

void xio_select(int on)
{
	if (on) {
		gpio_put(PIN_XD, 0);
		busy_wait_us_32(1);		/* the FPGA sees CS fall before the first edge */
	} else {
		busy_wait_us_32(1);
		gpio_put(PIN_XD, 1);
		busy_wait_us_32(1);		/* and rise, before the next frame */
	}
}

void xio_xfer(const uint8_t *out, uint8_t *in, int n)
{
	static const uint8_t zeros[64];
	uint8_t sink[64];
	while (n > 0) {
		int k = n > 64 ? 64 : n;
		spi_write_read_blocking(XL_SPI, out ? out : zeros, in ? in : sink, (size_t)k);
		if (out) out += k;
		if (in) in += k;
		n -= k;
	}
}

uint32_t xio_ms(void)
{
	return to_ms_since_boot(get_absolute_time());
}

void xio_idle(void)
{
	busy_wait_us_32(5);
}
