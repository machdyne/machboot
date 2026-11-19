/*
 * machboot
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * rtl/mb_xlink.v on its own, in CXXRTL: an SPI master (the RP2040, as
 * rp2040/src/xlink.c drives it) against the slave at the system clock,
 * with time in picoseconds, so SCK's edges fall between the 48 MHz
 * clock's as they will on the board. The master samples MISO at SCK's
 * rising edge, plus a pad delay on the FPGA's output.
 *
 *   build/xlink_tb [SCK_MHZ]       default 8.93 (what the RP2040 uses); it also tries others
 *
 * Checks: STATUS, SEND (and a SEND into a full buffer, ignored), RECV
 * of a reply (whole, and cut short: still armed), 2048-byte messages
 * both ways, and a few hundred random exchanges.
 */

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <vector>
#include "xlink_tb_rtl.cc"

using namespace cxxrtl_design;

static p_mb__xlink top;
static uint64_t now_ps;
static const uint64_t CLK_PS = 20833;		// 48 MHz
static const uint64_t PAD_PS = 6000;		// FPGA output pad + board
static uint64_t next_clk = CLK_PS;
static int fails, quiet;

// MISO as the master sees it: the value after the pad delay
static int miso_now, miso_prev;
static uint64_t miso_changed_at;

static void clock_once(void)
{
	top.p_clk.set<bool>(false);
	top.step();
	top.p_clk.set<bool>(true);
	top.step();
	int m = top.p_x__miso.get<int>();
	if (m != miso_now) {
		miso_prev = miso_now;
		miso_now = m;
		miso_changed_at = now_ps;
	}
}

// let time pass to t, clocking the design at each 48 MHz edge on the way
static void advance(uint64_t t)
{
	while (next_clk <= t) {
		now_ps = next_clk;
		clock_once();
		next_clk += CLK_PS;
	}
	now_ps = t;
}

static int miso_seen(void)
{
	return (now_ps >= miso_changed_at + PAD_PS) ? miso_now : miso_prev;
}

// -- the CPU's side: one Wishbone access at a time ----------------------------

static uint32_t wb(uint32_t adr, int we, uint32_t dat, int sel = 0xf)
{
	top.p_wb__adr__i.set<uint32_t>(0x70000000u | adr);
	top.p_wb__dat__i.set<uint32_t>(dat);
	top.p_wb__we__i.set<bool>(we);
	top.p_wb__sel__i.set<uint32_t>(sel);
	top.p_wb__stb__i.set<bool>(true);
	top.p_wb__cyc__i.set<bool>(true);
	for (int i = 0; i < 10; i++) {
		advance(next_clk);
		if (top.p_wb__ack__o.get<bool>())
			break;
	}
	uint32_t r = top.p_wb__dat__o.get<uint32_t>();
	top.p_wb__stb__i.set<bool>(false);
	top.p_wb__cyc__i.set<bool>(false);
	advance(next_clk);
	return r;
}

// -- the master's side ----------------------------------------------------------

static uint64_t half_ps;

static std::vector<uint8_t> frame(const std::vector<uint8_t> &out)
{
	std::vector<uint8_t> in;
	top.p_x__cs__n.set<bool>(false);
	advance(now_ps + 1000000);			// 1 us: the RP2040's GPIO, then its SPI
	for (uint8_t b : out) {
		uint8_t r = 0;
		for (int i = 7; i >= 0; i--) {
			top.p_x__mosi.set<bool>((b >> i) & 1);
			advance(now_ps + half_ps);
			top.p_x__sck.set<bool>(true);
			r = (uint8_t)(r << 1 | miso_seen());	// sampled at the rising edge
			advance(now_ps + half_ps);
			top.p_x__sck.set<bool>(false);
		}
		in.push_back(r);
	}
	advance(now_ps + half_ps);
	top.p_x__cs__n.set<bool>(true);
	advance(now_ps + 500000);
	return in;
}

static void check(int ok, const char *what)
{
	if (!ok) {
		if (!quiet)
			printf("  FAIL %s\n", what);
		fails++;
	}
}

struct status_t { uint8_t sync, flags, state; uint16_t txlen; uint8_t rxseq; };

static status_t status(void)
{
	std::vector<uint8_t> r = frame({0x01, 0, 0, 0, 0, 0});
	status_t s = { r[0], r[1], r[2], (uint16_t)(r[3] | (r[4] & 7) << 8), r[5] };
	return s;
}

static void send(const std::vector<uint8_t> &msg)
{
	std::vector<uint8_t> f = {0x02};
	f.insert(f.end(), msg.begin(), msg.end());
	frame(f);
}

static std::vector<uint8_t> recv(int n)
{
	std::vector<uint8_t> f(n + 1, 0);
	f[0] = 0x03;
	std::vector<uint8_t> r = frame(f);
	return std::vector<uint8_t>(r.begin() + 1, r.end());
}

// the CPU takes the message in the receive buffer
static std::vector<uint8_t> cpu_take(void)
{
	uint32_t rx = wb(0x0004, 0, 0);
	std::vector<uint8_t> m;
	if (!(rx & 1))
		return m;
	int len = (rx >> 16) & 0x7ff;
	if (!len) len = 2048;
	for (int i = 0; i < len; i += 4) {
		uint32_t w = wb(0x1000 + i, 0, 0);
		for (int k = 0; k < 4 && i + k < len; k++)
			m.push_back((uint8_t)(w >> (8 * k)));
	}
	wb(0x0004, 1, 1);
	return m;
}

static void cpu_reply(const std::vector<uint8_t> &r)
{
	for (size_t i = 0; i < r.size(); i++)		// byte writes, as the firmware's copy may do
		wb(0x1800 + (uint32_t)(i & ~3u), 1, (uint32_t)r[i] << (8 * (i & 3)), 1 << (i & 3));
	wb(0x0008, 1, (1u << 31) | (uint32_t)(r.size() & 0x7ff));
}

static std::vector<uint8_t> rnd(int n)
{
	std::vector<uint8_t> v(n);
	for (auto &b : v) b = (uint8_t)rand();
	return v;
}

static int run(double mhz, int rounds)
{
	fails = 0;
	half_ps = (uint64_t)(500000.0 / mhz);
	top.p_rst.set<bool>(true);
	top.p_x__cs__n.set<bool>(true);
	top.p_x__sck.set<bool>(false);
	advance(now_ps + 10 * CLK_PS);
	top.p_rst.set<bool>(false);
	advance(now_ps + 10 * CLK_PS);

	check(wb(0x0000, 0, 0) == 0x4B4E4C58, "MAGIC");
	wb(0x000C, 1, 0xA7);
	status_t s = status();
	check(s.sync == 0x5A, "STATUS: sync byte 0x5A");
	check(s.flags == 0xA7, "STATUS: flags");
	check(s.state == 0, "STATUS: nothing pending");

	std::vector<uint8_t> m1 = rnd(100);
	send(m1);
	s = status();
	check(s.state == 1 && s.rxseq == 1, "SEND: the buffer is full, RXSEQ 1");
	send(rnd(50));					// into a full buffer: ignored
	check(status().rxseq == 1, "SEND into a full buffer: ignored");
	check(cpu_take() == m1, "the CPU reads the message, byte for byte");
	check(status().state == 0, "taken: empty again");

	std::vector<uint8_t> r1 = rnd(1540);
	cpu_reply(r1);
	s = status();
	check(s.state == 2 && s.txlen == 1540, "a reply of 1540 bytes is ready");
	std::vector<uint8_t> part = recv(700);
	check(part == std::vector<uint8_t>(r1.begin(), r1.begin() + 700), "RECV cut short: the first 700 bytes");
	check(status().state == 2, "RECV cut short: still armed");
	check(recv(1540) == r1, "RECV: the whole reply");
	check(status().state == 0, "RECV whole: taken");

	std::vector<uint8_t> big = rnd(2048);
	send(big);
	check(cpu_take() == big, "a 2048-byte message");
	cpu_reply(big);
	check(status().txlen == 0, "a 2048-byte reply: TXLEN 0 means 2048");
	check(recv(2048) == big, "a 2048-byte reply");

	for (int i = 0; i < rounds && !fails; i++) {
		std::vector<uint8_t> q = rnd(1 + rand() % 600), a = rnd(1 + rand() % 1600);
		while (status().state & 1) ;
		send(q);
		if (cpu_take() != q) { check(0, "random: request"); break; }
		cpu_reply(a);
		while (!(status().state & 2)) ;
		if (recv((int)a.size()) != a) { check(0, "random: reply"); break; }
	}
	return fails;
}

int main(int argc, char **argv)
{
	double mhz = argc > 1 ? atof(argv[1]) : 8.9286;	/* the RP2040's 125 MHz / 14 */
	int f = run(mhz, 300);
	printf("%s xlink at %.2f MHz SCK (system 48 MHz): %d failures\n", f ? "FAIL" : "ok  ", mhz, f);
	/* how far it goes: the faster RP2040 dividers, for docs/xlink.md */
	if (argc <= 1)
		for (double m : {6.25, 10.4167, 12.5, 15.625}) {
			quiet = 1;
			int g = run(m, 20);
			printf("     at %.2f MHz: %s\n", m, g ? "fails" : "works");
		}
	return f ? 1 : 0;
}
