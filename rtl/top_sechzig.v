/*
 * machboot
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Board top: a Sechzig module (ML0, ML1, ML2) in a Sergei or Mozart
 * carrier. The same top for all six; the build picks:
 *
 *   MB_CARRIER  2 Sergei, 3 Mozart        (docs/memory-map.md, "Boards")
 *   MB_MODULE   0, 1, 2                   ML0: LFE5U-25F, ML1/ML2: LFE5U-45F
 *   MB_DIE_45F  defined for ML1 and ML2
 *
 * Pins: boards/<carrier>_ml<n>.lpf, taken from the Zeitlos tree's
 * boards/*.lpf (from the carriers' and modules' netlists); ML0 uses the
 * ML1 file, the same balls on a smaller die (Zeitlos docs/boards.md).
 *
 * The carrier's USB-C socket belongs to its RP2040 (JTAG, the UART), so
 * there is no USB device here. The module's flash holds the installed
 * system at 0 and the Zeitlos region at 0x100000, as it does without
 * machboot; this gateware never writes it: the fence
 * locks all of it. It is only ever loaded into SRAM, by openFPGALoader
 * or by the RP2040 at power-on (docs/sechzig.md).
 */

`ifndef MB_CARRIER
`define MB_CARRIER 2
`endif
`ifndef MB_MODULE
`define MB_MODULE 2
`endif

module top_sechzig (
	input CLK_48,
	output LED_B,

	output DDMI_CK_P,
	output DDMI_D0_P,
	output DDMI_D1_P,
	output DDMI_D2_P,

	output CSPI_SS_FLASH,
	output CSPI_MOSI,
	input CSPI_MISO,

	output UART0_TX,
	input UART0_RX,

	output SD_SS,
	output SD_SCK,
	output SD_MOSI,
	input SD_MISO,

	inout [1:0] usb_host_dp,
	inout [1:0] usb_host_dm,

`ifdef MB_DDR3_PARK
	// ML2's DDR3, which machboot does not use: held in reset while the
	// menu runs (RESET# and CKE low), so the next system finds it as just
	// after power-on (docs/systems.md, "DDR3")
	output ddr3_reset_n,
	output ddr3_cke,
`endif

	output PROGRAMN,

	// the XA-XD link to the RP2040 (docs/xlink.md): the RP2040's SPI1
	input XA,		// MOSI (RP2040 GP11, SPI1 TX)
	input XB,		// SCK  (RP2040 GP10)
	inout XC,		// MISO (RP2040 GP8, SPI1 RX): driven only while CS is low
	input XD		// CS   (RP2040 GP9, a GPIO)
);

`ifdef MB_DIE_45F
	localparam [31:0] IDCODE = 32'h4111_2043;	// LFE5U-45F
`else
	localparam [31:0] IDCODE = 32'h4111_1043;	// LFE5U-25F
`endif
	localparam [31:0] BOARD = (`MB_MODULE << 8) | `MB_CARRIER;

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
	wire xl_miso, xl_miso_oe;

	mb_soc #(
		.RAM_INIT("build/bootrom-sram.hex"),
		.RAM_PRELOAD("build/fw-sechzig-ram.hex"),	// fw/xlink.c in place of fw/usbd.c
		.FONT_FILE("build/font.hex"),
		.BOARD(BOARD),
		.IDCODE(IDCODE),
		.SLOT(32'h0000_0000),		// the module flash's system: at 0, as at power-on
		.FLASH_LOCK_END(24'h00_0000),
		.FLASH_WIN_START(24'h00_0000),
		.FLASH_WIN_END(24'h00_0000),
		.FLASH_LOCK_FROM(25'h000_0000),	// the whole flash locked
		.PROGRAMN_AVAIL(1),
		.USB_PORTS(2)
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
		.uart_tx(UART0_TX),
		.uart_rx(UART0_RX),
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
		.usbd_dp_rx(1'b0),
		.usbd_dn_rx(1'b0),
		.led(led),
		.programn_pull(programn_pull),
		.xl_sck(XB),
		.xl_mosi(XA),
		.xl_cs_n(XD),
		.xl_miso(xl_miso),
		.xl_miso_oe(xl_miso_oe)
	);

	assign XC = xl_miso_oe ? xl_miso : 1'bz;

	USRMCLK usrmclk (.USRMCLKI(flash_sck), .USRMCLKTS(1'b0));

	assign LED_B = led;
`ifdef MB_DDR3_PARK
	assign ddr3_reset_n = 1'b0;
	assign ddr3_cke = 1'b0;
`endif

	// PROGRAMN is OPENDRAIN=ON in the .lpf: 0 pulls it low, 1 releases it.
	// Pulled, the FPGA reconfigures from the module flash at 0: the
	// installed system, exactly as at power-on.
	assign PROGRAMN = programn_pull ? 1'b0 : 1'b1;

endmodule
