/*
 * machboot
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Whole-system simulation (CXXRTL): the SoC boots its boot ROM, which
 * loads the firmware from a modelled SPI flash; the firmware runs the
 * menu; keys go in over a modelled serial port; the screen is captured
 * from the TMDS lanes, decoded as a DVI receiver would, and saved.
 *
 *   sim FLASH.bin OUTDIR
 *
 * Steps (all must happen, or it fails):
 *   1. the startup screen: serial says "Starting", one frame captured
 *   2. F8: serial says "== Start a system ==", one frame captured
 *   3. Enter: PROGRAMN is pulled
 */

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <string>
#include <vector>
#include <deque>
#include <algorithm>
#include "sim.cc"
extern "C" {
#include "sdmodel.h"
}

using namespace std;

static cxxrtl_design::p_sim__top top;

/* -- clocks: integer picoseconds ---------------------------------------- */

static const uint64_t HALF_CLK = 10417;		/* 48 MHz */
static const uint64_t HALF_SCLK = 3968;		/* 126 MHz */
static const uint64_t HALF_PCLK = HALF_SCLK * 5;	/* 25.2 MHz, locked to sclk */

static uint64_t now_ps, next_clk, next_sclk;
static bool clk_v, sclk_v, pclk_v;
static int sclk_halves;
static bool video_on;
static uint64_t sys_cycles;

/* -- SPI flash (mode 0; READ 03, RDID 9F, RDSR 05) ---------------------- */

static vector<uint8_t> flash;
static bool f_ss_prev = true, f_sck_prev;
static int f_bits;
static uint32_t f_shift, f_cmd, f_addr;
static uint8_t f_out;
static int f_outbit = -1;
static bool f_miso;
static uint32_t f_reads, f_erases, f_programs;
static bool f_wel;			/* write enable latch */
static vector<uint8_t> f_page;		/* bytes of a page program */
static uint32_t f_pp_addr;

static uint8_t f_data_byte()
{
	if (f_cmd == 0x03) {
		uint8_t b = f_addr < flash.size() ? flash[f_addr] : 0xff;
		f_addr++;
		return b;
	}
	if (f_cmd == 0x9f) {
		static const uint8_t id[3] = { 0xEF, 0x40, 0x15 };
		uint8_t b = id[(f_bits / 8 - 1) % 3];
		return b;
	}
	return 0x00;		/* RDSR: not busy */
}

static void flash_tick()
{
	bool ss = top.p_flash__ss.get<bool>();
	bool sck = top.p_flash__sck.get<bool>();
	bool mosi = top.p_flash__mosi.get<bool>();

	if (ss) {
		if (!f_ss_prev) {
			/* the end of a command: commit what it asked for */
			if (f_cmd == 0x06 && f_bits == 8)
				f_wel = true;
			if (f_cmd == 0x20 && f_bits == 32 && f_wel) {
				uint32_t a = f_shift & 0xfff000;
				for (uint32_t i = 0; i < 4096 && a + i < flash.size(); i++)
					flash[a + i] = 0xff;
				f_erases++;
				f_wel = false;
			}
			if (f_cmd == 0x02 && f_wel && f_bits > 32) {
				for (size_t i = 0; i < f_page.size(); i++) {
					uint32_t a = (f_pp_addr & ~0xffu) | ((f_pp_addr + i) & 0xffu);
					if (a < flash.size())
						flash[a] &= f_page[i];	/* NOR */
				}
				f_programs++;
				f_wel = false;
			}
			f_cmd = 0;
		}
		f_bits = 0;
		f_shift = 0;
		f_outbit = -1;
	} else if (f_ss_prev) {
		f_bits = 0;
	} else if (sck && !f_sck_prev) {
		/* rising edge: sample MOSI */
		f_shift = (f_shift << 1) | (mosi ? 1 : 0);
		f_bits++;
		if (f_bits == 8) {
			f_cmd = f_shift & 0xff;
			if (f_cmd == 0x03)
				f_reads++;
		}
		if (f_cmd == 0x03 && f_bits == 32)
			f_addr = f_shift & 0xffffff;
		if (f_cmd == 0x02 && f_bits == 32) {
			f_pp_addr = f_shift & 0xffffff;
			f_page.clear();
		}
		if (f_cmd == 0x02 && f_bits > 32 && (f_bits - 32) % 8 == 0)
			f_page.push_back((uint8_t)(f_shift & 0xff));
	} else if (!sck && f_sck_prev) {
		/* falling edge: present the next data bit */
		bool data_phase = (f_cmd == 0x03 && f_bits >= 32) || (f_cmd == 0x9f && f_bits >= 8) ||
		                  (f_cmd == 0x05 && f_bits >= 8);
		if (data_phase) {
			if (f_outbit < 0) {
				f_out = f_data_byte();
				f_outbit = 7;
			}
			f_miso = (f_out >> f_outbit) & 1;
			f_outbit--;
		}
	}
	f_ss_prev = ss;
	f_sck_prev = sck;
	top.p_flash__miso.set<bool>(f_miso);
}

/* -- SD card: tests/sdmodel.c behind a bit shifter (SPI mode 0) ---------- */

static vector<uint8_t> card;
static bool sd_have, sd_ss_prev = true, sd_sck_prev;
static int sd_bit;
static uint8_t sd_in, sd_cur;

static uint64_t sd_low_cycles;
static void sd_tick()
{
	bool ss = top.p_sd__ss.get<bool>();
	if (!ss) sd_low_cycles++;
	bool sck = top.p_sd__sck.get<bool>();
	bool mosi = top.p_sd__mosi.get<bool>();
	bool miso = true;

	if (!sd_have) {
		top.p_sd__miso.set<bool>(true);
		return;
	}
	if (ss) {
		if (!sd_ss_prev)
			sdm_deselect();
		sd_bit = 0;
	} else {
		if (sd_ss_prev) {
			sd_bit = 0;
			sd_cur = sdm_peek();
		}
		if (sck && !sd_sck_prev) {
			sd_in = (uint8_t)((sd_in << 1) | (mosi ? 1 : 0));
			sd_bit++;
		} else if (!sck && sd_sck_prev && sd_bit == 8) {
			sdm_xchg(sd_in);
			sd_bit = 0;
			sd_cur = sdm_peek();
		}
		miso = (sd_cur >> (7 - sd_bit)) & 1;
	}
	sd_ss_prev = ss;
	sd_sck_prev = sck;
	top.p_sd__miso.set<bool>(miso);
}

/* -- UART, 1 Mbaud = 48 system clocks a bit ----------------------------- */

static string uart_log;
static int u_rx_state = -1, u_rx_cnt;
static uint8_t u_rx_byte;
static bool u_tx_prev = true;
static deque<uint8_t> u_in;
static int u_tx_state = -1, u_tx_cnt;
static uint8_t u_tx_byte;

static void uart_tick()
{
	/* device -> us */
	bool tx = top.p_uart__tx.get<bool>();
	if (u_rx_state < 0) {
		if (u_tx_prev && !tx) {
			u_rx_state = 0;
			u_rx_cnt = 48 + 24;	/* to the middle of bit 0 */
			u_rx_byte = 0;
		}
	} else if (--u_rx_cnt == 0) {
		if (u_rx_state < 8) {
			u_rx_byte |= (tx ? 1 : 0) << u_rx_state;
			u_rx_state++;
			u_rx_cnt = 48;
		} else {
			if (u_rx_byte != '\r') {
				uart_log += (char)u_rx_byte;
				fputc(u_rx_byte, stdout);
			}
			u_rx_state = -1;
		}
	}
	u_tx_prev = tx;

	/* us -> device */
	bool line = true;
	if (u_tx_state < 0 && !u_in.empty()) {
		u_tx_byte = u_in.front();
		u_in.pop_front();
		u_tx_state = 0;
		u_tx_cnt = 48;
	}
	if (u_tx_state >= 0) {
		if (u_tx_state == 0) line = false;
		else if (u_tx_state <= 8) line = (u_tx_byte >> (u_tx_state - 1)) & 1;
		else line = true;
		if (--u_tx_cnt == 0) {
			u_tx_state++;
			u_tx_cnt = 48;
			if (u_tx_state > 10)
				u_tx_state = -1;
		}
	}
	top.p_uart__rx.set<bool>(line);
}

/* -- DVI capture --------------------------------------------------------- */

static vector<uint16_t> lane[4];	/* bits as they leave, LSB first */

static void dvi_sample()
{
	lane[0].push_back(top.p_dvi__d0.get<bool>());
	lane[1].push_back(top.p_dvi__d1.get<bool>());
	lane[2].push_back(top.p_dvi__d2.get<bool>());
	lane[3].push_back(top.p_dvi__ck.get<bool>());
}

static const int CTRL[4] = { 0x354, 0x0AB, 0x154, 0x2AB };	/* c=00,01,10,11 (q[9:0]) */

static int word_at(const vector<uint16_t> &l, size_t i)
{
	int w = 0;
	for (int b = 0; b < 10; b++)
		w |= (l[i + b] & 1) << b;
	return w;
}

static int ctrl_of(int w)
{
	for (int c = 0; c < 4; c++)
		if (w == CTRL[c])
			return c;
	return -1;
}

static int decode(int q)
{
	int d = (q & 0x200) ? (~q & 0xff) : (q & 0xff);
	int o = d & 1;
	for (int i = 1; i < 8; i++) {
		int b = ((d >> i) ^ (d >> (i - 1))) & 1;
		if (!(q & 0x100))
			b ^= 1;
		o |= b << i;
	}
	return o;
}

/* decode the captured lanes into a 640x480 RGB frame; false if it fails */
static bool dvi_frame(const string &path)
{
	size_t n = lane[0].size();
	/* word alignment: the offset at which the clock lane reads 0x01F */
	int off = -1;
	const size_t skip = 1000;	/* the serialiser's first loads */
	for (int o = 0; o < 10 && off < 0; o++) {
		bool good = true;
		for (size_t i = skip + o; i + 10 <= n && i < skip + o + 2000; i += 10)
			if (word_at(lane[3], i) != 0x01F) { good = false; break; }
		if (good) off = o;
	}
	if (off < 0) {
		fprintf(stderr, "dvi: no word alignment on the clock lane; bits:");
		for (size_t i = skip; i < skip + 60 && i < n; i++)
			fprintf(stderr, "%d", lane[3][i]);
		fprintf(stderr, "\n");
		return false;
	}
	vector<uint8_t> img(640 * 480 * 3, 0);
	int y = -1, x = 0;
	bool in_vsync = false, prev_de = false, armed = false;
	int ctrl_err = 0;
	for (size_t i = skip + off; i + 10 <= n; i += 10) {
		int qb = word_at(lane[0], i), qg = word_at(lane[1], i), qr = word_at(lane[2], i);
		int cb = ctrl_of(qb);
		bool de = cb < 0;
		if (!de) {
			if (ctrl_of(qg) != 0 || ctrl_of(qr) != 0)
				ctrl_err++;
			bool vs = !(cb & 2);	/* active low */
			if (vs && !in_vsync)
				armed = true, y = -1;
			in_vsync = vs;
			prev_de = false;
			continue;
		}
		if (!armed)
			continue;
		if (!prev_de) {
			y++;
			x = 0;
		}
		prev_de = true;
		if (y >= 0 && y < 480 && x < 640) {
			img[(y * 640 + x) * 3 + 0] = decode(qr);
			img[(y * 640 + x) * 3 + 1] = decode(qg);
			img[(y * 640 + x) * 3 + 2] = decode(qb);
		}
		x++;
		if (y == 479 && x == 640)
			break;
	}
	if (y < 479) {
		fprintf(stderr, "dvi: only %d lines decoded\n", y + 1);
		return false;
	}
	if (ctrl_err)
		fprintf(stderr, "dvi: %d bad control words on green/red\n", ctrl_err);
	FILE *f = fopen(path.c_str(), "wb");
	fprintf(f, "P6\n640 480\n255\n");
	fwrite(img.data(), 1, img.size(), f);
	fclose(f);
	fprintf(stderr, "[frame %s]\n", path.c_str());
	return ctrl_err == 0;
}

/* -- the USB host on the device port --------------------------------------- */

static void step_one();
#include "usbhost.inc"
#ifdef MB_SIM_XLINK
#include "xlink.inc"

/* the RP2040's main loop alongside: msc_tick every simulated ms */
static uint64_t rp_next_ps;
static void rp_step(void)
{
	step_one();
	if (now_ps >= rp_next_ps) {
		rp_next_ps = now_ps + 1000000000ull;
		msc_tick();
	}
}
static bool rp_run_until(bool (*cond)(), uint64_t max_cycles, const char *what)
{
	uint64_t start = sys_cycles;
	while (!cond()) {
		rp_step();
		if (sys_cycles - start > max_cycles) {
			fprintf(stderr, "\nTIMEOUT waiting for %s\n", what);
			return false;
		}
	}
	return true;
}
static void rp_run_ms(uint32_t ms)
{
	uint64_t end = now_ps + (uint64_t)ms * 1000000000ull;
	while (now_ps < end)
		rp_step();
}

/* what the RP2040 keeps (store.c on the board) */
static string xl_kept = "boot.auto=0\nboot.wait=9\nboot.kbd=us\n";
static bool xl_put, xl_started;
static int xl_settings_get(char *b, int cap)
{
	int n = (int)xl_kept.size() < cap ? (int)xl_kept.size() : cap;
	memcpy(b, xl_kept.data(), (size_t)n);
	return n;
}
static void xl_settings_put(const char *t, int n) { xl_kept.assign(t, (size_t)n); xl_put = true; }
static void xl_start(void) { xl_started = true; }
static const msc_hooks_t xl_hooks = { xl_settings_get, xl_settings_put, xl_start, 0, 0, 0 };
/* the RP2040's system cache (docs/systems.md): empty, in memory */
static uint8_t xl_cache[256 * 1024];
static void xl_ca_erase(uint32_t off, uint32_t len) { memset(xl_cache + off, 0xFF, len); }
static void xl_ca_program(uint32_t off, const uint8_t *p) { for (int i = 0; i < 256; i++) xl_cache[off + i] &= p[i]; }
static const cache_io_t xl_cache_io = { xl_cache, sizeof(xl_cache), xl_ca_erase, xl_ca_program };
static bool xl_got_settings() { return xl_put; }
#endif

/* -- the loop -------------------------------------------------------------- */

static void step_one()
{
	/* the earliest of the next clock edges */
	uint64_t t = next_clk;
	if (video_on && next_sclk < t)
		t = next_sclk;
	now_ps = t;
	bool sys_edge = false, s_edge = false;
	if (next_clk == t) {
		clk_v = !clk_v;
		top.p_clk.set<bool>(clk_v);
		next_clk += HALF_CLK;
		sys_edge = clk_v;
	}
	if (video_on && next_sclk == t) {
		sclk_v = !sclk_v;
		top.p_sclk.set<bool>(sclk_v);
		next_sclk += HALF_SCLK;
		if (++sclk_halves == 5) {
			sclk_halves = 0;
			pclk_v = !pclk_v;
			top.p_pclk.set<bool>(pclk_v);
		}
		s_edge = true;
	}
	top.step();
	if (s_edge && lane[0].capacity())
		dvi_sample();
	if (sys_edge) {
		sys_cycles++;
		flash_tick();
		sd_tick();
		uart_tick();
		uh_tick();
	}
}

static bool run_until(bool (*cond)(), uint64_t max_cycles, const char *what)
{
	uint64_t start = sys_cycles;
	while (!cond()) {
		step_one();
		if (sys_cycles - start > max_cycles) {
			fprintf(stderr, "\nTIMEOUT waiting for %s (%llu cycles); the serial log so far:\n%s\n", what,
			        (unsigned long long)(sys_cycles - start), uart_log.c_str());
			return false;
		}
	}
	return true;
}

static size_t log_mark;
static const char *want;
static bool log_has() { return uart_log.find(want, log_mark) != string::npos; }
static bool reconfig() { return top.p_programn__pull.get<bool>(); }

static bool capture(const string &path)
{
	/* run the video for ~2 frames, keep the samples, decode one */
	video_on = true;
	next_sclk = now_ps + 1;
	for (int l = 0; l < 4; l++) {
		lane[l].clear();
		lane[l].reserve(6000000);
	}
	uint64_t frame_ps = (uint64_t)800 * 525 * HALF_PCLK * 2;
	uint64_t end = now_ps + frame_ps * 2 + frame_ps / 4;
	while (now_ps < end)
		step_one();
	bool ok = dvi_frame(path);
	for (int l = 0; l < 4; l++)
		vector<uint16_t>().swap(lane[l]);
	video_on = false;
	return ok;
}

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: sim FLASH.bin OUTDIR\n");
		return 2;
	}
	FILE *f = fopen(argv[1], "rb");
	if (!f) { perror(argv[1]); return 1; }
	fseek(f, 0, SEEK_END);
	flash.resize(ftell(f));
	fseek(f, 0, SEEK_SET);
	if (fread(flash.data(), 1, flash.size(), f) != flash.size()) return 1;
	fclose(f);
	string out = argv[2];

	next_clk = HALF_CLK;
	top.p_rst.set<bool>(true);
	top.p_uart__rx.set<bool>(true);
	top.p_xl__cs__n.set<bool>(true);
	for (int i = 0; i < 40; i++)
		step_one();
	top.p_rst.set<bool>(false);

	bool ok = true;

	auto load = [](const char *path, vector<uint8_t> &v) {
		FILE *c = fopen(path, "rb");
		if (!c) { perror(path); exit(1); }
		fseek(c, 0, SEEK_END);
		v.resize(ftell(c));
		fseek(c, 0, SEEK_SET);
		if (fread(v.data(), 1, v.size(), c) != v.size()) exit(1);
		fclose(c);
	};

	if (argc > 5 && string(argv[3]) == "--usb") {
		/*
		 * The USB device at the wire: a host enumerates it, opens the
		 * console, downloads a bitstream with DFU into the system slot,
		 * reads the console, and types on it. Checks the SIE, the
		 * endpoint buffers (rtl/mb_usbd.v) and the firmware's stack
		 * (fw/usbd.c) together.
		 */
		vector<uint8_t> before = flash, bit;
		load(argv[4], bit);
		load(argv[5], card);
		sdm_init(card.data(), card.size(), 1);
		sd_have = true;
		vector<uint8_t> card0 = card;
		auto keys = [](const char *s) { for (; *s; s++) u_in.push_back((uint8_t)*s); };
		want = "Starting";
		ok = run_until(log_has, 60000000, "the startup screen");
		keys("\x1b[19~");
		want = "== Start a system ==";
		ok = ok && run_until(log_has, 60000000, "the menu");
		log_mark = uart_log.size();
		uint64_t c0 = sys_cycles;

		/* the CRCs, against the spec's own examples */
		bool crc_ok = uh_crc5(0) == 0x02 && uh_crc16((const uint8_t *)"", 0) == 0x0000;
		uint8_t t9[] = { '1','2','3','4','5','6','7','8','9' };
		crc_ok = crc_ok && uh_crc16(t9, 9) == 0xB4C8;
		fprintf(stderr, "%s the host's CRC5 and CRC16\n", crc_ok ? "ok  " : "FAIL");
		ok = ok && crc_ok;

		uh_active = true;
		ok = ok && run_until([]() { return top.p_usbd__pu__on.get<bool>(); }, 2000000, "the pull-up");
		uh_idle(1000);
		uh_reset = true;
		for (int i = 0; i < 2 * 48 * 20; i++)		/* 20 us of SE0 */
			step_one();
		uh_reset = false;
		uh_idle(2000);

		uint8_t d[256];
		memset(d, 0, sizeof(d));
		int n = uh_control(0, 0x80, 6, 0x0100, 0, 18, d);
		bool dev_ok = n == 18 && d[0] == 18 && d[1] == 1 && d[8] == 0xD0 && d[9] == 0x16 && d[10] == 0x6D && d[11] == 0x11;
		fprintf(stderr, "%s device descriptor at address 0: %d bytes, %02x%02x:%02x%02x\n", dev_ok ? "ok  " : "FAIL",
		        n, d[9], d[8], d[11], d[10]);
		ok = ok && dev_ok;
		const int A = 9;
		ok = ok && uh_control(0, 0x00, 5, A, 0, 0, 0) == 0;
		uh_idle(500);
		n = uh_control(A, 0x80, 6, 0x0200, 0, 255, d);
		bool cfg_ok = n == 116 && d[1] == 2 && d[4] == 4;
		fprintf(stderr, "%s configuration descriptor at address %d: %d bytes, %d interfaces\n", cfg_ok ? "ok  " : "FAIL",
		        A, n, d[4]);
		ok = ok && cfg_ok;
		ok = ok && uh_control(A, 0x00, 9, 1, 0, 0, 0) == 0;
		ok = ok && uh_control(A, 0x21, 0x22, 1, 0, 0, 0) == 0;		/* DTR: a terminal opens the port */

		/* DFU: dfu-util -a 0 -D small.bit */
		uint8_t st[6];
		int blocks = 0;
		bool dfu_ok = ok;
		for (size_t off = 0; dfu_ok; off += 4096) {
			int len = off < bit.size() ? (int)min((size_t)4096, bit.size() - off) : 0;
			dfu_ok = uh_control(A, 0x21, 1, (uint16_t)blocks, 2, (uint16_t)len, len ? &bit[off] : 0) == len;
			dfu_ok = dfu_ok && uh_control(A, 0xA1, 3, 0, 2, 6, st) == 6 && st[0] == 0;
			blocks++;
			if (!len)
				break;
			dfu_ok = dfu_ok && st[4] == 5;
		}
		dfu_ok = dfu_ok && st[4] == 2;
		fprintf(stderr, "%s dfu: %d blocks, final state %d, status %d\n", dfu_ok ? "ok  " : "FAIL", blocks, st[4], st[0]);
		ok = ok && dfu_ok;

		/* the console: what machboot printed, and keys typed on it */
		string cdc;
		int tog = 0;
		bool tog_ok = true;
		for (int i = 0; i < 40 && cdc.find("usb: dfu: done") == string::npos; i++) {
			vector<uint8_t> v;
			int pid = uh_in(A, 1, v, 3000);
			if (pid < 0)
				break;
			tog_ok = tog_ok && pid == (tog ? PID_DATA1 : PID_DATA0);
			tog ^= 1;
			cdc.append(v.begin(), v.end());
		}
		log_mark = uart_log.size();
		const uint8_t typed[] = { 0x1b, '[', 'D', 'b' };
		bool typed_ok = uh_out(PID_OUT, A, 1, PID_DATA0, typed, 4);
		want = "== Board ==";
		typed_ok = typed_ok && run_until(log_has, 40000000, "the Board page, from keys typed on the USB console");
		bool cdc_ok = cdc.find("usb: dfu: done") != string::npos && tog_ok;
		fprintf(stderr, "%s console: %d bytes from the device, data toggles %s\n", cdc_ok ? "ok  " : "FAIL",
		        (int)cdc.size(), tog_ok ? "right" : "WRONG");
		fprintf(stderr, "%s console: keys typed on it reach the menu\n", typed_ok ? "ok  " : "FAIL");
		ok = ok && cdc_ok && typed_ok;

		/* the card reader: Card reader page, Enter; then SCSI over bulk-only */
		/* the console's keys left the sidebar with the focus, on Board */
		log_mark = uart_log.size();
		keys("c");
		want = "== Card reader ==";
		ok = ok && run_until(log_has, 40000000, "the Card reader page");
		log_mark = uart_log.size();
		keys("\x1b[C\r");
		want = "the card is the PC's";
		ok = ok && run_until(log_has, 40000000, "sharing the card");
		int mtog_in = 0, mtog_out = 0, tag = 1;
		auto msc = [&](const uint8_t *cb, int cblen, bool in, uint32_t len, vector<uint8_t> &data) -> int {
			uint8_t cbw[31];
			memset(cbw, 0, sizeof(cbw));
			memcpy(cbw, "USBC", 4);
			cbw[4] = (uint8_t)tag++;
			cbw[8] = (uint8_t)len; cbw[9] = (uint8_t)(len >> 8); cbw[10] = (uint8_t)(len >> 16);
			cbw[12] = in ? 0x80 : 0;
			cbw[14] = (uint8_t)cblen;
			memcpy(cbw + 15, cb, (size_t)cblen);
			if (!uh_out(PID_OUT, A, 3, mtog_out ? PID_DATA1 : PID_DATA0, cbw, 31))
				return -1;
			mtog_out ^= 1;
			data.clear();
			while (in && data.size() < len) {
				vector<uint8_t> v;
				int pid = uh_in(A, 3, v);
				if (pid != (mtog_in ? PID_DATA1 : PID_DATA0))
					return -1;
				mtog_in ^= 1;
				data.insert(data.end(), v.begin(), v.end());
				if (v.size() < 64)
					break;
			}
			vector<uint8_t> csw;
			int pid = uh_in(A, 3, csw);
			if (pid != (mtog_in ? PID_DATA1 : PID_DATA0) || csw.size() != 13 || memcmp(csw.data(), "USBS", 4))
				return -1;
			mtog_in ^= 1;
			return csw[12];
		};
		vector<uint8_t> md;
		const uint8_t tur[6] = { 0x00 };
		int s1 = msc(tur, 6, false, 0, md);		/* the medium changed: UNIT ATTENTION */
		int s2 = msc(tur, 6, false, 0, md);
		const uint8_t rd[10] = { 0x28, 0, 0, 0, 0, 0, 0, 0, 2, 0 };	/* READ (10): LBA 0, 2 sectors */
		int s3 = msc(rd, 10, true, 1024, md);
		bool msc_ok = s1 == 1 && s2 == 0 && s3 == 0 && md.size() == 1024 && equal(md.begin(), md.end(), card0.begin());
		fprintf(stderr, "%s card reader: TEST UNIT READY %d then %d, READ (10) of 2 sectors %d, %s\n", msc_ok ? "ok  " : "FAIL",
		        s1, s2, s3, md.size() == 1024 && equal(md.begin(), md.end(), card0.begin()) ? "byte for byte" : "WRONG DATA");
		ok = ok && msc_ok;

		bool slot_ok = equal(bit.begin(), bit.end(), flash.begin() + 0x060000);
		bool boot_same = equal(flash.begin(), flash.begin() + 0x05E000, before.begin());
		bool region_same = equal(flash.begin() + 0x100000, flash.end(), before.begin() + 0x100000);
		fprintf(stderr, "%s the system slot holds the downloaded bitstream\n", slot_ok ? "ok  " : "FAIL");
		if (!slot_ok)
			for (size_t i = 0; i < bit.size(); i++)
				if (bit[i] != flash[0x060000 + i]) {
					fprintf(stderr, "     first difference at +0x%zx: %02x, want %02x\n", i, flash[0x060000 + i], bit[i]);
					break;
				}
		FILE *ff = fopen((out + "/usb-flash-after.bin").c_str(), "wb");
		fwrite(flash.data(), 1, flash.size(), ff);
		fclose(ff);
		fprintf(stderr, "%s the boot slot and the Zeitlos region are unchanged\n", boot_same && region_same ? "ok  " : "FAIL");
		ok = ok && slot_ok && boot_same && region_same;
		fprintf(stderr, "\n[usb: %llu cycles = %.1f ms after the menu opened, %llu NAKs]\n",
		        (unsigned long long)(sys_cycles - c0), (sys_cycles - c0) / 48e3, (unsigned long long)uh_naks);
		FILE *lf = fopen((out + "/sim_usb_console.txt").c_str(), "w");
		fputs(cdc.c_str(), lf);
		fclose(lf);
		fprintf(stderr, ok ? "SIM PASS (usb)\n" : "SIM FAIL (usb)\n");
		return ok ? 0 : 1;
	}

	if (argc > 6 && string(argv[3]) == "--selfinstall") {
		/*
		 * Install mode, on the installer gateware: the firmware is found
		 * at 0x090000 (tinydfu's user area), Enter and Y install it at
		 * 0x000000, then the system from the card, then PROGRAMN. The
		 * fence is the RTL's: the boot slot is writable here, the
		 * Zeitlos region is not.
		 */
		load(argv[4], card);
		sdm_init(card.data(), card.size(), 1);
		sd_have = true;
		vector<uint8_t> before = flash, bit, payload;
		load(argv[5], bit);
		load(argv[6], payload);

		auto keys = [](const char *s) { for (; *s; s++) u_in.push_back((uint8_t)*s); };
		want = "== Install machboot ==";
		ok = run_until(log_has, 80000000, "the install-mode page");
		log_mark = uart_log.find(want);
		want = "[";
		ok = ok && run_until(log_has, 60000000, "its key line");
		log_mark = uart_log.size();
		keys("\r");
		want = "Replace tinydfu";
		ok = ok && run_until(log_has, 60000000, "the question");
		log_mark = uart_log.size();
		want = "[";
		ok = ok && run_until(log_has, 60000000, "its key line");
		keys("y");
		ok = ok && run_until(reconfig, 3000000000ull, "PROGRAMN after installing");

		bool boot_ok = payload.size() <= 0x05E000 && equal(payload.begin(), payload.end(), flash.begin());
		for (size_t i = payload.size(); boot_ok && i < 0x05E000; i++)
			boot_ok = flash[i] == 0xff;
		bool slot_ok = equal(bit.begin(), bit.end(), flash.begin() + 0x060000);
		bool region_same = equal(flash.begin() + 0x100000, flash.end(), before.begin() + 0x100000);
		bool hidden = true;
		for (size_t i = 0x090000; i < 0x091000; i++)
			hidden = hidden && (flash[i] == 0xff || (i - 0x060000) < bit.size());
		string settings(flash.begin() + 0x05E000, flash.begin() + 0x060000);
		bool recorded = settings.find("sys.name") != string::npos && settings.find("Zeitlos") != string::npos;
		fprintf(stderr, "\n[install mode: %u sector erases, %u page programs, %llu cycles = %.1f s]\n",
		        f_erases, f_programs, (unsigned long long)sys_cycles, sys_cycles / 48e6);
		fprintf(stderr, "%s 0x000000-0x05DFFF holds the boot-slot image\n", boot_ok ? "ok  " : "FAIL");
		fprintf(stderr, "%s the system slot holds the card's bitstream\n", slot_ok ? "ok  " : "FAIL");
		fprintf(stderr, "%s the Zeitlos region is unchanged\n", region_same ? "ok  " : "FAIL");
		fprintf(stderr, "%s the installer's firmware header is gone\n", hidden ? "ok  " : "FAIL");
		fprintf(stderr, "%s the new settings record the install\n", recorded ? "ok  " : "FAIL");
		ok = ok && boot_ok && slot_ok && region_same && hidden && recorded;
		FILE *lf = fopen((out + "/sim_selfinstall_serial.txt").c_str(), "w");
		fputs(uart_log.c_str(), lf);
		fclose(lf);
		/* and the result must boot as a normal machboot (the PC build checks the pages) */
		FILE *of = fopen((out + "/inst-flash-after.bin").c_str(), "wb");
		fwrite(flash.data(), 1, flash.size(), of);
		fclose(of);
		fprintf(stderr, ok ? "SIM PASS (install mode)\n" : "SIM FAIL (install mode)\n");
		return ok ? 0 : 1;
	}

	if (argc > 4 && string(argv[3]) == "--install") {
		/*
		 * Install from a card image: F8, F2 (Install), Enter. Then the
		 * flash must hold the card's bitstream in the system slot, the
		 * settings must record it, and nothing else may have changed --
		 * which checks the SD controller, the FAT reader, the flash
		 * controller's program and erase paths, its fence and its
		 * settings window, all in the RTL.
		 */
		FILE *c = fopen(argv[4], "rb");
		if (!c) { perror(argv[4]); return 1; }
		fseek(c, 0, SEEK_END);
		card.resize(ftell(c));
		fseek(c, 0, SEEK_SET);
		if (fread(card.data(), 1, card.size(), c) != card.size()) return 1;
		fclose(c);
		sdm_init(card.data(), card.size(), 1);
		sd_have = true;
		vector<uint8_t> before = flash;

		auto keys = [](const char *s) { for (; *s; s++) u_in.push_back((uint8_t)*s); };
		want = "Starting";
		ok = run_until(log_has, 60000000, "the startup screen");
		keys("\x1b[19~");
		want = "== Start a system ==";
		ok = ok && run_until(log_has, 60000000, "the menu");
		log_mark = uart_log.size();
		keys("\x1bOQ");
		want = "== Install a system ==";
		ok = ok && run_until(log_has, 60000000, "the Install page");
		log_mark = uart_log.size();
		want = "[";
		ok = ok && run_until(log_has, 60000000, "the Install page's key line");
		log_mark = uart_log.size();
		keys("\r");
		want = "is installed.";
		ok = ok && run_until(log_has, 400000000, "the install");
		for (int i = 0; i < 100000; i++)
			step_one();

		/* what the flash holds now */
		const char *bitpath = argv[5];
		FILE *b = fopen(bitpath, "rb");
		vector<uint8_t> bit;
		if (b) {
			fseek(b, 0, SEEK_END);
			bit.resize(ftell(b));
			fseek(b, 0, SEEK_SET);
			if (fread(bit.data(), 1, bit.size(), b) != bit.size()) bit.clear();
			fclose(b);
		}
		bool slot_ok = !bit.empty() && equal(bit.begin(), bit.end(), flash.begin() + 0x060000);
		bool boot_same = equal(flash.begin(), flash.begin() + 0x05E000, before.begin());
		bool region_same = equal(flash.begin() + 0x100000, flash.end(), before.begin() + 0x100000);
		string settings(flash.begin() + 0x05E000, flash.begin() + 0x060000);
		bool recorded = settings.find("sys.name") != string::npos && settings.find("Zeitlos") != string::npos;
		fprintf(stderr, "\n[install: %u sector erases, %u page programs, %u card reads]\n",
		        f_erases, f_programs, sdm_reads());
		fprintf(stderr, "%s the system slot holds the card's bitstream\n", slot_ok ? "ok  " : "FAIL");
		fprintf(stderr, "%s the boot slot below the settings is unchanged\n", boot_same ? "ok  " : "FAIL");
		fprintf(stderr, "%s the Zeitlos region is unchanged\n", region_same ? "ok  " : "FAIL");
		fprintf(stderr, "%s the settings record the install\n", recorded ? "ok  " : "FAIL");
		ok = ok && slot_ok && boot_same && region_same && recorded;
		f = fopen((out + "/sim_install_serial.txt").c_str(), "w");
		fputs(uart_log.c_str(), f);
		fclose(f);
		fprintf(stderr, ok ? "SIM PASS (install)\n" : "SIM FAIL (install)\n");
		return ok ? 0 : 1;
	}

#ifdef MB_SIM_XLINK
	if (argc > 4 && string(argv[3]) == "--xlink") {
		/*
		 * Sergei ML2: the card reader through the RP2040. Its own code
		 * (rp2040/src/msc.c, xlink.c) talks to the SoC over XA-XD; the
		 * menu is driven over the serial port, as a person would.
		 */
		load(argv[4], card);
		vector<uint8_t> card0 = card;
		sdm_init(card.data(), card.size(), 1);
		sd_have = true;
		uint8_t k, a, q;
		static uint8_t buf[8 * 512];
		/*
		 * The module flash holds a system (a 45F bitstream at 0), so
		 * machboot would count down to start it -- but the settings the
		 * RP2040 keeps say boot.auto=0. As after the RP2040's own load,
		 * it probes from the start; machboot asks for the settings at
		 * boot, and the countdown follows them.
		 */
		memset(xl_cache, 0xFF, sizeof(xl_cache));
		cache_init(&xl_cache_io);
		msc_hooks(&xl_hooks);
		msc_set_pc(1);
		ok = !msc_ready(&k, &a, &q) && a == 0x3A;
		fprintf(stderr, "\n[xlink: before machboot: %s]\n", ok ? "no medium, no probing" : "WRONG");
		msc_link_event();		/* the menu was just loaded */
		want = "== Start a system ==";
		ok = ok && rp_run_until(log_has, 40000000, "the menu");
		bool got = uart_log.find("xlink: settings from the RP2040") != string::npos;
		bool no_count = uart_log.find("Starting Zeitlos in") == string::npos &&
		                uart_log.find("Starting System in") == string::npos;
		fprintf(stderr, "[xlink: settings handed over at boot: %s; boot.auto=0 obeyed: %s]\n",
		        got ? "yes" : "NO", no_count ? "the menu, no countdown" : "NO, it counted down");
		ok = ok && got && no_count && msc_link_state() == 2;
		/* Card reader, Enter: shared */
		for (const char *p = "\x1b[Dc\x1b[C\r"; *p; p++)
			u_in.push_back((uint8_t)*p);
		log_mark = uart_log.size();
		want = "The SD card is on the PC as a USB drive.";
		ok = ok && run_until(log_has, 40000000, "the card to be shared");
		bool ua = !msc_ready(&k, &a, &q) && k == 0x06 && a == 0x28;
		bool ready = msc_ready(&k, &a, &q);
		bool cap = msc_sectors() == card.size() / 512;
		fprintf(stderr, "[xlink: shared: UNIT ATTENTION %s, then ready %s, %u sectors %s]\n",
		        ua ? "yes" : "NO", ready ? "yes" : "NO", msc_sectors(), cap ? "(the card's)" : "WRONG");
		ok = ok && ua && ready && cap;
		/* READ: 5 sectors (3 + 2 on the link), timed */
		uint64_t c0 = sys_cycles;
		bool rd = ok && msc_read(0, buf, 5) == 0 && !memcmp(buf, card.data(), 5 * 512);
		double ms = (sys_cycles - c0) / 48000.0;
		fprintf(stderr, "[xlink: READ 5 sectors: %s, %.2f ms = %.0f KB/s]\n", rd ? "the card's bytes" : "WRONG",
		        ms, 2.5 / (ms / 1000.0));
		/* the PC's way: 3-sector callbacks in sequence; after each, USB
		   sends it (1.3 ms for 1.5 KB at full speed) while the RP2040
		   fetches ahead -- both at once, as on the chip */
		{
			static uint8_t big[3 * 512];
			bool same = ok;
			uint64_t s0 = sys_cycles;
			xl_t_xfer = xl_t_idle = xl_t_sel = xl_n_frames = xl_n_idle = 0;
			sd_low_cycles = 0;
			uint64_t t_bg = 0;
			const uint32_t N = 120;			/* 60 KB */
			for (uint32_t l = 100; l < 100 + N && same; l += 3) {
				same = msc_read(l, big, 3) == 0 && !memcmp(big, card.data() + (size_t)l * 512, 3 * 512);
				uint64_t usb_done = sys_cycles + 1300 * 48;	/* 1.3 ms */
				uint64_t b0 = now_ps;
				msc_background();
				t_bg += now_ps - b0;
				while (sys_cycles < usb_done)
					step_one();
			}
			double t_ms = (sys_cycles - s0) / 48000.0;
			fprintf(stderr, "[xlink: %u sectors in sequence, USB's time included: %s, %.1f ms = %.0f KB/s]\n",
			        N, same ? "the card's bytes" : "WRONG", t_ms, N / 2.0 / (t_ms / 1000.0));
			ok = ok && same;
			fprintf(stderr, "[xlink: of that, the fetch-ahead %.1f ms: shifting %.1f ms, polling %.1f ms (%llu waits), CS %.1f ms (%llu frames)]\n",
			        t_bg / 1e9, xl_t_xfer / 1e9, xl_t_idle / 1e9, (unsigned long long)xl_n_idle, xl_t_sel / 1e9,
			        (unsigned long long)xl_n_frames);
			fprintf(stderr, "[xlink: the SD card selected %.1f ms of it: %.2f ms per sector]\n",
			        sd_low_cycles / 48000.0, sd_low_cycles / 48000.0 / N);
		}
		uint32_t far = (uint32_t)(card.size() / 512) - 3;
		rd = rd && msc_read(far, buf, 3) == 0 && !memcmp(buf, card.data() + (size_t)far * 512, 3 * 512);
		fprintf(stderr, "[xlink: READ the last 3 sectors: %s]\n", rd ? "ok" : "WRONG");
		ok = ok && rd;
		/* WRITE 4 sectors (3 + 1) */
		for (int i = 0; i < 4 * 512; i++)
			buf[i] = (uint8_t)(i * 11 + 5);
		uint32_t wl = far - 10;
		c0 = sys_cycles;
		bool wr = ok && msc_write(wl, buf, 4) == 0 && !memcmp(card.data() + (size_t)wl * 512, buf, 4 * 512);
		ms = (sys_cycles - c0) / 48000.0;
		/* and nothing else changed */
		for (size_t i = 0; wr && i < card.size(); i++)
			if ((i < (size_t)wl * 512 || i >= (size_t)(wl + 4) * 512) && card[i] != card0[i])
				wr = false;
		fprintf(stderr, "[xlink: WRITE 4 sectors: %s, %.2f ms]\n", wr ? "on the card, nothing else changed" : "WRONG", ms);
		ok = ok && wr;
		/* the PC ejects */
		log_mark = uart_log.size();
		msc_eject();
		want = "ejected on the PC";
		ok = ok && rp_run_until(log_has, 40000000, "the menu to take the card back");
		ok = ok && !msc_ready(&k, &a, &q) && a == 0x3A;

		/* a setting changed in the menu: the RP2040 fetches the new text
		   (machboot saved once already at start: the module's system went
		   into its list of systems, docs/systems.md) */
		xl_put = false;
		for (const char *p = "\x1b[Dt\x1b[C\x1b[C"; *p; p++)	/* Settings; Start by itself: on */
			u_in.push_back((uint8_t)*p);
		rp_run_ms(100);
		u_in.push_back(0x1b);					/* Esc: leave, save */
		ok = ok && rp_run_until(xl_got_settings, 40000000, "the RP2040 to fetch the settings");
		{
			string shown = xl_kept;
			for (auto &ch : shown) if (ch == '\n') ch = '|';
			fprintf(stderr, "[xlink: changed in the menu, the RP2040 keeps: %s]\n", shown.c_str());
		}
		ok = ok && xl_kept.find("boot.auto=1") != string::npos && xl_kept.find("boot.wait=9") != string::npos;

		/* Start: the RP2040 hears of it before PROGRAMN is pulled */
		for (const char *p = "\x1b[Ds\x1b[C\r"; *p; p++)
			u_in.push_back((uint8_t)*p);
		ok = ok && rp_run_until(reconfig, 40000000, "PROGRAMN");
		fprintf(stderr, "[xlink: Start: START %s before PROGRAMN]\n", xl_started ? "came" : "did NOT come");
		ok = ok && xl_started;
		f = fopen((out + "/sim_xlink_serial.txt").c_str(), "w");
		fputs(uart_log.c_str(), f);
		fclose(f);
		fprintf(stderr, ok ? "SIM PASS (xlink: Sergei ML2 card reader)\n" : "SIM FAIL (xlink)\n");
		return ok ? 0 : 1;
	}
#endif

	if (argc > 3 && string(argv[3]) == "--sram") {
		/* the JTAG test build on a blank flash: straight to the menu */
		want = "== Start a system ==";
		ok = run_until(log_has, 40000000, "the menu");
		want = "[";
		log_mark = uart_log.find("== Start a system ==");
		ok = ok && run_until(log_has, 40000000, "the key line");
		for (int i = 0; i < 200000; i++)
			step_one();
		ok = ok && capture(out + "/sim_sram_menu.ppm");
		f = fopen((out + "/sim_sram_serial.txt").c_str(), "w");
		fputs(uart_log.c_str(), f);
		fclose(f);
		fprintf(stderr, ok ? "SIM PASS (sram)\n" : "SIM FAIL (sram)\n");
		return ok ? 0 : 1;
	}

	want = "Starting";
	ok = ok && run_until(log_has, 40000000, "the startup screen");
	if (ok) {
		fprintf(stderr, "\n[boot took %llu cycles = %.1f ms, %u flash reads]\n",
		        (unsigned long long)sys_cycles, sys_cycles / 48000.0, f_reads);
		/* let the first startup screen finish drawing */
		for (int i = 0; i < 2000000 && !reconfig(); i++)
			step_one();
		ok = capture(out + "/sim_startup.ppm");
	}

	if (ok) {
		for (const char *p = "\x1b[19~"; *p; p++)
			u_in.push_back((uint8_t)*p);
		log_mark = uart_log.size();
		want = "[";		/* the key line ends the mirrored page */
		ok = run_until(log_has, 40000000, "the menu");
		for (int i = 0; i < 200000; i++)
			step_one();
		ok = ok && capture(out + "/sim_menu.ppm");
	}

	/* every page once: on Lakritz each is an overlay, loaded from flash as
	   it is shown (docs/overlays.md) -- then back to Start */
	{
		static const struct { const char *keys, *title; } walk[] = {
			{ "\x1b[Db\x1b[C", "== Board ==" },
			{ "\x1b[Dd\x1b[C", "== Diagnostics ==" },
			{ "\x1b[Dc\x1b[C", "== Card reader ==" },
			{ "\x1b[Dt\x1b[C", "== Settings ==" },
			{ "\x1bOQ", "== Install a system ==" },	/* F2, from anywhere */
			{ "\x1b[Ds\x1b[C", "== Start a system ==" },
		};
		for (size_t w = 0; ok && w < sizeof(walk) / sizeof(walk[0]); w++) {
			log_mark = uart_log.size();
			for (const char *p = walk[w].keys; *p; p++)
				u_in.push_back((uint8_t)*p);
			want = walk[w].title;
			ok = run_until(log_has, 40000000, walk[w].title);
			want = "[";
			ok = ok && run_until(log_has, 40000000, "its key line");
		}
		bool missing = uart_log.find("could not be loaded") != string::npos;
		fprintf(stderr, "\n[every page shown, each loaded from flash: %s]\n", ok && !missing ? "yes" : "NO");
		ok = ok && !missing;
	}

	if (ok) {
		u_in.push_back('\r');
		ok = run_until(reconfig, 20000000, "PROGRAMN");
		if (ok)
			fprintf(stderr, "\n[PROGRAMN pulled after %llu cycles]\n", (unsigned long long)sys_cycles);
	}

	f = fopen((out + "/sim_serial.txt").c_str(), "w");
	fputs(uart_log.c_str(), f);
	fclose(f);
	fprintf(stderr, ok ? "SIM PASS\n" : "SIM FAIL\n");
	return ok ? 0 : 1;
}
