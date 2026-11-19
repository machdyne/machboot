/*
 * machboot
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Simulation top: the SoC exactly as a board top builds it, with the
 * clocks, the flash pins, the serial port and the DVI lanes brought out
 * to sim_main.cc. No PLL (the testbench makes the clocks), no USB host;
 * the USB device's lines go to sim_main.cc's bit-level USB host.
 */
module sim_top (
	input clk,
	input pclk,
	input sclk,
	input rst,
	output flash_ss,
	output flash_sck,
	output flash_mosi,
	input flash_miso,
	output uart_tx,
	input uart_rx,
	output sd_ss,
	output sd_sck,
	output sd_mosi,
	input sd_miso,
	output dvi_d0,
	output dvi_d1,
	output dvi_d2,
	output dvi_ck,
	output led,
	output programn_pull,
	output usbd_tx_en,
	output usbd_dp_tx,
	output usbd_dn_tx,
	output usbd_pu_on,
	input usbd_dp_rx,
	input usbd_dn_rx,
	// the XA-XD link (MB_SIM_SECHZIG): sim/xlink.inc drives it as the RP2040
	input xl_sck,
	input xl_mosi,
	input xl_cs_n,
	output xl_miso,
	output xl_miso_oe
);
	wire [1:0] usb_dp;
	wire [1:0] usb_dm;

	mb_soc #(
`ifdef MB_SIM_SECHZIG
		// a Sergei ML2, as rtl/top_sechzig.v builds it
		.RAM_INIT("build/bootrom-sram.hex"),
		.RAM_PRELOAD("build/fw-sechzig-ram.hex"),
		.FONT_FILE("build/font.hex"),
		.BOARD(32'h0000_0202),
		.IDCODE(32'h4111_2043),
		.SLOT(32'h0000_0000),
		.FLASH_LOCK_END(24'h00_0000),
		.FLASH_WIN_START(24'h00_0000),
		.FLASH_WIN_END(24'h00_0000),
		.FLASH_LOCK_FROM(25'h000_0000),
`else
`ifdef MB_SRAM_TEST
		.RAM_INIT("build/bootrom-sram.hex"),
		.RAM_PRELOAD("build/fw-ram.hex"),
`else
		.RAM_INIT("build/bootrom.hex"),
`endif
		.FONT_FILE("build/font.hex"),
		.BOARD(32'd1),
		.IDCODE(32'h4111_1043),
		.SLOT(32'h0006_0000),
		.FLASH_LOCK_END(24'h06_0000),
`endif
`ifdef MB_INSTALLER
		.INSTALLER(1),
`endif
		.PROGRAMN_AVAIL(1),
		.USB_PORTS(2)
	) soc (
		.clk(clk), .rst(rst), .pclk(pclk), .sclk(sclk),
		.dvi_d0(dvi_d0), .dvi_d1(dvi_d1), .dvi_d2(dvi_d2), .dvi_ck(dvi_ck),
		.flash_ss(flash_ss), .flash_sck(flash_sck), .flash_mosi(flash_mosi), .flash_miso(flash_miso),
		.uart_tx(uart_tx), .uart_rx(uart_rx),
		.sd_ss(sd_ss), .sd_sck(sd_sck), .sd_mosi(sd_mosi), .sd_miso(sd_miso),
		.usb_dp(usb_dp), .usb_dm(usb_dm),
		.usbd_tx_en(usbd_tx_en), .usbd_dp_tx(usbd_dp_tx), .usbd_dn_tx(usbd_dn_tx), .usbd_pu_on(usbd_pu_on),
		.usbd_dp_rx(usbd_dp_rx), .usbd_dn_rx(usbd_dn_rx),
		.led(led), .programn_pull(programn_pull),
		.xl_sck(xl_sck), .xl_mosi(xl_mosi), .xl_cs_n(xl_cs_n), .xl_miso(xl_miso), .xl_miso_oe(xl_miso_oe)
	);
endmodule
