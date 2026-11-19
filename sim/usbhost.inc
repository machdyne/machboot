/*
 * machboot
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * A USB full-speed host at the wire, for sim_main.cc: it drives D+/D-
 * of the device under test (rtl/mb_usbd.v and ulixxe's SIE) a bit at a
 * time and decodes what the device sends back. Written from the USB 2.0
 * specification, chapter 7 (NRZI, bit stuffing, SYNC, EOP) and chapter 8
 * (packets, CRC5, CRC16, transactions) -- not from the device's RTL, so
 * the two check each other.
 *
 * The system clock is 48 MHz, so a bit is 4 cycles.
 */

enum { L_SE0 = 0, L_J = 1, L_K = 2 };

static deque<uint8_t> uh_tx;		/* line states, one per bit */
static int uh_tx_sub;
static bool uh_cap_on, uh_rx_ready;
static int uh_cap_sub;
static vector<uint8_t> uh_cap, uh_rx;
static bool uh_active;			/* the host is attached */
static bool uh_reset;			/* driving SE0 */

static void uh_tick()
{
	if (!uh_active)
		return;
	bool dtx = top.p_usbd__tx__en.get<bool>();
	if (dtx) {
		if (!uh_cap_on) {
			uh_cap_on = true;
			uh_cap.clear();
			uh_cap_sub = 0;
		}
		if (uh_cap_sub % 4 == 2) {
			bool dp = top.p_usbd__dp__tx.get<bool>(), dn = top.p_usbd__dn__tx.get<bool>();
			uh_cap.push_back(dp && !dn ? L_J : !dp && dn ? L_K : L_SE0);
		}
		uh_cap_sub++;
	} else if (uh_cap_on) {
		uh_cap_on = false;
		uh_rx = uh_cap;
		uh_rx_ready = true;
	}

	int line;
	if (uh_reset) {
		line = L_SE0;
	} else if (!uh_tx.empty()) {
		line = uh_tx.front();
		if (++uh_tx_sub == 4) {
			uh_tx_sub = 0;
			uh_tx.pop_front();
		}
	} else {
		/* idle: the device's pull-up makes it J; without it, the host's
		 * pull-downs make it SE0 */
		line = top.p_usbd__pu__on.get<bool>() ? L_J : L_SE0;
	}
	top.p_usbd__dp__rx.set<bool>(line == L_J);
	top.p_usbd__dn__rx.set<bool>(line == L_K);
}

/* CRC5 and CRC16 as USB 2.0 8.3.5 states them: reflected, sent LSb first */
static uint8_t uh_crc5(uint32_t v11)
{
	uint8_t c = 0x1f;
	for (int i = 0; i < 11; i++) {
		int b = (v11 >> i) & 1;
		c = ((c ^ b) & 1) ? (uint8_t)((c >> 1) ^ 0x14) : (uint8_t)(c >> 1);
	}
	return c ^ 0x1f;
}

static uint16_t uh_crc16(const uint8_t *p, int n)
{
	uint16_t c = 0xffff;
	for (int i = 0; i < n; i++) {
		c ^= p[i];
		for (int k = 0; k < 8; k++)
			c = (c & 1) ? (uint16_t)((c >> 1) ^ 0xA001) : (uint16_t)(c >> 1);
	}
	return c ^ 0xffff;
}

/* NRZI with bit stuffing, SYNC first, EOP last */
static void uh_send_bytes(const vector<uint8_t> &bytes)
{
	int state = L_J, ones = 0;
	vector<uint8_t> all;
	all.push_back(0x80);			/* SYNC: KJKJKJKK */
	all.insert(all.end(), bytes.begin(), bytes.end());
	for (uint8_t byte : all) {
		for (int i = 0; i < 8; i++) {
			if ((byte >> i) & 1) {
				ones++;
			} else {
				state = state == L_J ? L_K : L_J;
				ones = 0;
			}
			uh_tx.push_back((uint8_t)state);
			if (ones == 6) {
				state = state == L_J ? L_K : L_J;
				uh_tx.push_back((uint8_t)state);
				ones = 0;
			}
		}
	}
	uh_tx.push_back(L_SE0);
	uh_tx.push_back(L_SE0);
	uh_tx.push_back(L_J);
	while (!uh_tx.empty())
		step_one();
	/* a few bit times of idle between packets */
	for (int i = 0; i < 16; i++)
		step_one();
}

enum { PID_OUT = 0x1, PID_IN = 0x9, PID_SETUP = 0xD, PID_DATA0 = 0x3, PID_DATA1 = 0xB,
       PID_ACK = 0x2, PID_NAK = 0xA, PID_STALL = 0xE };

static uint8_t uh_pid(int pid) { return (uint8_t)(pid | ((~pid & 0xf) << 4)); }

static void uh_token(int pid, int addr, int ep)
{
	uint32_t v = (uint32_t)(addr & 0x7f) | (uint32_t)(ep & 0xf) << 7;
	uint32_t f = v | (uint32_t)uh_crc5(v) << 11;
	uh_send_bytes({ uh_pid(pid), (uint8_t)f, (uint8_t)(f >> 8) });
}

static void uh_data(int pid, const uint8_t *p, int n)
{
	vector<uint8_t> b;
	b.push_back(uh_pid(pid));
	b.insert(b.end(), p, p + n);
	uint16_t c = uh_crc16(p, n);
	b.push_back((uint8_t)c);
	b.push_back((uint8_t)(c >> 8));
	uh_send_bytes(b);
}

/* the device's next packet: its PID (-1 timeout, -2 garbled), data in out */
static int uh_recv(vector<uint8_t> &out, int timeout = 2000)
{
	uh_rx_ready = false;
	for (int i = 0; i < timeout * 2 && !uh_rx_ready && !uh_cap_on; i++)
		step_one();
	while (uh_cap_on)
		step_one();
	if (!uh_rx_ready)
		return -1;
	uh_rx_ready = false;
	/* decode: NRZI, unstuff, until SE0 */
	vector<int> bits;
	int prev = L_J, ones = 0;
	for (uint8_t s : uh_rx) {
		if (s == L_SE0)
			break;
		int bit = (s == prev) ? 1 : 0;
		prev = s;
		if (ones == 6) {
			ones = 0;
			if (bit != 0)
				return -2;		/* a stuffing error */
			continue;
		}
		bits.push_back(bit);
		ones = bit ? ones + 1 : 0;
	}
	if (bits.size() < 16)
		return -2;
	int sync = 0;
	for (int i = 0; i < 8; i++)
		sync |= bits[i] << i;
	if (sync != 0x80)
		return -2;
	vector<uint8_t> bytes;
	for (size_t i = 8; i + 8 <= bits.size(); i += 8) {
		int v = 0;
		for (int k = 0; k < 8; k++)
			v |= bits[i + k] << k;
		bytes.push_back((uint8_t)v);
	}
	int pid = bytes[0] & 0xf;
	if (((bytes[0] >> 4) ^ 0xf) != pid)
		return -2;
	out.clear();
	if (pid == PID_DATA0 || pid == PID_DATA1) {
		if (bytes.size() < 3)
			return -2;
		int n = (int)bytes.size() - 3;
		uint16_t c = (uint16_t)(bytes[1 + n] | bytes[2 + n] << 8);
		if (uh_crc16(&bytes[1], n) != c)
			return -2;
		out.assign(bytes.begin() + 1, bytes.begin() + 1 + n);
	}
	return pid;
}

static uint64_t uh_naks;

static void uh_idle(int bits)
{
	for (int i = 0; i < bits * 4 * 2; i++)
		step_one();
}

/* one IN transaction, retried on NAK; the data's PID, or -1 */
static int uh_in(int addr, int ep, vector<uint8_t> &out, int tries = 20000)
{
	for (int t = 0; t < tries; t++) {
		uh_token(PID_IN, addr, ep);
		int pid = uh_recv(out);
		if (pid == PID_NAK) {
			uh_naks++;
			uh_idle(200);
			continue;
		}
		if (pid == PID_DATA0 || pid == PID_DATA1) {
			uh_send_bytes({ uh_pid(PID_ACK) });
			return pid;
		}
		fprintf(stderr, "usb host: IN %d.%d: pid %d\n", addr, ep, pid);
		return -1;
	}
	fprintf(stderr, "usb host: IN %d.%d: NAKed %d times\n", addr, ep, tries);
	return -1;
}

/* one OUT (or SETUP) transaction, retried on NAK; true if ACKed */
static bool uh_out(int tok, int addr, int ep, int dpid, const uint8_t *p, int n, int tries = 20000)
{
	for (int t = 0; t < tries; t++) {
		uh_token(tok, addr, ep);
		uh_data(dpid, p, n);
		vector<uint8_t> dummy;
		int pid = uh_recv(dummy);
		if (pid == PID_ACK)
			return true;
		if (pid == PID_NAK) {
			uh_naks++;
			uh_idle(200);
			continue;
		}
		fprintf(stderr, "usb host: %s %d.%d: pid %d\n", tok == PID_SETUP ? "SETUP" : "OUT", addr, ep, pid);
		return false;
	}
	fprintf(stderr, "usb host: OUT %d.%d: NAKed %d times\n", addr, ep, tries);
	return false;
}

/* a control transfer; returns the bytes of an IN data stage, or -1 */
static int uh_control(int addr, uint8_t bm, uint8_t req, uint16_t val, uint16_t idx, uint16_t len,
                      uint8_t *data)
{
	uint8_t s[8] = { bm, req, (uint8_t)val, (uint8_t)(val >> 8), (uint8_t)idx, (uint8_t)(idx >> 8),
	                 (uint8_t)len, (uint8_t)(len >> 8) };
	if (!uh_out(PID_SETUP, addr, 0, PID_DATA0, s, 8))
		return -1;
	int got = 0, tog = 1;
	if (bm & 0x80) {
		while (got < len) {
			vector<uint8_t> d;
			int pid = uh_in(addr, 0, d);
			if (pid < 0)
				return -1;
			if (pid != (tog ? PID_DATA1 : PID_DATA0)) {
				fprintf(stderr, "usb host: control IN: wrong data toggle\n");
				return -1;
			}
			tog ^= 1;
			memcpy(data + got, d.data(), d.size() < (size_t)(len - got) ? d.size() : (size_t)(len - got));
			got += (int)d.size();
			if (d.size() < 64)
				break;
		}
		if (!uh_out(PID_OUT, addr, 0, PID_DATA1, 0, 0))		/* status */
			return -1;
	} else {
		while (got < len) {
			int n = len - got < 64 ? len - got : 64;
			if (!uh_out(PID_OUT, addr, 0, tog ? PID_DATA1 : PID_DATA0, data + got, n))
				return -1;
			tog ^= 1;
			got += n;
		}
		vector<uint8_t> d;
		int pid = uh_in(addr, 0, d, 400000);			/* status: the device may be writing flash */
		if (pid != PID_DATA1 || !d.empty()) {
			fprintf(stderr, "usb host: control OUT status: pid %d, %d bytes\n", pid, (int)d.size());
			return -1;
		}
	}
	return got;
}
