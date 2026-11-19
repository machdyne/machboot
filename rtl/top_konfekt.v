/*
 * machboot
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Board top: Machdyne Konfekt (ECP5-12F, CABGA256): a Lakritz with one
 * USB-A socket and no PMOD, with the same flash layout (tinydfu or
 * machboot at 0). rtl/top_lakritz.v is the model; what differs:
 *
 *   - one USB host port (USB_PORTS 1; the controller tells the firmware);
 *   - no serial pads in use: the console is the USB-C port's CDC-ACM
 *     (the debug UART's pads exist, J1/J2, but are not routed out by
 *     default -- Zeitlos boards/konfekt_v0.lpf); the UART is kept inside
 *     and its input held idle;
 *   - the RGB LED: blue (E1) is ours.
 *
 * Pins: boards/konfekt.lpf, from the Zeitlos tree's boards/konfekt_v0.lpf,
 * which was checked ball by ball against the schematic.
 *
 * MB_SRAM_TEST: the JTAG test build (make sram, docs/testing.md) -- the
 * firmware is preloaded into RAM, so the menu runs from an SRAM load
 * with nothing of machboot's in flash.
 */

module top_konfekt (
	input CLK_48,
	output LED_B,

	output DDMI_CK_P,
	output DDMI_D0_P,
	output DDMI_D1_P,
	output DDMI_D2_P,

	output CSPI_SS_FLASH,
	output CSPI_MOSI,
	input CSPI_MISO,

	output SD_SS,
	output SD_SCK,
	output SD_MOSI,
	input SD_MISO,

	inout [0:0] usb_host_dp,
	inout [0:0] usb_host_dm,

	// the USB-C socket (docs/usb-device.md)
	inout usb_ufp_dp,
	inout usb_ufp_dm,
	inout usb_ufp_pull,

	output PROGRAMN
);

	localparam [31:0] IDCODE = 32'h2111_1043;	// LFE5U-12F

	// -- clocks and reset ------------------------------------------------------

	wire clk = CLK_48;
	wire sclk;
	wire pclk;
	wire pll_locked;

	mb_pll pll (
		.clkin(CLK_48),
		.clkout0(sclk),
		.clkout1(pclk),
		.locked(pll_locked)
	);

	// about 1.4 ms of reset after configuration
	reg [16:0] por = 17'd0;
	wire rst = !por[16];
	always @(posedge clk)
		if (!por[16])
			por <= por + 17'd1;

	// -- the SoC -------------------------------------------------------------------

	wire flash_sck;
	wire led;
	wire programn_pull;
	wire usbd_tx_en, usbd_dp_tx, usbd_dn_tx, usbd_pu_on;

	mb_soc #(
`ifdef MB_SRAM_TEST
		.RAM_INIT("build/bootrom-sram.hex"),
		.RAM_PRELOAD("build/fw-ram.hex"),
`else
		.RAM_INIT("build/bootrom.hex"),
`endif
		.FONT_FILE("build/font.hex"),
		.BOARD(32'd5),			// docs/memory-map.md, "Boards"
		.IDCODE(IDCODE),
		.SLOT(32'h0006_0000),
		.FLASH_LOCK_END(24'h06_0000),
`ifdef MB_INSTALLER
		.INSTALLER(1),
`endif
		.PROGRAMN_AVAIL(1),
		.USB_PORTS(1)
	) soc (
		.clk(clk),
		.rst(rst),
		.pclk(pclk),
		.sclk(sclk),
		.dvi_d0(DDMI_D0_P),
		.dvi_d1(DDMI_D1_P),
		.dvi_d2(DDMI_D2_P),
		.dvi_ck(DDMI_CK_P),
		.flash_ss(CSPI_SS_FLASH),
		.flash_sck(flash_sck),
		.flash_mosi(CSPI_MOSI),
		.flash_miso(CSPI_MISO),
		.uart_tx(),
		.uart_rx(1'b1),
		.sd_ss(SD_SS),
		.sd_sck(SD_SCK),
		.sd_mosi(SD_MOSI),
		.sd_miso(SD_MISO),
		.usb_dp(usb_host_dp),
		.usb_dm(usb_host_dm),
		.usbd_tx_en(usbd_tx_en),
		.usbd_dp_tx(usbd_dp_tx),
		.usbd_dn_tx(usbd_dn_tx),
		.usbd_pu_on(usbd_pu_on),
		.usbd_dp_rx(usb_ufp_dp),
		.usbd_dn_rx(usb_ufp_dm),
		.led(led),
		.programn_pull(programn_pull),
		.xl_sck(1'b0),
		.xl_mosi(1'b0),
		.xl_cs_n(1'b1),
		.xl_miso(),
		.xl_miso_oe()
	);

	// The configuration flash's clock pin is a dedicated one; user logic
	// reaches it only through USRMCLK (Lattice TN1260, "SPI flash
	// access after configuration").
	USRMCLK usrmclk (.USRMCLKI(flash_sck), .USRMCLKTS(1'b0));

	assign LED_B = led;

	// The USB-C socket: one driver each, as in Zeitlos rtl/usb_cdc_uart.v.
	// D+/D- are inputs except while the SIE transmits; the pull-up switch
	// is driven high or released, never driven low.
	assign usb_ufp_dp = usbd_tx_en ? usbd_dp_tx : 1'bz;
	assign usb_ufp_dm = usbd_tx_en ? usbd_dn_tx : 1'bz;
	assign usb_ufp_pull = usbd_pu_on ? 1'b1 : 1'bz;

	// PROGRAMN is OPENDRAIN=ON in the .lpf: 0 pulls it low, 1 releases it.
	assign PROGRAMN = programn_pull ? 1'b0 : 1'b1;

endmodule
