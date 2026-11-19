/*
 * machboot
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * SD card in SPI mode (docs/sdcard.md): reads, and single-block writes
 * for the card reader (docs/usb-device.md).
 */

#ifndef MB_SD_H
#define MB_SD_H

#include <stdint.h>

typedef struct {
	int present;
	int sdhc;		/* block addressed (SDHC/SDXC) */
	int version;		/* 1 or 2 (physical layer spec version family) */
	uint32_t sectors;	/* capacity in 512-byte sectors, 0 if unknown */
} sd_info_t;

extern sd_info_t sd;

int sd_init(void);					/* 0: ready */
int sd_read(uint32_t lba, uint8_t *buf);		/* one 512-byte sector; 0: ok */
int sd_write(uint32_t lba, const uint8_t *buf);	/* one 512-byte sector; 0: ok */

#endif
