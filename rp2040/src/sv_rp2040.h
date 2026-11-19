/*
 * machboot supervisor
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 */

#ifndef SV_RP2040_H
#define SV_RP2040_H

#include <stdint.h>

extern uint32_t sv_jtag_activity_us;	/* main.c: the PC's last JTAG command */
extern int sv_jtag_seen;
void sv_board_init(void);

#define SV_VERSION "r12 (machboot 0.4)"
void sv_log(const char *s);
const char *sv_log_pending(uint32_t *n);	/* not yet shown on the CDC port */
void sv_log_taken(uint32_t n);
void sv_log_replay(void);			/* a terminal opened the port: all of it again */

#endif
