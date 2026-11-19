/*
 * machboot
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 *   \SYS\GATEWARE\ZEITLOS\SYSTEM.CFG    name=Zeitlos / version=0.0.8 (key=value lines)
 *   \SYS\GATEWARE\ZEITLOS\LAKRITZ.BIT   the bitstream for this board
 *   \SYS\GATEWARE\BLINK.BIT             a loose file
 *
 * Every FAT partition is searched, in order, for \SYS\GATEWARE (the card
 * may be shared with DOS: Zeitlos and the DOS boot files on the first
 * partition, ISOs on the second). The first one found is used.
 */

#include "card.h"
#include "sd.h"
#include "usb.h"
#include "mb.h"
#include "hw.h"
#include "lib.h"
#include "console.h"

card_t card;

const char *board_file(uint32_t board)
{
	static char s[13];
	if ((board & 0xff) == 2 || (board & 0xff) == 3) {
		/* SERGEI0.BIT ... MOZART2.BIT: one file per carrier and module */
		mb_snprintf(s, sizeof(s), "%s%u.BIT", (board & 0xff) == 2 ? "SERGEI" : "MOZART", (board >> 8) & 0xff);
		return s;
	}
	switch (board & 0xff) {
	case 1: return "LAKRITZ.BIT";
	case 4: return "OBST.BIT";
	case 5: return "KONFEKT.BIT";
	case 6: return "MINZE.BIT";
	case 7: return "SCHOKO.BIT";
	case 8: return "NOIR.BIT";
	case 9: return "KLINGE.BIT";
	default: return "BOARD.BIT";
	}
}

static int ends_bit(const char *n)
{
	int l = (int)strlen(n);
	return l > 4 && !strcmp_ci(n + l - 4, ".BIT");
}

static void copy(char *d, const char *s, int cap)
{
	int i = 0;
	for (; s[i] && i < cap - 1; i++)
		d[i] = s[i];
	d[i] = 0;
}

/* name= and version= from SYSTEM.CFG */
static void read_cfg(fat_vol_t *v, const fat_entry_t *fe, card_entry_t *ce)
{
	static uint8_t buf[512];
	fat_file_t f;
	fat_open(v, fe, &f);
	int n = fat_read(&f, buf, sizeof(buf) - 1);
	if (n <= 0)
		return;
	buf[n] = 0;
	char *p = (char *)buf;
	while (*p) {
		char *line = p;
		while (*p && *p != '\n' && *p != '\r')
			p++;
		if (*p)
			*p++ = 0;
		while (*line == ' ' || *line == '\t')
			line++;
		if (*line == '#' || !*line)
			continue;
		char *eq = line;
		while (*eq && *eq != '=')
			eq++;
		if (!*eq)
			continue;
		*eq = 0;
		char *val = eq + 1;
		char *k = eq - 1;
		while (k >= line && (*k == ' ' || *k == '\t'))
			*k-- = 0;
		while (*val == ' ' || *val == '\t')
			val++;
		int l = (int)strlen(val);
		while (l > 0 && (val[l - 1] == ' ' || val[l - 1] == '\t'))
			val[--l] = 0;
		if (!strcmp_ci(line, "name"))
			copy(ce->name, val, sizeof(ce->name));
		else if (!strcmp_ci(line, "version"))
			copy(ce->ver, val, sizeof(ce->ver));
	}
}

/* \SYS\GATEWARE on one device's volumes: its entries go after what is there */
static fat_vol_t *scan_dev(int dev)
{
	fat_entry_t e, sub;
	fat_dir_t d, dd;
	fat_vol_t *vol = 0;
	int first = dev == FAT_USB ? 4 : 0;

	for (int i = first; i < first + 4 && !vol; i++) {
		if (!fat_vols[i].type)
			continue;
		if (fat_find(&fat_vols[i], "\\SYS\\GATEWARE", &e) && (e.attr & FAT_ATTR_DIR))
			vol = &fat_vols[i];
	}
	if (!vol)
		return 0;

	const char *want = board_file(mb.board);
	fat_dir_open(vol, e.cluster, &d);
	while (card.n < CARD_MAX && fat_dir_next(&d, &e)) {
		card_entry_t *ce = &card.e[card.n];
		memset(ce, 0, sizeof(*ce));
		ce->vol = vol;
		ce->usb = dev == FAT_USB;
		if (e.attr & FAT_ATTR_DIR) {
			ce->kind = CE_SYSTEM;
			copy(ce->dir, e.name, sizeof(ce->dir));
			copy(ce->name, e.name, sizeof(ce->name));
			int has_cfg = 0;
			fat_dir_open(vol, e.cluster, &dd);
			while (fat_dir_next(&dd, &sub)) {
				if (!strcmp_ci(sub.name, "SYSTEM.CFG")) {
					read_cfg(vol, &sub, ce);
					has_cfg = 1;
				} else if (!strcmp_ci(sub.name, want) && !(sub.attr & FAT_ATTR_DIR)) {
					ce->bit = sub;
					ce->has_bit = 1;
				}
			}
			if (has_cfg)
				card.n++;
		} else if (ends_bit(e.name)) {
			ce->kind = CE_FILE;
			copy(ce->name, e.name, sizeof(ce->name));
			ce->bit = e;
			ce->has_bit = 1;
			card.n++;
		}
	}
	return vol;
}

void card_scan(void)
{
	memset(&card, 0, sizeof(card));
	if ((mb.features & FEAT_SD) && !sd_init()) {
		card.sd_ok = 1;
		card.vols = fat_mount(FAT_SD);
		card.vol = scan_dev(FAT_SD);
		card.found = card.vol != 0;
		if (card.found)
			con_printf("card: %d volume(s), \\SYS\\GATEWARE on partition %d, %d entries\n",
			           card.vols, card.vol->part, card.n);
	}
	/* a USB flash drive, the same way, after the card's */
	if (usb_drive_ready() && fat_mount(FAT_USB)) {
		int n0 = card.n;
		if (scan_dev(FAT_USB)) {
			card.usb = 1;
			con_printf("usb drive: \\SYS\\GATEWARE, %d entries\n", card.n - n0);
		}
	}
}

card_entry_t *card_system(const char *name)
{
	for (int i = 0; i < card.n; i++)
		if (card.e[i].kind == CE_SYSTEM && !strcmp_ci(card.e[i].name, name))
			return &card.e[i];
	return 0;
}
