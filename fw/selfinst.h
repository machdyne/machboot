/*
 * machboot
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Install mode: machboot, written by tinydfu's user alt at 0x040000,
 * installs itself at 0x000000 (docs/install-mode.md).
 */

#ifndef MB_SELFINST_H
#define MB_SELFINST_H

#include <stdint.h>

/*
 * The installer image (tools/mkinstaller.py), at tinydfu's 0x040000:
 *
 *   +0x000000  the installer's bitstream (fence turned around)
 *   +0x050000  the firmware (the same firmware as the boot slot's)
 *   +0x05E000  this header (the boot slot's settings sectors; unused here)
 *   +0x060000  the payload: the boot-slot image for 0x000000
 */
#define SI_HEADER_OFFSET	0x05E000u	/* where the boot slot keeps its settings */
#define SI_MAGIC		0x4E49424Du	/* "MBIN" */

typedef struct {
	uint32_t magic;
	uint32_t format;	/* 1 */
	uint32_t payload_off;	/* from the start of the installer image */
	uint32_t payload_len;
	uint32_t payload_crc;	/* CRC-32 */
	uint32_t idcode;	/* the FPGA the payload is for */
	uint32_t version;	/* machboot version of the payload */
	uint32_t reserved;
} si_header_t;

enum {
	SI_NONE = 0,		/* not an installer image, or not the installer build */
	SI_READY,		/* can install */
	SI_BAD,			/* the image is damaged or for another FPGA */
};

typedef struct {
	int state;
	si_header_t h;
	uint32_t src;		/* flash address of the payload */
	uint32_t fail_at;	/* where a write failed */
} selfinst_t;

extern selfinst_t si;

void selfinst_probe(void);

enum { SI_OK = 0, SI_ERR_IMAGE, SI_ERR_FLASH };
int selfinst_run(void (*progress)(int permille, const char *what));

#endif
