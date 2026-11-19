/*
 * machboot supervisor
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The card reader's USB side: TinyUSB's mass-storage callbacks over
 * src/msc.c. TinyUSB answers INQUIRY, READ CAPACITY, MODE SENSE, REQUEST
 * SENSE itself from these; CFG_TUD_MSC_EP_BUFSIZE (1536) makes every
 * READ/WRITE callback a whole number of sectors, at most MSC_MAX_SECT:
 * one request on the link.
 */

#include <string.h>
#include "tusb.h"
#include "msc.h"

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16], uint8_t product_rev[4])
{
	(void)lun;
	memcpy(vendor_id, "Machdyne", 8);
	memcpy(product_id, "SD card reader  ", 16);
	memcpy(product_rev, "0.3 ", 4);
}

bool tud_msc_test_unit_ready_cb(uint8_t lun)
{
	uint8_t k, a, q;
	if (msc_ready(&k, &a, &q))
		return true;
	tud_msc_set_sense(lun, k, a, q);
	return false;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size)
{
	(void)lun;
	*block_count = msc_sectors();
	*block_size = 512;
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject)
{
	(void)lun; (void)power_condition;
	if (load_eject && !start)
		msc_eject();		/* the PC ejected it: the card is machboot's again */
	return true;
}

bool tud_msc_is_writable_cb(uint8_t lun)
{
	(void)lun;
	return true;
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize)
{
	if (offset || bufsize % 512 || msc_read(lba, buffer, bufsize / 512)) {
		tud_msc_set_sense(lun, 0x03, 0x11, 0x00);	/* MEDIUM ERROR, unrecovered read */
		return -1;
	}
	return (int32_t)bufsize;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize)
{
	if (offset || bufsize % 512 || msc_write(lba, buffer, bufsize / 512)) {
		tud_msc_set_sense(lun, 0x03, 0x0C, 0x00);	/* MEDIUM ERROR, write error */
		return -1;
	}
	return (int32_t)bufsize;
}

int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void *buffer, uint16_t bufsize)
{
	(void)buffer; (void)bufsize;
	switch (scsi_cmd[0]) {
	case 0x1E:		/* PREVENT ALLOW MEDIUM REMOVAL */
	case 0x35:		/* SYNCHRONIZE CACHE (10): every write is on the card when it is acknowledged */
		return 0;
	default:
		tud_msc_set_sense(lun, 0x05, 0x20, 0x00);	/* ILLEGAL REQUEST, invalid command */
		return -1;
	}
}
