/*
 * machboot supervisor -- RP2040 firmware for Sergei and Mozart
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The RP2040's pins, from the carriers' schematics (sergei v0 ld5,
 * mozart): the two are the same. docs/rp2040.md.
 */

#ifndef SV_BOARD_H
#define SV_BOARD_H

#define PIN_UART_TX	4	/* UART1 TX -> the module's UART_RX */
#define PIN_UART_RX	5	/* UART1 RX <- the module's UART_TX */
#define PIN_XC		8	/* the XA-XD link (docs/xlink.md): MISO, SPI1 RX */
#define PIN_XD		9	/* CS, a GPIO */
#define PIN_XB		10	/* SCK, SPI1 */
#define PIN_XA		11	/* MOSI, SPI1 TX */
#define PIN_LED		12	/* LED_R: active low (the LED is wired to 3V3) */
#define LED_ON		0
#define PIN_TDI		16
#define PIN_TDO		17
#define PIN_TCK		18
#define PIN_TMS		19
#define PIN_SYS_RST_N	20	/* not driven yet: an input, pulled up, so the module is never held in reset */
#define PIN_SYS_CLK48	23	/* GPOUT1: the module's 48 MHz clock */

#endif
