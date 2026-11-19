/*
 * machboot supervisor
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The card reader on Sergei and Mozart: a USB drive whose sectors are
 * the FPGA's SD card, over the XA-XD link (docs/xlink.md). The PC sees
 * a card reader; there is a card in it while machboot's Card reader
 * page shares it.
 *
 * Portable (tests/test_xlink.c runs it on a PC); src/msc_tusb.c is the
 * TinyUSB side.
 */

#ifndef MSC_H
#define MSC_H

#include <stdint.h>

#define MSC_MAX_SECT	3		/* per link request: 3 x 512 + header < 2048 */

typedef struct {
	int (*settings_get)(char *buf, int cap);	/* what the RP2040 keeps, for machboot */
	void (*settings_put)(const char *text, int len);	/* machboot's, changed: keep them */
	void (*start)(void);		/* machboot is starting the system: let that reset through */
	void (*run)(uint32_t crc);	/* machboot asked for that cached system: load it */
	int (*note_get)(char *buf, int cap);	/* something to tell machboot once it answers */
	void (*poll)(void);		/* during a long request: keep USB going */
	void (*log)(const char *line);	/* a line for the supervisor's log */
} msc_hooks_t;

void msc_hooks(const msc_hooks_t *h);
void msc_set_pc(int configured);	/* a PC has configured our USB */
void msc_tick(void);			/* call often: probing, polling, a HELLO every second */
void msc_link_event(void);		/* something may have loaded machboot: probe for a while */
void msc_link_drop(void);		/* machboot is gone (a reset): let go of the link */
int msc_link_state(void);		/* 0 down, 1 probing, 2 up */

/* TEST UNIT READY: 1 ready; 0 with the sense to report */
int msc_ready(uint8_t *key, uint8_t *asc, uint8_t *ascq);
uint32_t msc_sectors(void);
int msc_read(uint32_t lba, uint8_t *buf, uint32_t n);		/* 0 ok, -1 no medium, -2 error */
int msc_write(uint32_t lba, const uint8_t *buf, uint32_t n);
void msc_eject(void);
void msc_background(void);		/* call in the main loop: the fetch-ahead */
int msc_link(void);			/* machboot answers on the link */

#endif
