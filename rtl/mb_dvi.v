/*
 * machboot
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * DVI transmitter: three TMDS encoders and a 10:1 serialiser per lane,
 * out through ODDRX1F (two bits per shift clock).
 *
 *   pclk   25.2 MHz   one pixel per clock
 *   sclk  126.0 MHz   5 x pclk; 2 bits per clock -> 252 Mbit/s per lane
 *
 * Both clocks come from the same PLL (rtl/mb_pll.v), so their phase is
 * fixed but not known to this module. Rather than assume it, the shift
 * domain finds it: pclk toggles a flop every pixel, sclk synchronises
 * that toggle through two flops and loads the shift registers on the
 * clock after it sees a change. That lands every load two to three
 * shift clocks (16-24 ns) after the pixel edge, in the middle of the
 * 39.7 ns the pixel-domain hold registers are stable, and it repeats
 * exactly every five shift clocks because the two clocks are locked.
 *
 * Lane order is DVI's: channel 0 blue (carries HSYNC/VSYNC in its
 * control period), 1 green, 2 red. The TMDS clock lane sends
 * 0000011111 per pixel (LSB first: five ones, five zeros).
 */

module mb_dvi (
	input pclk,
	input sclk,

	input [7:0] red,
	input [7:0] green,
	input [7:0] blue,
	input de,		// 1 in the visible area
	input hsync,		// level as it should appear on the wire
	input vsync,

	output out_d0,		// blue
	output out_d1,		// green
	output out_d2,		// red
	output out_ck
);

	wire [9:0] q_b;
	wire [9:0] q_g;
	wire [9:0] q_r;

	mb_tmds tmds_b (.clk(pclk), .de(de), .d(blue),  .c({vsync, hsync}), .q(q_b));
	mb_tmds tmds_g (.clk(pclk), .de(de), .d(green), .c(2'b00),          .q(q_g));
	mb_tmds tmds_r (.clk(pclk), .de(de), .d(red),   .c(2'b00),          .q(q_r));

	// -- pixel domain: hold the words, toggle once per pixel -----------

	reg [9:0] hold_b = 10'd0;
	reg [9:0] hold_g = 10'd0;
	reg [9:0] hold_r = 10'd0;
	reg ptog = 1'b0;

	always @(posedge pclk) begin
		hold_b <= q_b;
		hold_g <= q_g;
		hold_r <= q_r;
		ptog <= ~ptog;
	end

	// -- shift domain ---------------------------------------------------

	reg [2:0] tsync = 3'b000;
	wire load = tsync[2] ^ tsync[1];

	reg [9:0] sh_b = 10'd0;
	reg [9:0] sh_g = 10'd0;
	reg [9:0] sh_r = 10'd0;
	reg [9:0] sh_c = 10'd0;

	always @(posedge sclk) begin
		tsync <= {tsync[1:0], ptog};
		if (load) begin
			sh_b <= hold_b;
			sh_g <= hold_g;
			sh_r <= hold_r;
			sh_c <= 10'b0000011111;
		end else begin
			sh_b <= {2'b00, sh_b[9:2]};
			sh_g <= {2'b00, sh_g[9:2]};
			sh_r <= {2'b00, sh_r[9:2]};
			sh_c <= {2'b00, sh_c[9:2]};
		end
	end

	// D0 goes out on the rising edge, D1 on the falling edge: bit 0
	// first, then bit 1.
	ODDRX1F oddr_b (.SCLK(sclk), .RST(1'b0), .D0(sh_b[0]), .D1(sh_b[1]), .Q(out_d0));
	ODDRX1F oddr_g (.SCLK(sclk), .RST(1'b0), .D0(sh_g[0]), .D1(sh_g[1]), .Q(out_d1));
	ODDRX1F oddr_r (.SCLK(sclk), .RST(1'b0), .D0(sh_r[0]), .D1(sh_r[1]), .Q(out_d2));
	ODDRX1F oddr_c (.SCLK(sclk), .RST(1'b0), .D0(sh_c[0]), .D1(sh_c[1]), .Q(out_ck));

endmodule
