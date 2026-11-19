/*
 * machboot
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Reading the configuration flash: its identity, and what is in a slot.
 */

#ifndef MB_FLASH_H
#define MB_FLASH_H

#include <stdint.h>

typedef struct {
	uint32_t jedec;		/* manufacturer << 16 | type << 8 | capacity */
	uint32_t size;		/* bytes, from the capacity code; 0 if unknown */
} flash_info_t;

typedef struct {
	int valid;		/* an ECP5 bitstream starts here */
	int has_header;		/* a .bit file's header is still in front of it */
	uint32_t preamble;	/* flash address of the 0xBDB3 preamble */
	uint32_t idcode;	/* from its VERIFY_ID command; 0 if none found */
	char part[24];		/* "LFE5U-25F-6CABGA256" from the .bit header, if any */
} slot_info_t;

void flash_identify(flash_info_t *fi);

/* writing: 0 on success, -1 refused (fence, not armed) or failed */
int flash_erase4k(uint32_t addr);
int flash_program(uint32_t addr, const uint8_t *p, uint32_t n);	/* within one 256-byte page */
void flash_slot(uint32_t addr, slot_info_t *si);
void bit_parse(uint8_t (*get)(void *ctx, uint32_t off), void *ctx, uint32_t limit, slot_info_t *si);
const char *die_name(uint32_t idcode);

/* the firmware image header (tools/mkfw.py, bootrom/boot.c) */
#define FW_MAGIC	0x5746424Du	/* "MBFW" */
typedef struct {
	uint32_t magic;
	uint32_t hwver;
	uint32_t length;
	uint32_t load;
	uint32_t entry;
	uint32_t crc;
	uint32_t version;	/* major << 16 | minor << 8 | patch */
	uint32_t reserved;
} fw_header_t;

#endif
