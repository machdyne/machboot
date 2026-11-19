/*
 * machboot supervisor
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Loading an ECP5's configuration SRAM over JTAG (docs/power-on.md).
 * Portable: the pins are jtag_io.h's.
 *
 * The sequence is the one Lattice documents for SRAM configuration over
 * JTAG (TN1260, "sysCONFIG", the ISC commands), as openFPGALoader's
 * ECP5 SRAM load also runs it: IDCODE, ISC_ENABLE, ISC_ERASE (SRAM),
 * LSC_INIT_ADDRESS, LSC_BITSTREAM_BURST, then the status check and
 * ISC_DISABLE, after which the FPGA wakes up with the new design. An
 * ISC_ENABLE also stops a configuration from the SPI flash that is in
 * progress, so this works whatever the FPGA was doing.
 */

#ifndef ECP5_H
#define ECP5_H

#include <stdint.h>

enum {
	ECP5_OK = 0,
	ECP5_ERR_IDCODE,	/* not the FPGA the bitstream is for (or no FPGA) */
	ECP5_ERR_ENABLE,	/* ISC_ENABLE did not take */
	ECP5_ERR_ERASE,
	ECP5_ERR_CONFIG,	/* the bitstream was rejected (status after the burst) */
	ECP5_ERR_DISABLE,
	ECP5_ERR_DONE,		/* configured without an error, but DONE stayed low: it does not run */
	ECP5_ABORTED,		/* the PC wanted JTAG (ecp5_load's yield said so) */
};

uint32_t ecp5_idcode(void);
extern uint32_t ecp5_last_idcode;	/* what the last ecp5_idcode() read */
uint32_t ecp5_status(void);

/*
 * Load len bytes of bitstream (the .bit file's bytes, as they are) into
 * SRAM. yield() is called between chunks; if it returns non-zero the
 * load stops (the TAP is left reset).
 */
int ecp5_blank(uint32_t idcode);	/* erased, every pin floating; ecp5_load() goes on from there */
int ecp5_load(const uint8_t *bit, uint32_t len, uint32_t idcode, int (*yield)(void));

const char *ecp5_error(int e);

#endif
