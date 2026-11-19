/*
 * machboot
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * USB device: the board's USB-C socket, full speed. docs/usb-device.md.
 *
 * ulixxe's SIE (rtl/ulixxe/, MIT; from Zeitlos rtl/ext/usb_cdc) does the
 * wire: NRZI, bit stuffing, CRCs, PIDs, data toggles, handshakes. This
 * module puts packet buffers and a register file in front of it, so the
 * firmware can be any device it likes -- machboot's is CDC-ACM plus DFU
 * (fw/usbd.c). Nothing here knows a descriptor or a request.
 *
 * Four endpoints, 0-3, each with a 64-byte OUT buffer and a 64-byte IN
 * buffer. Wishbone, 32-bit words:
 *
 *   0x000  MAGIC     r   "USBD"
 *   0x004  CTRL      rw  bit 0 attach (pull-up on, receiver on),
 *                        bits 14:8 device address
 *   0x008  EVENTS    r   bit 0 bus reset seen; w: 1 clears it
 *                        (a bus reset also clears the address and
 *                        every endpoint's state)
 *   0x00C  FRAME     r   the last SOF's frame number
 *   0x010  EPTYPE    rw  bits 3:0 IN bulk, 7:4 OUT bulk, 11:8 IN
 *                        interrupt (endpoint 0 is control, always)
 *   0x014  TOGGLE    w   bits 3:0 IN, 7:4 OUT: reset to DATA0
 *
 *   0x040 + 16n      endpoint n
 *     +0  OUT        r   bit 0 full, bit 1 it was a SETUP, bits 14:8 length
 *                    w   anything: empty again (the next packet is ACKed)
 *     +4  IN         r   bit 0 armed; w: bits 6:0 length, bit 8 arm
 *                        (armed: the next IN token sends the buffer;
 *                        the host's ACK disarms it)
 *     +8  STALL      rw  bit 0: STALL both directions (a SETUP clears it
 *                        on endpoint 0)
 *
 *   0x400 + 0x80n    endpoint n's OUT buffer, 64 bytes, read
 *   0x440 + 0x80n    endpoint n's IN buffer, 64 bytes, write
 *
 * An OUT packet to a full endpoint is NAKed and not stored; a SETUP is
 * always stored (USB 2.0 8.5.3) and also disarms endpoint 0's IN side.
 * An IN token to an endpoint that is not armed is NAKed; a packet the
 * host does not ACK is sent again from its start.
 *
 * The SIE runs at 48 MHz with 4 samples per bit (clk_gate, one cycle in
 * four) -- the system clock, as in Zeitlos.
 */

module mb_usbd (
	input clk,			// 48 MHz
	input rst,

	input [31:0] wb_adr_i,
	input [31:0] wb_dat_i,
	output reg [31:0] wb_dat_o,
	input [3:0] wb_sel_i,
	input wb_we_i,
	input wb_stb_i,
	input wb_cyc_i,
	output reg wb_ack_o,

	output dp_pu,		// 1: pull D+ up (announce a full-speed device)
	output tx_en,
	output dp_tx,
	output dn_tx,
	input dp_rx,
	input dn_rx
);

	localparam [31:0] MAGIC = 32'h44425355;	// "USBD"

	// -- the 1-in-4 clock enable the SIE wants ------------------------------

	reg [1:0] gate_cnt;
	reg gate;
	always @(posedge clk) begin
		if (rst) begin
			gate_cnt <= 2'd0;
			gate <= 1'b0;
		end else begin
			gate_cnt <= gate_cnt + 2'd1;
			gate <= (gate_cnt == 2'd3);
		end
	end

	// -- registers ----------------------------------------------------------

	reg attach;
	reg [6:0] addr;
	reg ev_reset;
	reg [3:0] in_bulk, out_bulk, in_int;
	reg [3:0] tog_in, tog_out;		// pulses, held until the next gate

	reg [3:0] out_full, out_setup, stall, in_armed;
	reg [6:0] out_len0, out_len1, out_len2, out_len3;
	reg [6:0] in_len0, in_len1, in_len2, in_len3;
	reg seen_reset;

	// -- the SIE --------------------------------------------------------------

	wire bus_reset;
	wire [3:0] endp;
	wire [10:0] frame;
	wire [7:0] out_data;
	wire out_valid, out_err, setup, out_ready;
	wire in_req, in_ready, in_data_ack;
	wire [1:0] ep = endp[1:0];
	wire ep_ok = (endp[3:2] == 2'b00);

	reg [6:0] in_len_ep;
	always @(*) begin
		case (ep)
		2'd0: in_len_ep = in_len0;
		2'd1: in_len_ep = in_len1;
		2'd2: in_len_ep = in_len2;
		default: in_len_ep = in_len3;
		endcase
	end

	reg [6:0] rp;			// IN read pointer, within the packet
	reg [6:0] wp;			// OUT write pointer
	reg rx_setup;			// this OUT transaction began with SETUP
	// Whether an OUT packet is taken is decided at its first byte and
	// kept to its end: the firmware may empty the endpoint while the
	// packet arrives, and the rest of it must not be stored (or ACKed)
	// as if it were the whole packet.
	reg pkt_on;			// an OUT packet's bytes are arriving
	reg pkt_drop;			// ... and it is not being stored
	reg tx_q;			// tx_en a cycle ago
	reg hs_drop;			// a zero-length packet's fate, as the handshake said it
	wire [7:0] in_byte;

	wire in_armed_ep = ep_ok && in_armed[ep];
	wire in_valid = in_armed_ep && ((in_len_ep == 7'd0) || (rp < in_len_ep));
	wire in_zlp = in_armed_ep && (in_len_ep == 7'd0);
	wire in_nak = !in_armed_ep;
	wire busy_now = ep_ok ? (out_full[ep] && !rx_setup) : 1'b1;
	wire drop_now = pkt_on ? pkt_drop : busy_now;
	wire out_nak = drop_now;
	wire eop_drop = pkt_on ? pkt_drop : hs_drop;
	wire stall_ep = ep_ok ? stall[ep] : 1'b0;

	sie #(
		.IN_CTRL_MAXPACKETSIZE(64),
		.IN_BULK_MAXPACKETSIZE(64),
		.IN_INT_MAXPACKETSIZE(64),
		.IN_ISO_MAXPACKETSIZE(8),
		.BIT_SAMPLES(4)
	) sie_i (
		.frame_o(frame),
		.clk_i(clk),
		.rstn_i(~rst),
		.clk_gate_i(gate),
		.bus_reset_o(bus_reset),
		.endp_o(endp),
		.stall_i(stall_ep),
		.out_data_o(out_data),
		.out_valid_o(out_valid),
		.out_err_o(out_err),
		.setup_o(setup),
		.out_ready_o(out_ready),
		.out_nak_i(out_nak),
		.in_req_o(in_req),
		.in_ready_o(in_ready),
		.in_data_ack_o(in_data_ack),
		.in_valid_i(in_valid),
		.in_data_i(in_byte),
		.in_zlp_i(in_zlp),
		.in_nak_i(in_nak),
		.usb_en_i(attach && seen_reset),
		.usb_detach_i(!attach),
		.addr_i(addr),
		.in_bulk_endps_i({12'h000, in_bulk & 4'b1110}),
		.out_bulk_endps_i({12'h000, out_bulk & 4'b1110}),
		.in_int_endps_i({12'h000, in_int & 4'b1110}),
		.out_int_endps_i(16'h0000),
		.in_iso_endps_i(16'h0000),
		.out_iso_endps_i(16'h0000),
		.in_toggle_reset_i({12'h000, tog_in}),
		.out_toggle_reset_i({12'h000, tog_out}),
		.dp_pu_o(dp_pu),
		.tx_en_o(tx_en),
		.dp_tx_o(dp_tx),
		.dn_tx_o(dn_tx),
		.dp_rx_i(dp_rx),
		.dn_rx_i(dn_rx)
	);

	// -- packet buffers ---------------------------------------------------
	//
	// Two simple dual-port memories of 64 words: OUT is written by the
	// USB side a byte at a time and read by the CPU; IN is written by
	// the CPU and read by the USB side. Word index = { endpoint, byte[5:2] }.

	reg [31:0] obuf [0:63];
	reg [31:0] ibuf [0:63];

	wire req = wb_cyc_i && wb_stb_i && !wb_ack_o;
	wire [10:0] off = wb_adr_i[10:0];		// byte offset
	wire cpu_buf = off[10];				// 0x400-0x7FF: the buffers
	wire [5:0] cpu_widx = { off[8:7], off[5:2] };
	wire cpu_in_wr = req && wb_we_i && cpu_buf && off[6];

	always @(posedge clk) begin
		if (cpu_in_wr && wb_sel_i[0]) ibuf[cpu_widx][7:0] <= wb_dat_i[7:0];
		if (cpu_in_wr && wb_sel_i[1]) ibuf[cpu_widx][15:8] <= wb_dat_i[15:8];
		if (cpu_in_wr && wb_sel_i[2]) ibuf[cpu_widx][23:16] <= wb_dat_i[23:16];
		if (cpu_in_wr && wb_sel_i[3]) ibuf[cpu_widx][31:24] <= wb_dat_i[31:24];
	end

	// the USB side's IN read: registered, a cycle after the pointer moves
	// (the SIE takes a byte at most every 32 cycles)
	reg [31:0] in_word;
	always @(posedge clk)
		in_word <= ibuf[{ ep, rp[5:2] }];
	reg [1:0] rp_lane;
	always @(posedge clk)
		rp_lane <= rp[1:0];
	assign in_byte = in_word >> { rp_lane, 3'b000 };

	// the USB side's OUT write
	wire out_byte_in = gate && out_ready && out_valid;
	wire out_byte = out_byte_in && !drop_now && (wp < 7'd64);
	always @(posedge clk) begin
		if (out_byte && wp[1:0] == 2'd0) obuf[{ ep, wp[5:2] }][7:0] <= out_data;
		if (out_byte && wp[1:0] == 2'd1) obuf[{ ep, wp[5:2] }][15:8] <= out_data;
		if (out_byte && wp[1:0] == 2'd2) obuf[{ ep, wp[5:2] }][23:16] <= out_data;
		if (out_byte && wp[1:0] == 2'd3) obuf[{ ep, wp[5:2] }][31:24] <= out_data;
	end

	reg [31:0] obuf_q;
	always @(posedge clk)
		obuf_q <= obuf[cpu_widx];

	// -- the endpoint state ---------------------------------------------------

	wire setup_tok = gate && out_ready && setup && !out_valid && !out_err;
	wire out_eop = gate && out_ready && !setup && !out_valid && !out_err;
	wire out_bad = gate && out_ready && out_err;
	wire in_start = gate && in_ready && in_req;
	wire in_next = gate && in_ready && !in_req && !in_data_ack;
	wire in_acked = gate && in_ready && in_data_ack;

	// CPU writes to the endpoint registers
	wire cpu_reg_wr = req && wb_we_i && !cpu_buf;
	wire ep_reg = (off[10:6] == 5'h01);		// 0x040-0x07F
	wire [1:0] cpu_ep = off[5:4];
	wire wr_out = cpu_reg_wr && ep_reg && off[3:2] == 2'd0;
	wire wr_in = cpu_reg_wr && ep_reg && off[3:2] == 2'd1;
	wire wr_stall = cpu_reg_wr && ep_reg && off[3:2] == 2'd2;

	always @(posedge clk) begin
		if (rst) begin
			attach <= 1'b0;
			addr <= 7'd0;
			ev_reset <= 1'b0;
			seen_reset <= 1'b0;
			in_bulk <= 4'd0;
			out_bulk <= 4'd0;
			in_int <= 4'd0;
			tog_in <= 4'd0;
			tog_out <= 4'd0;
			out_full <= 4'd0;
			out_setup <= 4'd0;
			stall <= 4'd0;
			in_armed <= 4'd0;
			out_len0 <= 7'd0; out_len1 <= 7'd0; out_len2 <= 7'd0; out_len3 <= 7'd0;
			in_len0 <= 7'd0; in_len1 <= 7'd0; in_len2 <= 7'd0; in_len3 <= 7'd0;
			rp <= 7'd0;
			wp <= 7'd0;
			rx_setup <= 1'b0;
			pkt_on <= 1'b0;
			pkt_drop <= 1'b0;
			tx_q <= 1'b0;
			hs_drop <= 1'b0;
		end else if (bus_reset) begin
			addr <= 7'd0;
			ev_reset <= 1'b1;
			seen_reset <= 1'b1;
			out_full <= 4'd0;
			out_setup <= 4'd0;
			stall <= 4'd0;
			in_armed <= 4'd0;
			rp <= 7'd0;
			wp <= 7'd0;
			rx_setup <= 1'b0;
			pkt_on <= 1'b0;
			pkt_drop <= 1'b0;
			if (cpu_reg_wr && off[10:2] == 9'h001) attach <= wb_dat_i[0];
		end else begin
			if (gate) begin
				tog_in <= 4'd0;
				tog_out <= 4'd0;
			end

			// -- USB side --
			if (setup_tok && ep_ok && ep == 2'd0) begin
				wp <= 7'd0;
				pkt_on <= 1'b0;
				rx_setup <= 1'b1;
				out_full[0] <= 1'b0;
				in_armed[0] <= 1'b0;
				stall[0] <= 1'b0;
			end
			// the handshake after an OUT packet starts: what the SIE was
			// told is what happens to a zero-length one
			tx_q <= tx_en;
			if (tx_en && !tx_q)
				hs_drop <= drop_now;
			if (out_byte_in && !pkt_on) begin
				pkt_on <= 1'b1;
				pkt_drop <= busy_now;
			end
			if (out_byte)
				wp <= wp + 7'd1;
			if (out_eop) begin
				pkt_on <= 1'b0;
				if (ep_ok && !eop_drop) begin
					out_full[ep] <= 1'b1;
					out_setup[ep] <= rx_setup;
					case (ep)
					2'd0: out_len0 <= wp;
					2'd1: out_len1 <= wp;
					2'd2: out_len2 <= wp;
					default: out_len3 <= wp;
					endcase
				end
				wp <= 7'd0;
				rx_setup <= 1'b0;
			end
			if (out_bad) begin
				wp <= 7'd0;
				rx_setup <= 1'b0;
				pkt_on <= 1'b0;
			end
			if (in_start)
				rp <= 7'd0;
			if (in_next)
				rp <= rp + 7'd1;
			if (in_acked && ep_ok)
				in_armed[ep] <= 1'b0;

			// -- CPU side (after the USB side: the CPU's word wins a tie) --
			if (cpu_reg_wr && off[10:2] == 9'h001) begin
				attach <= wb_dat_i[0];
				addr <= wb_dat_i[14:8];
			end
			if (cpu_reg_wr && off[10:2] == 9'h002 && wb_dat_i[0])
				ev_reset <= 1'b0;
			if (cpu_reg_wr && off[10:2] == 9'h004) begin
				in_bulk <= wb_dat_i[3:0];
				out_bulk <= wb_dat_i[7:4];
				in_int <= wb_dat_i[11:8];
			end
			if (cpu_reg_wr && off[10:2] == 9'h005) begin
				tog_in <= wb_dat_i[3:0];
				tog_out <= wb_dat_i[7:4];
			end
			if (wr_out) begin
				out_full[cpu_ep] <= 1'b0;
				out_setup[cpu_ep] <= 1'b0;
			end
			if (wr_in) begin
				in_armed[cpu_ep] <= wb_dat_i[8];
				case (cpu_ep)
				2'd0: in_len0 <= wb_dat_i[6:0];
				2'd1: in_len1 <= wb_dat_i[6:0];
				2'd2: in_len2 <= wb_dat_i[6:0];
				default: in_len3 <= wb_dat_i[6:0];
				endcase
			end
			if (wr_stall)
				stall[cpu_ep] <= wb_dat_i[0];
		end
	end

	// -- reads -------------------------------------------------------------

	reg [6:0] out_len_cpu;
	always @(*) begin
		case (cpu_ep)
		2'd0: out_len_cpu = out_len0;
		2'd1: out_len_cpu = out_len1;
		2'd2: out_len_cpu = out_len2;
		default: out_len_cpu = out_len3;
		endcase
	end

	reg rd_buf;
	reg [31:0] rd_reg;
	always @(*) begin
		rd_reg = 32'h0;
		if (ep_reg) begin
			case (off[3:2])
			2'd0: rd_reg = { 17'h0, out_len_cpu, 6'h0, out_setup[cpu_ep], out_full[cpu_ep] };
			2'd1: rd_reg = { 23'h0, in_armed[cpu_ep], 8'h0 };
			2'd2: rd_reg = { 31'h0, stall[cpu_ep] };
			default: rd_reg = 32'h0;
			endcase
		end else begin
			case (off[10:2])
			9'h000: rd_reg = MAGIC;
			9'h001: rd_reg = { 17'h0, addr, 7'h0, attach };
			9'h002: rd_reg = { 31'h0, ev_reset };
			9'h003: rd_reg = { 21'h0, frame };
			9'h004: rd_reg = { 20'h0, in_int, out_bulk, in_bulk };
			default: rd_reg = 32'h0;
			endcase
		end
	end

	reg [31:0] rd_reg_q;
	always @(posedge clk) begin
		if (rst) begin
			wb_ack_o <= 1'b0;
			rd_buf <= 1'b0;
			rd_reg_q <= 32'h0;
		end else begin
			wb_ack_o <= req;
			rd_buf <= cpu_buf && !off[6];
			rd_reg_q <= rd_reg;
		end
	end

	always @(*)
		wb_dat_o = rd_buf ? obuf_q : rd_reg_q;

endmodule
