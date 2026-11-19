/*
 * machboot
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * USB on the host ports (docs/usb.md): keyboards, hubs, flash drives.
 */

#ifndef MB_USB_H
#define MB_USB_H

#include <stdint.h>

#define USB_PORTS 2
#define USB_DEVS 6		/* the two root ports, and four behind hubs */

enum {
	USB_NONE,		/* nothing plugged in */
	USB_BUSY,		/* being set up */
	USB_KEYBOARD,		/* working */
	USB_OTHER,		/* not something machboot uses: ignored */
	USB_HUB,		/* a hub: its ports are served */
	USB_FAILED,		/* did not answer properly, gave up until unplugged */
	USB_DRIVE,		/* a flash drive: read like the SD card */
	USB_NOROOM,		/* a third keyboard, a third hub: no room for it */
};

typedef struct {
	int kind;
	uint16_t vid, pid;
	int lowspeed;
	int root;		/* root port 0 or 1 */
	int depth;		/* 0: on the root port; 1: behind a hub ... */
	int hub_port;		/* behind a hub: its port, from 1 */
	int fail_state;		/* where it failed, for F1 details */
	int fail_status;
	uint32_t sectors;	/* USB_DRIVE */
} usb_dev_info_t;

int usb_init(void);			/* 0 if there is no USB host in the gateware */
void usb_step(void);			/* call often; never blocks */
/* the devices, in tree order (each root port, then what is behind it);
 * 0 when there are no more */
int usb_dev_info(int n, usb_dev_info_t *di);
int usb_keyboard_block(int blk);	/* blk (0 or 1) if a keyboard reports into it, else -1 */

/* a flash drive (the first one): sectors of 512 bytes */
int usb_drive_ready(void);		/* 1: mounted and answering */
int usb_drive_changed(void);		/* 1 once after a drive came or went */
uint32_t usb_drive_sectors(void);
int usb_drive_read(uint32_t lba, uint8_t *buf);	/* 0: ok */

#endif
