/*
 * machboot
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Installing into the module's flash, for builds that never do it
 * (Sergei, Mozart: the module's flash holds the module's own system, and
 * machboot installs into the RP2040's flash instead -- fw/systems.c).
 * The menu never offers it there (can_install() is 0 in SRAM builds).
 * fw/install.c is the real one.
 */

#include "install.h"

const char *install_error(int r)
{
	(void)r;
	return "not on this board";
}

int install(card_entry_t *ce, void (*progress)(int, const char *))
{
	(void)ce; (void)progress;
	return INST_FLASH;
}
volatile int install_cancel;
