/*
 * machboot
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Keys (all values text, so `kv` on Zeitlos could show them one day):
 *
 *   boot.auto     "1" / "0"      start the system by itself
 *   boot.wait     "3"            seconds
 *   boot.kbd      "us" / "de"    keyboard layout
 *   sys.name      "Zeitlos"      what the last install put in the slot
 *   sys.ver       "0.0.8"
 *   sys.crc       "1a2b3c4d"     its CRC-32, hex
 *   sys.size      "481234"       its size, decimal
 *   sys.list      "Zeitlos|Mach86"  Start's list, in order (MB_SLOT: Lakritz,
 *                                Konfekt; docs/systems.md)
 *
 * Only keys whose value changed are written; kvlog appends, and erases
 * a sector only when one fills (hundreds of changes).
 */

#include "settings.h"
#include "kvlog.h"
#include "flash.h"
#include "hw.h"
#include "lib.h"
#include "mb.h"
#ifdef MB_SLOT
#include "systems.h"
#endif

settings_t cfg;
static int writable;
int settings_remote;		/* fw/xlink.c: the RP2040 keeps them (Sergei, Mozart) */
int settings_dirty;		/* ... and has not seen the latest yet */
static uint32_t base;		/* flash address of the store */
static kv_work_t work;

static uint8_t dev_read(void *ctx, uint32_t off) { (void)ctx; return flash_rd8(base + off); }
static int dev_erase(void *ctx, uint32_t off) { (void)ctx; return flash_erase4k(base + off); }
static int dev_program(void *ctx, uint32_t off, const uint8_t *p, uint32_t n)
{
	(void)ctx;
	return flash_program(base + off, p, n);
}

static const kv_dev_t dev = { dev_read, dev_erase, dev_program, 0 };

static int get(const char *key, char *buf, uint32_t cap)
{
	uint32_t len = 0;
	if (kv_get(&dev, key, (uint8_t *)buf, cap - 1, &len) != KV_OK)
		return 0;
	buf[len] = 0;
	return 1;
}

static uint32_t num(const char *s, int base16)
{
	uint32_t v = 0;
	for (; *s; s++) {
		int d;
		if (*s >= '0' && *s <= '9') d = *s - '0';
		else if (base16 && *s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
		else if (base16 && *s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
		else break;
		v = v * (base16 ? 16 : 10) + (uint32_t)d;
	}
	return v;
}

void settings_load(int can_write)
{
	char b[32];

	writable = can_write;
	base = (mb.fw_base ? mb.fw_base - FW_OFFSET : 0) + SETTINGS_OFFSET;

	memset(&cfg, 0, sizeof(cfg));
	cfg.autostart = 1;
	cfg.wait_s = 3;
	cfg.kbd = KBD_US;

	if (!mb.fw_base || mb.install_mode)
		return;		/* JTAG test build, or install mode: no settings of ours in flash */

	if (get("boot.auto", b, sizeof(b))) cfg.autostart = b[0] != '0';
	if (get("boot.wait", b, sizeof(b))) {
		uint32_t w = num(b, 0);
		if (w >= 1 && w <= 30) cfg.wait_s = (int)w;
	}
	if (get("boot.kbd", b, sizeof(b))) cfg.kbd = (b[0] == 'd') ? KBD_DE : KBD_US;
	get("sys.name", cfg.sys_name, sizeof(cfg.sys_name));
	get("sys.ver", cfg.sys_ver, sizeof(cfg.sys_ver));
	if (get("sys.crc", b, sizeof(b))) cfg.sys_crc = num(b, 1);
	if (get("sys.size", b, sizeof(b))) cfg.sys_size = num(b, 0);
#ifdef MB_SLOT
	{
		static char l[KV_VAL_MAX + 1];
		if (get("sys.list", l, sizeof(l)))
			systems_from_names(l);
	}
#endif
}

void settings_attach(uint32_t addr)
{
	base = addr;
	writable = 1;
}

int settings_writable(void)
{
	return writable || settings_remote;
}

/* write key=val if it differs from what is stored */
static int put(const char *key, const char *val)
{
#ifdef MB_SLOT
	static char cur[KV_VAL_MAX + 1];	/* the whole value (sys.list), or a long one would always differ */
#else
	char cur[64];				/* (the JTAG test build is a few bytes from full) */
#endif
	if (get(key, cur, sizeof(cur)) && strlen(cur) == strlen(val) && !memcmp(cur, val, strlen(val)))
		return 0;
	return kv_set(&dev, &work, key, (const uint8_t *)val, (uint32_t)strlen(val), 0) == KV_OK ? 0 : -1;
}

int settings_save(void)
{
	char b[16];
	if (settings_remote) {
		settings_dirty = 1;	/* fw/xlink.c hands them to the RP2040 */
		return 0;
	}
	if (!writable)
		return -1;
	int r = 0;
	r |= put("boot.auto", cfg.autostart ? "1" : "0");
	mb_snprintf(b, sizeof(b), "%d", cfg.wait_s);
	r |= put("boot.wait", b);
	r |= put("boot.kbd", cfg.kbd == KBD_DE ? "de" : "us");
#ifdef MB_SLOT
	{
		static char l[KV_VAL_MAX];
		systems_names(l, sizeof(l));
		r |= put("sys.list", l);
	}
#endif
	return r;
}

int settings_set_system(const char *name, const char *ver, uint32_t crc, uint32_t size)
{
	char b[16];
	memset(cfg.sys_name, 0, sizeof(cfg.sys_name));
	memset(cfg.sys_ver, 0, sizeof(cfg.sys_ver));
	memcpy(cfg.sys_name, name, strlen(name) < sizeof(cfg.sys_name) ? strlen(name) : sizeof(cfg.sys_name) - 1);
	memcpy(cfg.sys_ver, ver, strlen(ver) < sizeof(cfg.sys_ver) ? strlen(ver) : sizeof(cfg.sys_ver) - 1);
	cfg.sys_crc = crc;
	cfg.sys_size = size;
	if (!writable)
		return -1;
	int r = 0;
	r |= put("sys.name", cfg.sys_name);
	r |= put("sys.ver", cfg.sys_ver);
	mb_snprintf(b, sizeof(b), "%08x", crc);
	r |= put("sys.crc", b);
	mb_snprintf(b, sizeof(b), "%u", size);
	r |= put("sys.size", b);
	return r;
}
