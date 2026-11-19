/*
 * machboot
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * TMDS encoder, one channel.
 *
 * Written from the DVI 1.0 specification, section 3.2 (the encode
 * algorithm and its flow chart) and 3.3.3 (the four control tokens).
 * Plain Verilog, no functions, no local declarations: every
 * intermediate is a named wire at module scope.
 *
 * The output word is transmitted LSB first (q[0] first), as section
 * 3.2.1 specifies. One pixel per clock; output registered.
 */

module mb_tmds (
	input clk,
	input de,		// 1: video data period; 0: control period
	input [7:0] d,		// pixel component
	input [1:0] c,		// control bits (c1, c0), sent while de is 0
	output reg [9:0] q
);

	// -- stage 1: transition-minimised 9-bit word q_m -----------------

	wire [3:0] n1_d = d[0] + d[1] + d[2] + d[3] + d[4] + d[5] + d[6] + d[7];
	wire use_xnor = (n1_d > 4'd4) || (n1_d == 4'd4 && d[0] == 1'b0);

	wire [8:0] q_m;
	assign q_m[0] = d[0];
	assign q_m[1] = use_xnor ? ~(q_m[0] ^ d[1]) : (q_m[0] ^ d[1]);
	assign q_m[2] = use_xnor ? ~(q_m[1] ^ d[2]) : (q_m[1] ^ d[2]);
	assign q_m[3] = use_xnor ? ~(q_m[2] ^ d[3]) : (q_m[2] ^ d[3]);
	assign q_m[4] = use_xnor ? ~(q_m[3] ^ d[4]) : (q_m[3] ^ d[4]);
	assign q_m[5] = use_xnor ? ~(q_m[4] ^ d[5]) : (q_m[4] ^ d[5]);
	assign q_m[6] = use_xnor ? ~(q_m[5] ^ d[6]) : (q_m[5] ^ d[6]);
	assign q_m[7] = use_xnor ? ~(q_m[6] ^ d[7]) : (q_m[6] ^ d[7]);
	assign q_m[8] = ~use_xnor;

	// -- stage 2: DC balance --------------------------------------------

	wire [3:0] n1_q = q_m[0] + q_m[1] + q_m[2] + q_m[3] +
	                  q_m[4] + q_m[5] + q_m[6] + q_m[7];
	wire [3:0] n0_q = 4'd8 - n1_q;

	// signed differences, 6 bits is plenty: |cnt| never exceeds 10
	wire signed [5:0] diff10 = $signed({2'b00, n1_q}) - $signed({2'b00, n0_q});	// n1 - n0
	wire signed [5:0] diff01 = $signed({2'b00, n0_q}) - $signed({2'b00, n1_q});	// n0 - n1

	reg signed [5:0] cnt = 6'sd0;

	wire cnt_zero = (cnt == 6'sd0);
	wire balanced = (n1_q == n0_q);
	wire cnt_pos  = (cnt > 6'sd0);
	wire cnt_neg  = (cnt < 6'sd0);

	wire signed [5:0] two_qm8  = q_m[8] ? 6'sd2 : 6'sd0;
	wire signed [5:0] two_nqm8 = q_m[8] ? 6'sd0 : 6'sd2;

	always @(posedge clk) begin
		if (!de) begin
			cnt <= 6'sd0;
			case (c)
				2'b00: q <= 10'b1101010100;
				2'b01: q <= 10'b0010101011;
				2'b10: q <= 10'b0101010100;
				default: q <= 10'b1010101011;
			endcase
		end else if (cnt_zero || balanced) begin
			q[9] <= ~q_m[8];
			q[8] <= q_m[8];
			q[7:0] <= q_m[8] ? q_m[7:0] : ~q_m[7:0];
			cnt <= q_m[8] ? (cnt + diff10) : (cnt + diff01);
		end else if ((cnt_pos && (n1_q > n0_q)) || (cnt_neg && (n0_q > n1_q))) begin
			q[9] <= 1'b1;
			q[8] <= q_m[8];
			q[7:0] <= ~q_m[7:0];
			cnt <= cnt + two_qm8 + diff01;
		end else begin
			q[9] <= 1'b0;
			q[8] <= q_m[8];
			q[7:0] <= q_m[7:0];
			cnt <= cnt - two_nqm8 + diff10;
		end
	end

endmodule
