/*
 * machboot
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Behavioural stand-ins for the ECP5 primitives the SoC uses, for
 * simulation only (sim/).
 *
 * ODDRX1F: D0 is driven while SCLK is high after the rising edge, D1
 * while it is low -- two bits per clock, D0 first (Lattice TN1262).
 */
module ODDRX1F (input SCLK, input RST, input D0, input D1, output Q);
	reg q0 = 1'b0, q1 = 1'b0;
	always @(posedge SCLK) begin
		q0 <= D0;
		q1 <= D1;
	end
	assign Q = SCLK ? q0 : q1;
endmodule
