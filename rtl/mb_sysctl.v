/*
 * machboot
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * System registers at 0x3000_0000 (docs/memory-map.md).
 *
 *   0x00  MAGIC     r   0x4D42_4F54 ("MBOT")
 *   0x04  HWVER     r   register-map version; firmware refuses to run on
 *                       a version it does not know (fw/hw.h, MB_HWVER)
 *   0x08  MS        r   milliseconds since reset
 *   0x0C  CYCLES    r   48 MHz cycle counter, low 32 bits
 *   0x10  LED       rw  bit 0: the board LED
 *   0x14  RECONFIG  w   0x5A52_4254 ("ZRBT") as one whole-word store:
 *                       pull PROGRAMN, and the FPGA loads the next
 *                       configuration (the system slot, docs/boot.md).
 *                   r   { 16'h5A52, 15'b0, avail }
 *   0x18  BOARD     r   board number (docs/memory-map.md, "Boards")
 *   0x1C  FEATURES  r   bit 0 video, 1 USB host, 2 USB device, 3 SD,
 *                       4 RP2040 link, 5 PROGRAMN, 6 installer build
 *                       (the flash fence protects the Zeitlos region, not
 *                       the boot slot: docs/install-mode.md)
 *   0x20  IDCODE    r   the JTAG IDCODE of the FPGA this was built for
 *                       (a synthesis constant; a bitstream for any other
 *                       die is refused before it is written)
 *   0x24  FRAMES    r   video frames since reset (60 per second)
 *   0x28  SLOT      r   system slot address the bitstream was packed to
 *                       boot (ecppack --bootaddr), for display
 *
 * Plain Wishbone classic slave, one wait state.
 */

module mb_sysctl #(
	parameter [31:0] HWVER = 32'd1,
	parameter [31:0] BOARD = 32'd0,
	parameter [31:0] FEATURES = 32'd0,
	parameter [31:0] IDCODE = 32'd0,
	parameter [31:0] SLOT = 32'h0006_0000,
	parameter PROGRAMN_AVAIL = 1
) (
	input wb_clk_i,
	input wb_rst_i,
	input [7:0] wb_adr_i,
	input [31:0] wb_dat_i,
	output reg [31:0] wb_dat_o,
	input [3:0] wb_sel_i,
	input wb_we_i,
	input wb_stb_i,
	input wb_cyc_i,
	output reg wb_ack_o,

	input frame_tog,	// toggles once per frame, pixel-clock domain
	output reg led,
	output reg programn_pull	// 1: pull PROGRAMN low
);

	localparam [31:0] MAGIC = 32'h4D42_4F54;
	localparam [31:0] RECONFIG_KEY = 32'h5A52_4254;

	reg [31:0] cycles = 32'd0;
	reg [31:0] ms = 32'd0;
	reg [15:0] ms_div = 16'd0;
	reg [31:0] frames = 32'd0;

	// the frame toggle into this domain: synchronise, edge-detect
	reg [2:0] fsync = 3'b000;

	wire req = wb_cyc_i && wb_stb_i && !wb_ack_o;

	always @(posedge wb_clk_i) begin
		cycles <= cycles + 32'd1;
		if (ms_div == 16'd47999) begin
			ms_div <= 16'd0;
			ms <= ms + 32'd1;
		end else begin
			ms_div <= ms_div + 16'd1;
		end

		fsync <= {fsync[1:0], frame_tog};
		if (fsync[2] ^ fsync[1])
			frames <= frames + 32'd1;

		wb_ack_o <= 1'b0;

		if (wb_rst_i) begin
			led <= 1'b0;
			programn_pull <= 1'b0;
			cycles <= 32'd0;
			ms <= 32'd0;
			ms_div <= 16'd0;
		end else if (req) begin
			wb_ack_o <= 1'b1;
			if (wb_we_i) begin
				case (wb_adr_i[7:2])
					6'h04: if (wb_sel_i[0]) led <= wb_dat_i[0];
					6'h05: if (wb_sel_i == 4'b1111 && wb_dat_i == RECONFIG_KEY && PROGRAMN_AVAIL)
						programn_pull <= 1'b1;
					default: ;
				endcase
			end else begin
				case (wb_adr_i[7:2])
					6'h00: wb_dat_o <= MAGIC;
					6'h01: wb_dat_o <= HWVER;
					6'h02: wb_dat_o <= ms;
					6'h03: wb_dat_o <= cycles;
					6'h04: wb_dat_o <= {31'd0, led};
					6'h05: wb_dat_o <= {16'h5A52, 15'd0, (PROGRAMN_AVAIL ? 1'b1 : 1'b0)};
					6'h06: wb_dat_o <= BOARD;
					6'h07: wb_dat_o <= FEATURES;
					6'h08: wb_dat_o <= IDCODE;
					6'h09: wb_dat_o <= frames;
					6'h0A: wb_dat_o <= SLOT;
					default: wb_dat_o <= 32'd0;
				endcase
			end
		end
	end

endmodule
