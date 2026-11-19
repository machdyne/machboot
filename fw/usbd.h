/*
 * machboot
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The USB device on the USB-C socket: a serial console (CDC-ACM) and
 * DFU into the system slot (docs/usb-device.md).
 */

#ifndef MB_USBD_H
#define MB_USBD_H

#include <stdint.h>

/*
 * Two implementations: fw/usbd.c, the FPGA's own USB device (Lakritz,
 * Konfekt), and fw/xlink.c, the card reader through the RP2040 over the
 * XA-XD link (Sergei, Mozart: docs/xlink.md). The menu cannot tell.
 */

int usbd_init(void);		/* 0 if the gateware has no USB device */
void usbd_poll(void);		/* call often: the menu's key loop does */

/* the console: bytes to the host if a terminal has the port open */
void usbd_cdc_putc(char c);
int usbd_cdc_getc(void);	/* -1 if nothing */

/* the card reader: share the SD card with the PC (1), or take it back */
void usbd_msc_share(int on);
void usbd_before_start(void);	/* just before PROGRAMN: Sergei's RP2040 must let it boot */

/* what the DFU side is doing, for the screen */
enum { UD_DFU_IDLE, UD_DFU_RECEIVING, UD_DFU_DONE, UD_DFU_FAILED };
typedef struct {
	int present;		/* the gateware has the device */
	int configured;		/* a host has configured it */
	int cdc_open;		/* a terminal has the port open (DTR) */
	int dfu;		/* UD_DFU_* */
	uint32_t dfu_bytes;	/* received so far */
	uint32_t dfu_slot_bytes;	/* of them, for the system slot */
	uint32_t dfu_region_bytes;	/* of them, for the Zeitlos region */
	const char *dfu_error;	/* UD_DFU_FAILED: why */
	int dfu_events;		/* counts every change, for the menu to redraw */
	int msc_shared;		/* the card is the PC's (Card reader page) */
	uint32_t msc_read, msc_written;	/* bytes, since it was shared */
	int msc_events;		/* counts commands, for the menu to redraw */
	int rp2040;		/* Sergei, Mozart: 0 not this board, 1 the RP2040 has not
				   answered (not machboot-sv firmware?), 2 it has */
} usbd_state_t;

extern usbd_state_t usbd;

#endif
