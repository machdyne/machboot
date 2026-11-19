/*
 * machboot
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The menu's system on chip: one zeitlos32 CPU on a Wishbone classic
 * bus, and the few devices a boot manager needs. Board-independent;
 * a board top (rtl/top_<board>.v) supplies the clocks and the pins.
 *
 *   0x0000_0000  RAM (88 KB block RAM; boot ROM at 0, firmware above)
 *   0x1000_0000  SPI flash: read window, and registers at 0x1F00_0000
 *                (rtl/zeitlos/spiflash.v, from Zeitlos; the write fence
 *                is LOCK_END = the start of the system slot)
 *   0x2000_0000  text cells, write-only, one word per cell
 *   0x3000_0000  system registers (rtl/mb_sysctl.v)
 *   0x4000_0000  USB host (rtl/usb/, from Zeitlos), if MB_USB_HOST
 *   0x5000_0000  SD card: SPI master (rtl/zeitlos/spim.v, from Zeitlos)
 *   0x6000_0000  USB device (rtl/mb_usbd.v on ulixxe's SIE), if MB_USB_DEV
 *   0xF000_0000  UART (rtl/zeitlos/uart.v, from Zeitlos; 16550 layout)
 *
 * Anything else is acknowledged at once and reads as zero, so a stray
 * pointer cannot hang the bus. docs/memory-map.md.
 */

module mb_soc #(
	parameter RAM_INIT = "build/bootrom.hex",
	parameter RAM_PRELOAD = "",		// JTAG test builds: firmware in RAM
	parameter FONT_FILE = "build/font.hex",
	parameter [31:0] BOARD = 32'd0,
	parameter [31:0] IDCODE = 32'd0,
	parameter [31:0] SLOT = 32'h0006_0000,
	parameter [23:0] FLASH_LOCK_END = 24'h06_0000,
	parameter [23:0] FLASH_WIN_START = 24'h05_E000,	// settings (kvlog, 8 KB)
	parameter [23:0] FLASH_WIN_END = 24'h06_0000,
	parameter [24:0] FLASH_LOCK_FROM = 25'h100_0000,	// and everything from here up (0: the whole flash)
	// the installer build (docs/install-mode.md): the fence is turned
	// around -- the boot slot and system slot may be written, the Zeitlos
	// region (0x100000 up) may not
	parameter INSTALLER = 0,
	parameter PROGRAMN_AVAIL = 1,
	parameter USB_PORTS = 2
) (
	input clk,		// 48 MHz system clock
	input rst,		// synchronous, active high
	input pclk,		// 25.2 MHz pixel clock
	input sclk,		// 126 MHz TMDS shift clock

	// DVI lanes (to ODDR outputs inside mb_dvi)
	output dvi_d0,
	output dvi_d1,
	output dvi_d2,
	output dvi_ck,

	// SPI flash (sck goes to USRMCLK in the board top)
	output flash_ss,
	output flash_sck,
	output flash_mosi,
	input flash_miso,

	// serial console
	output uart_tx,
	input uart_rx,

	// SD card, SPI mode
	output sd_ss,
	output sd_sck,
	output sd_mosi,
	input sd_miso,

	// USB host ports
	inout [USB_PORTS-1:0] usb_dp,
	inout [USB_PORTS-1:0] usb_dm,

	// USB device (the USB-C socket); the pads and their tri-states are
	// the board top's (rtl/top_lakritz.v)
	output usbd_tx_en,
	output usbd_dp_tx,
	output usbd_dn_tx,
	output usbd_pu_on,	// 1: the 1.5k pull-up on D+ on
	input usbd_dp_rx,
	input usbd_dn_rx,

	output led,
	output programn_pull,

	// the XA-XD link to the RP2040 (Sergei, Mozart; MB_XLINK): docs/xlink.md
	input xl_sck,
	input xl_mosi,
	input xl_cs_n,
	output xl_miso,
	output xl_miso_oe
);

	localparam [31:0] FEAT_VIDEO = 32'h01;
	localparam [31:0] FEAT_USBH  = 32'h02;
	localparam [31:0] FEAT_SD    = 32'h08;
	localparam [31:0] FEAT_PROGN = 32'h20;
	localparam [31:0] FEAT_INSTALLER = 32'h40;
	localparam [31:0] FEAT_COMMON = FEAT_VIDEO | FEAT_SD | (PROGRAMN_AVAIL ? FEAT_PROGN : 32'h0) |
	                                (INSTALLER ? FEAT_INSTALLER : 32'h0);
	localparam [31:0] FEAT_USBD  = 32'h04;
`ifdef MB_USB_DEV
	localparam [31:0] FEAT_DEV = FEAT_USBD;
`else
	localparam [31:0] FEAT_DEV = 32'h0;
`endif
	localparam [31:0] FEAT_RPLINK = 32'h10;
`ifdef MB_XLINK
	localparam [31:0] FEAT_LINK = FEAT_RPLINK;
`else
	localparam [31:0] FEAT_LINK = 32'h0;
`endif
`ifdef MB_USB_HOST
	localparam [31:0] FEATURES = FEAT_COMMON | FEAT_DEV | FEAT_LINK | FEAT_USBH;
`else
	localparam [31:0] FEATURES = FEAT_COMMON | FEAT_DEV | FEAT_LINK;
`endif
	localparam [23:0] FENCE_END = INSTALLER ? 24'h00_0000 : FLASH_LOCK_END;
	localparam [24:0] FENCE_FROM = INSTALLER ? 25'h010_0000 : FLASH_LOCK_FROM;
	localparam [23:0] FENCE_WIN_START = INSTALLER ? 24'h00_0000 : FLASH_WIN_START;
	localparam [23:0] FENCE_WIN_END = INSTALLER ? 24'h00_0000 : FLASH_WIN_END;

	// -- CPU ------------------------------------------------------------------

	wire [31:0] cpu_adr;
	wire [31:0] cpu_dat_o;
	wire [31:0] cpu_dat_i;
	wire cpu_we;
	wire [3:0] cpu_sel;
	wire cpu_stb;
	wire cpu_ack;
	wire cpu_cyc;
	wire cpu_trap;

	zeitlos32_wb #(
		.PROGADDR_RESET(32'h0000_0000),
		.PROGADDR_IRQ(32'h0000_0010),
		.STACKADDR(32'h0001_6000),	// top of RAM
		.ENABLE_MUL(1),
		.ENABLE_DIV(1),
		.FAST_MUL(0),
		.ENABLE_COUNTERS(1),
		.MASKED_IRQ(32'hffff_ffff)	// no interrupts: the firmware polls
	) cpu (
		.wb_clk_i(clk),
		.wb_rst_i(rst),
		.wbm_adr_o(cpu_adr),
		.wbm_dat_o(cpu_dat_o),
		.wbm_dat_i(cpu_dat_i),
		.wbm_we_o(cpu_we),
		.wbm_sel_o(cpu_sel),
		.wbm_stb_o(cpu_stb),
		.wbm_ack_i(cpu_ack),
		.wbm_cyc_o(cpu_cyc),
		.mtu_base(32'h0000_0000),
		.mem_instr(),
		.irq(32'h0000_0000),
		.eoi(),
		.trap(cpu_trap)
	);

	// -- decode ---------------------------------------------------------------

	wire [3:0] page = cpu_adr[31:28];
	wire cs_ram   = (page == 4'h0);
	wire cs_flash = (page == 4'h1);
	wire cs_text  = (page == 4'h2);
	wire cs_sys   = (page == 4'h3);
`ifdef MB_USB_HOST
	wire cs_usbh  = (page == 4'h4);
`else
	wire cs_usbh  = 1'b0;
`endif
	wire cs_sd    = (page == 4'h5);
`ifdef MB_USB_DEV
	wire cs_usbd  = (page == 4'h6);
`else
	wire cs_usbd  = 1'b0;
`endif
`ifdef MB_XLINK
	wire cs_xlink = (page == 4'h7);
`else
	wire cs_xlink = 1'b0;
`endif
	wire cs_uart  = (page == 4'hF);
	wire cs_none  = !(cs_ram || cs_flash || cs_text || cs_sys || cs_usbh || cs_sd || cs_usbd || cs_xlink || cs_uart);

	wire [31:0] word_adr = {6'd0, cpu_adr[27:2]};

	// -- RAM --------------------------------------------------------------------

	wire [31:0] ram_dat;
	wire ram_ack;

	mb_ram #(
		.ROM_FILE(RAM_INIT),
		.RAM_FILE(RAM_PRELOAD)
	) ram (
		.wb_clk_i(clk),
		.wb_rst_i(rst),
		.wb_adr_i(cpu_adr),
		.wb_dat_i(cpu_dat_o),
		.wb_dat_o(ram_dat),
		.wb_sel_i(cpu_sel),
		.wb_we_i(cpu_we),
		.wb_stb_i(cpu_stb),
		.wb_cyc_i(cpu_cyc && cs_ram),
		.wb_ack_o(ram_ack)
	);

	// -- flash ------------------------------------------------------------------

	wire [31:0] flash_dat;
	wire flash_ack;

	spiflash_wb #(
		.LOCK_END(FENCE_END),
		.LOCK_FROM(FENCE_FROM),
		.WIN_START(FENCE_WIN_START),
		.WIN_END(FENCE_WIN_END)
	) flash (
		.wb_clk_i(clk),
		.wb_rst_i(rst),
		.wb_adr_i({4'h0, cpu_adr[27:0]}),
		.wb_dat_i(cpu_dat_o),
		.wb_dat_o(flash_dat),
		.wb_sel_i(cpu_sel),
		.wb_we_i(cpu_we),
		.wb_cyc_i(cpu_cyc && cs_flash),
		.wb_stb_i(cpu_stb),
		.wb_ack_o(flash_ack),
		.ss(flash_ss),
		.sck(flash_sck),
		.mosi(flash_mosi),
		.miso(flash_miso)
	);

	// -- text display -----------------------------------------------------------

	reg text_ack = 1'b0;
	wire text_req = cpu_cyc && cpu_stb && cs_text && !text_ack;

	always @(posedge clk)
		text_ack <= text_req && !rst;

	wire [7:0] v_r;
	wire [7:0] v_g;
	wire [7:0] v_b;
	wire v_de;
	wire v_hs;
	wire v_vs;
	wire v_frame_tog;

	mb_text #(
		.FONT_FILE(FONT_FILE)
	) text (
		.sys_clk(clk),
		.wr_addr(cpu_adr[13:2]),
		.wr_data(cpu_dat_o[15:0]),
		.wr_en(text_req && cpu_we),
		.pclk(pclk),
		.red(v_r),
		.green(v_g),
		.blue(v_b),
		.de(v_de),
		.hsync(v_hs),
		.vsync(v_vs),
		.frame_tog(v_frame_tog)
	);

	mb_dvi dvi (
		.pclk(pclk),
		.sclk(sclk),
		.red(v_r),
		.green(v_g),
		.blue(v_b),
		.de(v_de),
		.hsync(v_hs),
		.vsync(v_vs),
		.out_d0(dvi_d0),
		.out_d1(dvi_d1),
		.out_d2(dvi_d2),
		.out_ck(dvi_ck)
	);

	// -- system registers ---------------------------------------------------

	wire [31:0] sys_dat;
	wire sys_ack;

	mb_sysctl #(
		.HWVER(32'd2),
		.BOARD(BOARD),
		.FEATURES(FEATURES),
		.IDCODE(IDCODE),
		.SLOT(SLOT),
		.PROGRAMN_AVAIL(PROGRAMN_AVAIL)
	) sys (
		.wb_clk_i(clk),
		.wb_rst_i(rst),
		.wb_adr_i(cpu_adr[7:0]),
		.wb_dat_i(cpu_dat_o),
		.wb_dat_o(sys_dat),
		.wb_sel_i(cpu_sel),
		.wb_we_i(cpu_we),
		.wb_stb_i(cpu_stb),
		.wb_cyc_i(cpu_cyc && cs_sys),
		.wb_ack_o(sys_ack),
		.frame_tog(v_frame_tog),
		.led(led),
		.programn_pull(programn_pull)
	);

	// -- UART ---------------------------------------------------------------------

	wire [31:0] uart_dat;
	wire uart_ack;

	uart_wb uart (
		.wb_clk_i(clk),
		.wb_rst_i(rst),
		.wb_adr_i(word_adr[25:0]),
		.wb_dat_i(cpu_dat_o),
		.wb_dat_o(uart_dat),
		.wb_we_i(cpu_we),
		.wb_sel_i(cpu_sel),
		.wb_stb_i(cpu_stb),
		.wb_cyc_i(cpu_cyc && cs_uart),
		.wb_ack_o(uart_ack),
		.stx_pad_o(uart_tx),
		.srx_pad_i(uart_rx),
		.int_o()
	);

	// -- SD card ---------------------------------------------------------------------

	wire [31:0] sd_dat;
	wire sd_ack;
	wire sd_cs_n;

	// starts at 400 kHz (DIV 59), as card initialisation requires
	spim_wb #(
		.DEFAULT_DIV(8'd59)
	) sd (
		.wb_clk_i(clk),
		.wb_rst_i(rst),
		.wb_adr_i(word_adr),
		.wb_dat_i(cpu_dat_o),
		.wb_dat_o(sd_dat),
		.wb_we_i(cpu_we),
		.wb_sel_i(cpu_sel),
		.wb_stb_i(cpu_stb),
		.wb_ack_o(sd_ack),
		.wb_cyc_i(cpu_cyc && cs_sd),
		.spi_cs_n(sd_cs_n),
		.spi_miso(sd_miso),
		.spi_mosi(sd_mosi),
		.spi_sck(sd_sck),
		.spi_int(1'b1)
	);

	assign sd_ss = sd_cs_n;

	// -- USB host -------------------------------------------------------------------

	wire [31:0] usbh_dat;
	wire usbh_ack;

`ifdef MB_USB_HOST
	usb_host #(
		.PORTS(USB_PORTS),
		.T_MS(32'd48000),
		.T_FRAME(32'd48000)
	) usbh (
		.wb_clk_i(clk),
		.wb_rst_i(rst),
		.wb_adr_i(word_adr[10:0]),
		.wb_dat_i(cpu_dat_o),
		.wb_dat_o(usbh_dat),
		.wb_we_i(cpu_we),
		.wb_sel_i(cpu_sel),
		.wb_stb_i(cpu_stb),
		.wb_ack_o(usbh_ack),
		.wb_cyc_i(cpu_cyc && cs_usbh),
		.usb_dp(usb_dp),
		.usb_dm(usb_dm),
		.curs_max_x(11'd639),
		.curs_max_y(10'd479),
		.curs_x0(),
		.curs_y0(),
		.curs_x1(),
		.curs_y1(),
		.typ0(),
		.typ1(),
		.tx_active_o(),
		.tx_port_o(),
		.line0_o(),
		.hid0_int_o(),
		.hid1_int_o(),
		.int_o()
	);
`else
	assign usbh_dat = 32'd0;
	assign usbh_ack = 1'b0;
	// usb_dp/usb_dm are left undriven: the pads stay inputs.
`endif

	// -- USB device ------------------------------------------------------------------

	wire [31:0] usbd_dat;
	wire usbd_ack;

`ifdef MB_USB_DEV
	mb_usbd usbd (
		.clk(clk),
		.rst(rst),
		.wb_adr_i(cpu_adr),
		.wb_dat_i(cpu_dat_o),
		.wb_dat_o(usbd_dat),
		.wb_sel_i(cpu_sel),
		.wb_we_i(cpu_we),
		.wb_stb_i(cpu_stb),
		.wb_cyc_i(cpu_cyc && cs_usbd),
		.wb_ack_o(usbd_ack),
		.dp_pu(usbd_pu_on),
		.tx_en(usbd_tx_en),
		.dp_tx(usbd_dp_tx),
		.dn_tx(usbd_dn_tx),
		.dp_rx(usbd_dp_rx),
		.dn_rx(usbd_dn_rx)
	);
`else
	assign usbd_dat = 32'd0;
	assign usbd_ack = 1'b0;
	assign usbd_tx_en = 1'b0;
	assign usbd_dp_tx = 1'b0;
	assign usbd_dn_tx = 1'b0;
	assign usbd_pu_on = 1'b0;
`endif

	// -- the XA-XD link -----------------------------------------------------------

	wire [31:0] xlink_dat;
	wire xlink_ack;

`ifdef MB_XLINK
	mb_xlink xlink (
		.clk(clk),
		.rst(rst),
		.wb_adr_i(cpu_adr),
		.wb_dat_i(cpu_dat_o),
		.wb_dat_o(xlink_dat),
		.wb_sel_i(cpu_sel),
		.wb_we_i(cpu_we),
		.wb_stb_i(cpu_stb),
		.wb_cyc_i(cpu_cyc && cs_xlink),
		.wb_ack_o(xlink_ack),
		.x_sck(xl_sck),
		.x_mosi(xl_mosi),
		.x_cs_n(xl_cs_n),
		.x_miso(xl_miso),
		.x_miso_oe(xl_miso_oe)
	);
`else
	assign xlink_dat = 32'd0;
	assign xlink_ack = 1'b0;
	assign xl_miso = 1'b0;
	assign xl_miso_oe = 1'b0;
`endif

	// -- unmapped -------------------------------------------------------------------

	reg none_ack = 1'b0;
	always @(posedge clk)
		none_ack <= cpu_cyc && cpu_stb && cs_none && !none_ack && !rst;

	// -- return path ------------------------------------------------------------

	assign cpu_ack = ram_ack | flash_ack | text_ack | sys_ack | uart_ack | usbh_ack | sd_ack | usbd_ack | xlink_ack | none_ack;

	assign cpu_dat_i =
		cs_ram   ? ram_dat :
		cs_flash ? flash_dat :
		cs_sys   ? sys_dat :
		cs_uart  ? uart_dat :
		cs_usbh  ? usbh_dat :
		cs_sd    ? sd_dat :
		cs_usbd  ? usbd_dat :
		cs_xlink ? xlink_dat :
		32'd0;

endmodule
