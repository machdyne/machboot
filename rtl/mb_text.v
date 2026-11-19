/*
 * machboot
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Text-mode display: 80 x 30 cells of 8 x 16 pixels, 640 x 480 at
 * 60 Hz (800 x 525 total, 25.2 MHz pixel clock), 16 colours.
 *
 * -- The cell --
 *
 *   bits 7:0    character (machboot code page, docs/font.md)
 *   bits 11:8   foreground colour
 *   bits 15:12  background colour
 *
 * The CPU writes cells (write-only; firmware keeps its own copy for the
 * serial mirror, fw/screen.c). Cell n is row n/80, column n%80.
 *
 * -- Timing (VESA DMT 640x480@60) --
 *
 *   horizontal  640 visible, 16 front porch, 96 sync, 48 back porch
 *   vertical    480 visible, 10 front porch,  2 sync, 33 back porch
 *   both syncs negative.
 *
 * -- Pipeline --
 *
 *   stage 0  counters; cell address from them
 *   stage 1  cell read from text RAM
 *   stage 2  glyph row read from the font ROM
 *   stage 3  pixel picked, colour looked up, outputs registered
 *
 * Sync and enable are delayed through the same three stages.
 */

module mb_text #(
	parameter FONT_FILE = "build/font.hex"
) (
	// CPU side
	input sys_clk,
	input [11:0] wr_addr,
	input [15:0] wr_data,
	input wr_en,

	// video side
	input pclk,
	output reg [7:0] red,
	output reg [7:0] green,
	output reg [7:0] blue,
	output reg de,
	output reg hsync,
	output reg vsync,
	output reg frame_tog		// toggles at the start of each frame
);

	reg [15:0] tram [0:4095];

	initial frame_tog = 1'b0;
	reg [7:0] font [0:4095];

	// The text RAM has no initial contents (it would cost bitstream,
	// docs/building.md); the boot ROM clears it first thing.
	initial $readmemh(FONT_FILE, font);

	always @(posedge sys_clk)
		if (wr_en)
			tram[wr_addr] <= wr_data;

	// -- stage 0 -----------------------------------------------------------

	reg [9:0] hc = 10'd0;
	reg [9:0] vc = 10'd0;

	always @(posedge pclk) begin
		if (hc == 10'd799) begin
			hc <= 10'd0;
			if (vc == 10'd524)
				vc <= 10'd0;
			else
				vc <= vc + 10'd1;
		end else begin
			hc <= hc + 10'd1;
		end
	end

	wire [4:0] row = vc[8:4];
	wire [11:0] cell = {1'b0, row, 6'b000000} + {3'b000, row, 4'b0000} + {5'b00000, hc[9:3]};

	wire vis0 = (hc < 10'd640) && (vc < 10'd480);
	wire hs0  = !((hc >= 10'd656) && (hc < 10'd752));
	wire vs0  = !((vc >= 10'd490) && (vc < 10'd492));
	wire fr0  = (hc == 10'd0) && (vc == 10'd0);

	// -- stage 1 -----------------------------------------------------------

	reg [15:0] cell1 = 16'd0;
	reg [2:0] x1 = 3'd0;
	reg [3:0] gy1 = 4'd0;
	reg vis1 = 1'b0, hs1 = 1'b1, vs1 = 1'b1, fr1 = 1'b0;

	always @(posedge pclk) begin
		cell1 <= tram[cell];
		x1 <= hc[2:0];
		gy1 <= vc[3:0];
		vis1 <= vis0;
		hs1 <= hs0;
		vs1 <= vs0;
		fr1 <= fr0;
	end

	// -- stage 2 -----------------------------------------------------------

	reg [7:0] glyph2 = 8'd0;
	reg [7:0] attr2 = 8'd0;
	reg [2:0] x2 = 3'd0;
	reg vis2 = 1'b0, hs2 = 1'b1, vs2 = 1'b1, fr2 = 1'b0;

	always @(posedge pclk) begin
		glyph2 <= font[{cell1[7:0], gy1}];
		attr2 <= cell1[15:8];
		x2 <= x1;
		vis2 <= vis1;
		hs2 <= hs1;
		vs2 <= vs1;
		fr2 <= fr1;
	end

	// -- stage 3 -----------------------------------------------------------

	wire pix = glyph2[3'd7 - x2];
	wire [3:0] col = pix ? attr2[3:0] : attr2[7:4];

	always @(posedge pclk) begin
		de <= vis2;
		hsync <= hs2;
		vsync <= vs2;
		if (fr2)
			frame_tog <= ~frame_tog;
		if (!vis2) begin
			red <= 8'h00; green <= 8'h00; blue <= 8'h00;
		end else begin
			// the 16-colour palette (docs/font.md, "Colours")
			case (col)
				4'h0: begin red <= 8'h00; green <= 8'h00; blue <= 8'h00; end
				4'h1: begin red <= 8'h00; green <= 8'h00; blue <= 8'hAA; end
				4'h2: begin red <= 8'h00; green <= 8'hAA; blue <= 8'h00; end
				4'h3: begin red <= 8'h00; green <= 8'hAA; blue <= 8'hAA; end
				4'h4: begin red <= 8'hAA; green <= 8'h00; blue <= 8'h00; end
				4'h5: begin red <= 8'hAA; green <= 8'h00; blue <= 8'hAA; end
				4'h6: begin red <= 8'hAA; green <= 8'h55; blue <= 8'h00; end
				4'h7: begin red <= 8'hAA; green <= 8'hAA; blue <= 8'hAA; end
				4'h8: begin red <= 8'h55; green <= 8'h55; blue <= 8'h55; end
				4'h9: begin red <= 8'h55; green <= 8'h55; blue <= 8'hFF; end
				4'hA: begin red <= 8'h55; green <= 8'hFF; blue <= 8'h55; end
				4'hB: begin red <= 8'h55; green <= 8'hFF; blue <= 8'hFF; end
				4'hC: begin red <= 8'hFF; green <= 8'h55; blue <= 8'h55; end
				4'hD: begin red <= 8'hFF; green <= 8'h55; blue <= 8'hFF; end
				4'hE: begin red <= 8'hFF; green <= 8'hFF; blue <= 8'h55; end
				default: begin red <= 8'hFF; green <= 8'hFF; blue <= 8'hFF; end
			endcase
		end
	end

endmodule
