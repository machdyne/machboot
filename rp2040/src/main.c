/*
 * machboot supervisor -- RP2040 firmware for Sergei and Mozart
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * A drop-in (docs/rp2040.md) for the dirtyJTAG firmware the
 * carriers ship with -- JTAG for openFPGALoader, the module's UART as a
 * USB serial port, and the module's 48 MHz clock. Clean-room: written
 * from the protocol and the schematics, not from dirtyJTAG.
 *
 * Also (docs/power-on.md): the menu loaded at power-on and after a
 * reset (sv.c), settings kept here (store.c), the card reader (msc.c).
 */

#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/uart.h"
#include "hardware/irq.h"
#include "tusb.h"
#include "board.h"
#include "djtag.h"
#include "msc.h"
#include "xlink_io.h"
#include "sv.h"
#include "sv_rp2040.h"

/* -- the module's clock: before anything else -------------------------- */

static void clock_out(void)
{
	/* clk_usb is 48 MHz from PLL_USB, set up before main() */
	clock_gpio_init(PIN_SYS_CLK48, CLOCKS_CLK_GPOUT1_CTRL_AUXSRC_VALUE_CLK_USB, 1);
}

/* -- JTAG: the vendor interface ------------------------------------------- */

static uint8_t reply[512];
static uint32_t nreply;

void djtag_reply(const uint8_t *p, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++) {
		if (nreply == sizeof(reply))
			djtag_flush();
		reply[nreply++] = p[i];
	}
}

void djtag_flush(void)
{
	uint32_t off = 0;
	while (off < nreply) {
		if (!tud_vendor_mounted())
			break;
		uint32_t n = tud_vendor_write(reply + off, nreply - off);
		off += n;
		tud_vendor_flush();
		if (off < nreply)
			tud_task();
	}
	nreply = 0;
}

static uint32_t activity;

static void jtag_task(void)
{
	uint8_t buf[64];
	while (tud_vendor_available()) {
		uint32_t n = tud_vendor_read(buf, sizeof(buf));
		djtag_feed(buf, n);
		activity = time_us_32();
		sv_jtag_activity_us = activity;
		sv_jtag_seen = 1;
	}
}

/* -- the module's UART on the CDC port ----------------------------------------- */

#define UART uart1
static uint8_t rxq[2048];
static volatile uint32_t rxq_h, rxq_t;

static void uart_irq(void)
{
	while (uart_is_readable(UART)) {
		uint8_t c = (uint8_t)uart_getc(UART);
		if (rxq_h - rxq_t < sizeof(rxq))
			rxq[rxq_h++ % sizeof(rxq)] = c;
	}
}

static void uart_bridge_init(void)
{
	uart_init(UART, 115200);
	gpio_set_function(PIN_UART_TX, GPIO_FUNC_UART);
	gpio_set_function(PIN_UART_RX, GPIO_FUNC_UART);
	gpio_pull_up(PIN_UART_RX);
	uart_set_fifo_enabled(UART, true);
	irq_set_exclusive_handler(UART1_IRQ, uart_irq);
	irq_set_enabled(UART1_IRQ, true);
	uart_set_irq_enables(UART, true, false);
}

void tud_cdc_line_coding_cb(uint8_t itf, cdc_line_coding_t const *lc)
{
	(void)itf;
	uart_parity_t par = lc->parity == 1 ? UART_PARITY_ODD : lc->parity == 2 ? UART_PARITY_EVEN : UART_PARITY_NONE;
	uint data = lc->data_bits >= 5 && lc->data_bits <= 8 ? lc->data_bits : 8;
	uint stop = lc->stop_bits == 2 ? 2 : 1;
	uart_set_baudrate(UART, lc->bit_rate ? lc->bit_rate : 115200);
	uart_set_format(UART, data, stop, par);
}

/* a terminal opened the port: the supervisor's log from power-on first */
void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts)
{
	(void)itf; (void)rts;
	if (dtr)
		sv_log_replay();
}

/* a serial break (minicom: Ctrl-A F): load the menu now */
void tud_cdc_send_break_cb(uint8_t itf, uint16_t duration_ms)
{
	(void)itf; (void)duration_ms;
	sv_request();
}

static uint8_t last_out = '\n';
static uint32_t quiet_since;

static void uart_task(void)
{
	/* the supervisor's lines, between the module's lines (or when it is quiet) */
	uint32_t ln;
	const char *lp = sv_log_pending(&ln);
	if (ln && tud_cdc_connected() && (last_out == '\n' || time_us_32() - quiet_since > 50000)) {
		uint32_t n = tud_cdc_write_available();
		if (n > ln)
			n = ln;
		if (n) {
			if (last_out != '\n')
				tud_cdc_write("\r\n", 2);
			tud_cdc_write(lp, n);
			tud_cdc_write_flush();
			sv_log_taken(n);
			last_out = '\n';
		}
	}
	/* module -> PC */
	if (rxq_h != rxq_t && tud_cdc_connected()) {
		uint8_t b[64];
		uint32_t n = 0;
		while (n < sizeof(b) && rxq_t != rxq_h && tud_cdc_write_available() > n)
			b[n++] = rxq[rxq_t++ % sizeof(rxq)];
		if (n) {
			tud_cdc_write(b, n);
			tud_cdc_write_flush();
			last_out = b[n - 1];
			quiet_since = time_us_32();
		}
	} else if (!tud_cdc_connected()) {
		rxq_t = rxq_h;		/* nobody listens: drop */
	}
	/* PC -> module */
	while (tud_cdc_available() && uart_is_writable(UART)) {
		uint8_t c;
		if (tud_cdc_read(&c, 1) == 1)
			uart_putc_raw(UART, (char)c);
	}
}

/* -- the LED: on when the PC has configured us; blinks with JTAG ------------ */

static void led_task(void)
{
	int on = tud_mounted();
	if (time_us_32() - activity < 100000)
		on = (time_us_32() >> 15) & 1;
	gpio_put(PIN_LED, on ? LED_ON : !LED_ON);
}

int main(void)
{
	clock_out();
	/* the RP2040's pads come out of reset pulled down: SYS_RST_N must not be */
	gpio_init(PIN_SYS_RST_N);
	gpio_set_dir(PIN_SYS_RST_N, GPIO_IN);
	gpio_pull_up(PIN_SYS_RST_N);
	gpio_init(PIN_LED);
	gpio_put(PIN_LED, !LED_ON);
	gpio_set_dir(PIN_LED, GPIO_OUT);
	djtag_init();
	uart_bridge_init();
	xio_attach(0);		/* the XA-XD link released until machboot answers on it */
	tusb_init();
	sv_board_init();	/* at power-on: the menu, first thing */
	for (;;) {
		tud_task();
		jtag_task();
		uart_task();
		led_task();
		/* the card reader's HELLO, not while openFPGALoader is at work (a
		   JTAG load leaves no machboot to answer, and must not wait) */
		msc_set_pc(tud_mounted());
		msc_background();	/* while USB sends the last READ's sectors */
		if (!sv_jtag_seen || time_us_32() - activity > 1500000)
			msc_tick();
		sv_tick();
	}
}
