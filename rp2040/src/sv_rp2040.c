/*
 * machboot supervisor
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * sv.c, store.c and msc.c's hooks on the RP2040 (docs/power-on.md).
 *
 * The RP2040's flash (a W25Q32, 4 MB):
 *
 *   0x000000  this firmware                 (UF2: machboot-sv.uf2)
 *   0x080000  the menu: a 256-byte header, then the bitstream, 512 KB
 *             (UF2: machboot-<carrier>_ml<n>.uf2 carries it; machboot-sv.uf2
 *              erases its header, so a supervisor alone loads nothing)
 *   0x100000  machboot's settings and its list of systems, 64 KB (store.c)
 *   0x110000  the system cache, to the end (cache.c, docs/systems.md)
 */

#include <string.h>
#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/gpio.h"
#include "hardware/structs/iobank0.h"
#include "hardware/structs/vreg_and_chip_reset.h"
#include "hardware/structs/watchdog.h"
#include "tusb.h"
#include "board.h"
#include "sv.h"
#include "store.h"
#include "msc.h"
#include "cache.h"
#include "sv_rp2040.h"

#define MENU_OFF	0x080000u
#define MENU_MAX	0x080000u
#define STORE_OFF	0x100000u
#define CACHE_OFF	0x110000u	/* to the end of the chip (flash_bytes()) */

uint32_t sv_jtag_activity_us;
int sv_jtag_seen;

/* -- the log: "[sv] ..." lines, kept from power-on, shown on the CDC port
   (main.c replays them whenever a terminal opens it) ------------------ */

#define LOG_SIZE 4096
static char logbuf[LOG_SIZE];
static uint32_t log_len, log_sent;

void sv_log(const char *s)
{
	char line[160];
	int n = snprintf(line, sizeof(line), "[sv %lu.%03lu] %s\r\n",
	                 (unsigned long)(to_ms_since_boot(get_absolute_time()) / 1000),
	                 (unsigned long)(to_ms_since_boot(get_absolute_time()) % 1000), s);
	if (n < 0)
		return;
	if ((uint32_t)n > sizeof(line) - 1)
		n = sizeof(line) - 1;
	if (log_len + (uint32_t)n > LOG_SIZE) {
		/* full: forget the older half */
		uint32_t drop = LOG_SIZE / 2;
		memmove(logbuf, logbuf + drop, log_len - drop);
		log_len -= drop;
		log_sent = log_sent > drop ? log_sent - drop : 0;
	}
	memcpy(logbuf + log_len, line, (size_t)n);
	log_len += (uint32_t)n;
}

const char *sv_log_pending(uint32_t *n)
{
	*n = log_len - log_sent;
	return logbuf + log_sent;
}

void sv_log_taken(uint32_t n)
{
	log_sent += n;
}

void sv_log_replay(void)
{
	log_sent = 0;
}

/* -- the settings' flash ------------------------------------------------- */

static void st_erase(uint32_t off)
{
	uint32_t s = save_and_disable_interrupts();
	flash_range_erase(STORE_OFF + off, STORE_SECTOR);
	restore_interrupts(s);
}

static void st_program(uint32_t off, const uint8_t *page)
{
	uint32_t s = save_and_disable_interrupts();
	flash_range_program(STORE_OFF + off, page, STORE_PAGE);
	restore_interrupts(s);
}

static const store_io_t store_io = {
	(const uint8_t *)(XIP_BASE + STORE_OFF), st_erase, st_program
};

/* -- msc.c's hooks: settings and START ------------------------------------- */

static int settings_get(char *buf, int cap) { return store_get(buf, cap); }
static void settings_put(const char *text, int len) { store_put(text, len); }

/* -- the system cache's flash -------------------------------------------- */

static void ca_erase(uint32_t off, uint32_t len)
{
	uint32_t s = save_and_disable_interrupts();
	flash_range_erase(CACHE_OFF + off, len);
	restore_interrupts(s);
}

static void ca_program(uint32_t off, const uint8_t *page)
{
	uint32_t s = save_and_disable_interrupts();
	flash_range_program(CACHE_OFF + off, page, 256);
	restore_interrupts(s);
}

static cache_io_t cache_io = {
	(const uint8_t *)(XIP_BASE + CACHE_OFF), 0, ca_erase, ca_program
};

/* the chip's size, from its JEDEC ID (9Fh: maker, type, capacity as a
   power of two). Taking 4 MB on trust while the pico-sdk was told 2 MB
   once let its flash_range_erase() halt the RP2040 at the 2 MB mark.
   Anything odd: 2 MB, which every RP2040 board has. */
static uint32_t jedec;
static uint32_t flash_bytes(void)
{
	uint8_t tx[4] = { 0x9F, 0, 0, 0 }, rx[4] = { 0, 0, 0, 0 };
	uint32_t s = save_and_disable_interrupts();
	flash_do_cmd(tx, rx, 4);
	restore_interrupts(s);
	jedec = (uint32_t)rx[1] << 16 | (uint32_t)rx[2] << 8 | rx[3];
	if (rx[3] >= 0x15 && rx[3] <= 0x18)	/* 2 MB .. 16 MB */
		return 1u << rx[3];
	return 2u * 1024 * 1024;
}

static void run(uint32_t crc)
{
	const cache_item_t *it = cache_find(crc);
	if (!it)
		return;
	/* checked again before every start, not only when it was copied */
	uint32_t t0 = to_ms_since_boot(get_absolute_time());
	int ok = cache_check(it);
	char b[100];
	snprintf(b, sizeof(b), "cache: %.23s at 0x%06lx, %lu bytes: CRC %s (%lu ms)", it->name,
	         (unsigned long)(CACHE_OFF + it->off), (unsigned long)it->len, ok ? "right" : "WRONG",
	         (unsigned long)(to_ms_since_boot(get_absolute_time()) - t0));
	sv_log(b);
	if (!ok) {
		sv_refuse(it->name, "its copy in flash is damaged: remove it and install it again");
		return;
	}
	sv_run(cache_bitstream(it), it->len, it->idcode, it->name);
}

static void usb_poll(void) { tud_task(); }

static const msc_hooks_t hooks = { settings_get, settings_put, sv_start, run, sv_note, usb_poll, sv_log };

/* -- sv.c's ---------------------------------------------------------------- */

static uint32_t ms(void) { return to_ms_since_boot(get_absolute_time()); }

static int pc_active(void)
{
	return sv_jtag_seen && time_us_32() - sv_jtag_activity_us < 1500000u;
}

static int pc_wants(void)
{
	return tud_vendor_available() > 0;
}

/* SYS_RST_N's falling edge, latched by the IO bank whether or not its
   interrupt is enabled: even a short pulse is caught */
static int reset_edge(void)
{
	uint32_t r = iobank0_hw->intr[PIN_SYS_RST_N / 8] >> (4 * (PIN_SYS_RST_N % 8));
	if (!(r & GPIO_IRQ_EDGE_FALL))
		return 0;
	gpio_acknowledge_irq(PIN_SYS_RST_N, GPIO_IRQ_EDGE_FALL);
	return 1;
}

static int reset_low(void) { return !gpio_get(PIN_SYS_RST_N); }

static void poll(void) { tud_task(); }

static const sv_io_t sv_io = {
	(const uint8_t *)(XIP_BASE + MENU_OFF), MENU_MAX,
	ms, pc_active, pc_wants, reset_edge, reset_low, poll, sv_log
};

void sv_board_init(void)
{
	/* power on, or the RP2040's own reset pin -- not the watchdog the
	   boot ROM uses to start a new UF2, nor a reboot to BOOTSEL */
	uint32_t cr = vreg_and_chip_reset_hw->chip_reset;
	int power_on = (cr & (VREG_AND_CHIP_RESET_CHIP_RESET_HAD_POR_BITS |
	                      VREG_AND_CHIP_RESET_CHIP_RESET_HAD_RUN_BITS)) &&
	               !watchdog_hw->reason;

	gpio_acknowledge_irq(PIN_SYS_RST_N, GPIO_IRQ_EDGE_FALL);	/* from before */
	char b[120];
	sv_log("machboot supervisor " SV_VERSION);
	snprintf(b, sizeof(b), "reset reasons: chip_reset %08lx, watchdog %08lx: %s",
	         (unsigned long)cr, (unsigned long)watchdog_hw->reason,
	         power_on ? "power-on (or the RP2040's reset): the menu now" : "not a power-on (a UF2, or BOOTSEL)");
	sv_log(b);
	store_init(&store_io);
	{
		char t[64];
		int n = store_get(t, sizeof(t) - 1);
		snprintf(b, sizeof(b), "settings: %s", n ? "kept from before" : "none kept yet (machboot's defaults)");
		sv_log(b);
	}
	{
		uint32_t fb = flash_bytes();
		cache_io.size = fb > CACHE_OFF ? fb - CACHE_OFF : 0;
		snprintf(b, sizeof(b), "flash: JEDEC %06lx, %lu KB; the cache from 0x%06x, %lu KB",
		         (unsigned long)jedec, (unsigned long)(fb / 1024), (unsigned)CACHE_OFF,
		         (unsigned long)(cache_io.size / 1024));
		sv_log(b);
	}
	cache_init(&cache_io);
	{
		static cache_item_t it[32];
		uint32_t largest, fr = cache_free(&largest);
		int n = cache_list(it, 32);
		snprintf(b, sizeof(b), "cache: %d system%s, %lu KB free (largest gap %lu KB)", n, n == 1 ? "" : "s",
		         (unsigned long)(fr / 1024), (unsigned long)(largest / 1024));
		sv_log(b);
		for (int i = 0; i < n; i++) {
			snprintf(b, sizeof(b), "cache:   %.23s %.15s at 0x%06lx, %lu bytes, CRC %08lx", it[i].name, it[i].ver,
			         (unsigned long)(CACHE_OFF + it[i].off), (unsigned long)it[i].len, (unsigned long)it[i].crc);
			sv_log(b);
		}
	}
	msc_hooks(&hooks);
	sv_init(&sv_io, power_on);
}
