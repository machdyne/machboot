/*
 * machboot
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * FAT12/16/32, read only, short (8.3) names (docs/sdcard.md).
 */

#ifndef MB_FAT_H
#define MB_FAT_H

#include <stdint.h>

#define FAT_MAX_VOLS 8		/* the SD card's at 0-3, a USB flash drive's at 4-7 */
#define FAT_SD 0
#define FAT_USB 1

typedef struct {
	int type;		/* 12, 16, 32; 0: no volume */
	int dev;		/* FAT_SD or FAT_USB */
	int part;		/* partition 1-4, or 0 for a card without a partition table */
	uint32_t start;		/* first sector */
	uint32_t fat_lba, data_lba, root_lba;
	uint32_t root_entries;	/* FAT12/16 */
	uint32_t root_cluster;	/* FAT32 */
	uint32_t spc;		/* sectors per cluster */
	uint32_t clusters;
	char label[12];
} fat_vol_t;

typedef struct {
	char name[13];		/* "NAME.EXT" */
	uint8_t attr;
	uint32_t cluster;
	uint32_t size;
} fat_entry_t;

typedef struct {
	fat_vol_t *v;
	uint32_t cluster;	/* 0: a FAT12/16 root directory */
	uint32_t index;		/* entry number within the directory */
} fat_dir_t;

typedef struct {
	fat_vol_t *v;
	uint32_t first, size, pos;
	uint32_t cur;		/* cluster holding pos */
	uint32_t cur_n;		/* its index in the chain */
} fat_file_t;

#define FAT_ATTR_DIR	0x10

extern fat_vol_t fat_vols[FAT_MAX_VOLS];

int fat_mount(int dev);					/* volumes found on the SD card or the drive */
void fat_dir_open(fat_vol_t *v, uint32_t cluster, fat_dir_t *d);
int fat_dir_next(fat_dir_t *d, fat_entry_t *e);		/* 1: an entry; 0: the end */
int fat_find(fat_vol_t *v, const char *path, fat_entry_t *e);	/* "\\SYS\\GATEWARE"; 1: found */
void fat_open(fat_vol_t *v, const fat_entry_t *e, fat_file_t *f);
int fat_read(fat_file_t *f, uint8_t *buf, uint32_t n);	/* bytes read, -1 on error */
int fat_seek0(fat_file_t *f);

uint8_t *fat_scratch(void);	/* 512 bytes; the sector cache forgets what it held */

#endif
