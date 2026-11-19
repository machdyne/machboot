/*
 * machboot
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Main RAM: block RAM, 32-bit words, byte write enables, Wishbone
 * classic slave with one wait state.
 *
 * The first 1 KB reads from the boot ROM (bootrom/), a separate
 * 256-word block RAM with initial contents; writes there are ignored.
 * The RAM itself has NO initial contents, on purpose: every block RAM
 * that is initialised costs about 2.3 KB of bitstream, uncompressed
 * (docs/building.md), and this RAM is 44 of them. The boot ROM
 * copies the firmware from flash to 0x400 and jumps to it
 * (docs/boot.md); the firmware clears its own .bss.
 */

module mb_ram #(
	parameter WORDS = 22528,		// 88 KB
	parameter ADDR_BITS = 15,
	parameter ROM_FILE = "build/bootrom.hex",
	// JTAG test builds only (make sram): the firmware preloaded at 0x400,
	// so the menu runs with nothing in flash. Empty: no initial contents.
	parameter RAM_FILE = ""
) (
	input wb_clk_i,
	input wb_rst_i,
	input [31:0] wb_adr_i,
	input [31:0] wb_dat_i,
	output reg [31:0] wb_dat_o,
	input [3:0] wb_sel_i,
	input wb_we_i,
	input wb_stb_i,
	input wb_cyc_i,
	output reg wb_ack_o
);

	reg [31:0] mem [0:WORDS-1];
	reg [31:0] rom [0:255];

	initial $readmemh(ROM_FILE, rom);

	generate
		if (RAM_FILE != "") begin : preload
			initial $readmemh(RAM_FILE, mem);
		end
	endgenerate

	wire [ADDR_BITS-1:0] a = wb_adr_i[ADDR_BITS+1:2];
	wire in_rom = (a[ADDR_BITS-1:8] == 0);
	wire req = wb_cyc_i && wb_stb_i && !wb_ack_o;
	// 88 KB is not a power of two: addresses past the end are not
	// stored (they would wrap in the block RAMs; a simulator stops)
	wire in_mem = (a < WORDS);
	wire wr = req && wb_we_i && !in_rom && in_mem;

	reg [31:0] mem_q;
	reg [31:0] rom_q;
	reg sel_rom;

	always @(posedge wb_clk_i) begin
		if (wr && wb_sel_i[0]) mem[a][7:0]   <= wb_dat_i[7:0];
		if (wr && wb_sel_i[1]) mem[a][15:8]  <= wb_dat_i[15:8];
		if (wr && wb_sel_i[2]) mem[a][23:16] <= wb_dat_i[23:16];
		if (wr && wb_sel_i[3]) mem[a][31:24] <= wb_dat_i[31:24];
		if (in_mem)
			mem_q <= mem[a];
	end

	always @(posedge wb_clk_i) begin
		rom_q <= rom[a[7:0]];
		sel_rom <= in_rom;
	end

	always @(*)
		wb_dat_o = sel_rom ? rom_q : mem_q;

	always @(posedge wb_clk_i) begin
		if (wb_rst_i)
			wb_ack_o <= 1'b0;
		else
			wb_ack_o <= req;
	end

endmodule
