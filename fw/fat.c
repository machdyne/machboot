/*
 * machboot
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Written from Microsoft's "FAT: General Overview of On-Disk Format"
 * (fatgen103): the BPB, the FAT type decided by the cluster count alone
 * (< 4085 FAT12, < 65525 FAT16, else FAT32), the FAT12 entry spanning a
 * sector boundary, the fixed FAT12/16 root directory, directory entries
 * of 32 bytes (0x00 ends the directory, 0xE5 is deleted, attribute 0x0F
 * is a long-name entry, 0x08 a volume label), and end-of-chain marks.
 *
 * The partition table is the PC MBR: 0x55AA at 510, four 16-byte
 * entries at 446; FAT types 0x01, 0x04, 0x06, 0x0B, 0x0C, 0x0E. A card
 * formatted without one (a "superfloppy") has the BPB in sector 0.
 *
 * Long names are skipped: machboot's own names are 8.3 (docs/sdcard.md),
 * and every FAT driver writes a short name beside a long one.
 */

#include "fat.h"
#include "sd.h"
#include "usb.h"
#include "lib.h"

fat_vol_t fat_vols[FAT_MAX_VOLS];

static uint8_t sec[512];
static uint32_t sec_lba = 0xffffffffu;
static int sec_dev, cur_dev;		/* the device: each public function sets cur_dev */

/* the sector cache's buffer, for someone who needs 512 bytes for a moment
   (diagnostics): the cache forgets what it held */
uint8_t *fat_scratch(void)
{
	sec_lba = 0xffffffffu;
	return sec;
}

static int blk_read(uint32_t lba, uint8_t *buf)
{
	return cur_dev == FAT_USB ? usb_drive_read(lba, buf) : sd_read(lba, buf);
}

static int read_sec(uint32_t lba)
{
	if (lba == sec_lba && sec_dev == cur_dev)
		return 0;
	sec_lba = 0xffffffffu;
	if (blk_read(lba, sec))
		return -1;
	sec_lba = lba;
	sec_dev = cur_dev;
	return 0;
}

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* a BPB at lba? fill v */
static int try_bpb(uint32_t lba, int part, fat_vol_t *v)
{
	if (read_sec(lba))
		return 0;
	const uint8_t *b = sec;
	if (!(b[0] == 0xEB || b[0] == 0xE9) || le16(b + 11) != 512 || le16(b + 510) != 0xAA55)
		return 0;
	uint32_t spc = b[13];
	uint32_t rsvd = le16(b + 14);
	uint32_t nfats = b[16];
	uint32_t rootent = le16(b + 17);
	uint32_t tot = le16(b + 19) ? le16(b + 19) : le32(b + 32);
	uint32_t fatsz = le16(b + 22) ? le16(b + 22) : le32(b + 36);
	if (!spc || (spc & (spc - 1)) || !rsvd || !nfats || !fatsz || !tot)
		return 0;
	uint32_t rootsec = (rootent * 32 + 511) / 512;
	uint32_t data = rsvd + nfats * fatsz + rootsec;
	if (data >= tot)
		return 0;
	uint32_t clusters = (tot - data) / spc;

	memset(v, 0, sizeof(*v));
	v->dev = cur_dev;
	v->type = clusters < 4085 ? 12 : clusters < 65525 ? 16 : 32;
	v->part = part;
	v->start = lba;
	v->spc = spc;
	v->clusters = clusters;
	v->fat_lba = lba + rsvd;
	v->root_lba = lba + rsvd + nfats * fatsz;
	v->root_entries = rootent;
	v->data_lba = lba + data;
	v->root_cluster = v->type == 32 ? le32(b + 44) : 0;
	const uint8_t *lab = b + (v->type == 32 ? 71 : 43);
	int n = 0;
	for (int i = 0; i < 11; i++)
		v->label[n++] = (char)lab[i];
	while (n > 0 && v->label[n - 1] == ' ')
		n--;
	v->label[n] = 0;
	return 1;
}

int fat_mount(int dev)
{
	fat_vol_t *vols = &fat_vols[dev == FAT_USB ? 4 : 0];
	int n = 0;
	memset(vols, 0, 4 * sizeof(*vols));
	cur_dev = dev;
	sec_lba = 0xffffffffu;

	if (try_bpb(0, 0, &vols[0]))
		return 1;
	if (read_sec(0) || le16(sec + 510) != 0xAA55)
		return 0;

	uint8_t table[64];
	memcpy(table, sec + 446, 64);
	for (int i = 0; i < 4; i++) {
		const uint8_t *e = table + i * 16;
		uint8_t t = e[4];
		uint32_t lba = le32(e + 8);
		if (!(t == 0x01 || t == 0x04 || t == 0x06 || t == 0x0B || t == 0x0C || t == 0x0E) || !lba)
			continue;
		if (try_bpb(lba, i + 1, &vols[n]))
			n++;
	}
	return n;
}

static uint32_t fat_next(fat_vol_t *v, uint32_t c)
{
	uint32_t off, lba, val;
	if (v->type == 12) {
		off = c + c / 2;
		lba = v->fat_lba + off / 512;
		if (read_sec(lba))
			return 0x0fffffff;
		uint32_t lo = sec[off % 512];
		if (off % 512 == 511) {
			if (read_sec(lba + 1))
				return 0x0fffffff;
			val = lo | (uint32_t)sec[0] << 8;
		} else {
			val = lo | (uint32_t)sec[off % 512 + 1] << 8;
		}
		val = (c & 1) ? val >> 4 : val & 0xfff;
		return val >= 0xff8 ? 0x0fffffff : val;
	}
	if (v->type == 16) {
		off = c * 2;
		if (read_sec(v->fat_lba + off / 512))
			return 0x0fffffff;
		val = le16(sec + off % 512);
		return val >= 0xfff8 ? 0x0fffffff : val;
	}
	off = c * 4;
	if (read_sec(v->fat_lba + off / 512))
		return 0x0fffffff;
	val = le32(sec + off % 512) & 0x0fffffff;
	return val >= 0x0ffffff8 ? 0x0fffffff : val;
}

static int cluster_ok(fat_vol_t *v, uint32_t c)
{
	return c >= 2 && c < v->clusters + 2;
}

static uint32_t cluster_lba(fat_vol_t *v, uint32_t c)
{
	return v->data_lba + (c - 2) * v->spc;
}

void fat_dir_open(fat_vol_t *v, uint32_t cluster, fat_dir_t *d)
{
	cur_dev = v->dev;
	d->v = v;
	d->cluster = (cluster == 0 && v->type == 32) ? v->root_cluster : cluster;
	d->index = 0;
}

/* the sector holding entry d->index, or 0 past the end */
static uint32_t dir_sector(fat_dir_t *d)
{
	fat_vol_t *v = d->v;
	uint32_t s = d->index / 16;
	if (d->cluster == 0) {
		if (d->index >= v->root_entries)
			return 0;
		return v->root_lba + s;
	}
	uint32_t c = d->cluster;
	uint32_t hops = s / v->spc;
	for (uint32_t i = 0; i < hops; i++) {
		c = fat_next(v, c);
		if (!cluster_ok(v, c))
			return 0;
	}
	return cluster_lba(v, c) + s % v->spc;
}

int fat_dir_next(fat_dir_t *d, fat_entry_t *e)
{
	cur_dev = d->v->dev;
	for (;;) {
		uint32_t lba = dir_sector(d);
		if (!lba || read_sec(lba))
			return 0;
		const uint8_t *p = sec + (d->index % 16) * 32;
		d->index++;
		if (p[0] == 0x00)
			return 0;
		if (p[0] == 0xE5 || (p[11] & 0x0F) == 0x0F || (p[11] & 0x08))
			continue;
		int n = 0;
		for (int i = 0; i < 8 && p[i] != ' '; i++)
			e->name[n++] = (char)(i == 0 && p[0] == 0x05 ? 0xE5 : p[i]);
		if (p[8] != ' ') {
			e->name[n++] = '.';
			for (int i = 8; i < 11 && p[i] != ' '; i++)
				e->name[n++] = (char)p[i];
		}
		e->name[n] = 0;
		if (!strcmp_ci(e->name, ".") || !strcmp_ci(e->name, ".."))
			continue;
		e->attr = p[11];
		e->cluster = (uint32_t)le16(p + 26) | (d->v->type == 32 ? (uint32_t)le16(p + 20) << 16 : 0);
		e->size = le32(p + 28);
		return 1;
	}
}

int fat_find(fat_vol_t *v, const char *path, fat_entry_t *e)
{
	cur_dev = v->dev;
	char part[13];
	uint32_t cluster = 0;
	fat_dir_t d;

	while (*path == '\\' || *path == '/')
		path++;
	while (*path) {
		int n = 0;
		while (*path && *path != '\\' && *path != '/' && n < 12)
			part[n++] = *path++;
		part[n] = 0;
		while (*path == '\\' || *path == '/')
			path++;
		fat_dir_open(v, cluster, &d);
		int found = 0;
		while (fat_dir_next(&d, e))
			if (!strcmp_ci(e->name, part)) {
				found = 1;
				break;
			}
		if (!found)
			return 0;
		if (*path) {
			if (!(e->attr & FAT_ATTR_DIR))
				return 0;
			cluster = e->cluster;
		}
	}
	return 1;
}

void fat_open(fat_vol_t *v, const fat_entry_t *e, fat_file_t *f)
{
	cur_dev = v->dev;
	f->v = v;
	f->first = e->cluster;
	f->size = e->size;
	fat_seek0(f);
}

int fat_seek0(fat_file_t *f)
{
	cur_dev = f->v->dev;
	f->pos = 0;
	f->cur = f->first;
	f->cur_n = 0;
	return 0;
}

int fat_read(fat_file_t *f, uint8_t *buf, uint32_t n)
{
	cur_dev = f->v->dev;
	fat_vol_t *v = f->v;
	uint32_t csize = v->spc * 512;
	uint32_t done = 0;

	if (n > f->size - f->pos)
		n = f->size - f->pos;
	while (done < n) {
		uint32_t cn = f->pos / csize;
		while (f->cur_n < cn) {
			f->cur = fat_next(v, f->cur);
			f->cur_n++;
		}
		if (!cluster_ok(v, f->cur))
			return -1;
		uint32_t in = f->pos % csize;
		uint32_t lba = cluster_lba(v, f->cur) + in / 512;
		uint32_t o = in % 512;
		uint32_t k = 512 - o;
		if (k > n - done)
			k = n - done;
		if (o == 0 && k == 512) {
			if (blk_read(lba, buf + done))
				return -1;
		} else {
			if (read_sec(lba))
				return -1;
			memcpy(buf + done, sec + o, k);
		}
		done += k;
		f->pos += k;
	}
	return (int)done;
}
