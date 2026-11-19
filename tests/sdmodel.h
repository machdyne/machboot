/*
 * machboot
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * An SD card in SPI mode, at the byte level: used by the PC build
 * (fw/host/hw_host.c) and, wrapped in a bit-level shifter, by the
 * whole-system simulation (sim/sim_main.cc). Written from the SD
 * Physical Layer Simplified Specification, chapter 7, independently of
 * fw/sd.c -- a model written from the driver would agree with its bugs.
 *
 * Answers CMD0, CMD8, CMD55/ACMD41 (idle for the first two tries),
 * CMD58 (OCR with CCS for SDHC), CMD16, CMD9 (a CSD 2.0) and CMD17;
 * everything else is "illegal command".
 */

#ifndef SDMODEL_H
#define SDMODEL_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void sdm_init(uint8_t *image, size_t size, int sdhc);
uint8_t sdm_xchg(uint8_t mosi);		/* one byte, card selected */
uint8_t sdm_peek(void);			/* what the next exchange will send back */
void sdm_deselect(void);
uint32_t sdm_reads(void);		/* CMD17s served */
uint32_t sdm_writes(void);		/* CMD24s done */

#ifdef __cplusplus
}
#endif

#endif
