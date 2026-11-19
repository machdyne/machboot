/*
 * machboot
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Install mode for builds that never have it (Sergei, Mozart: the menu
 * is loaded into SRAM and the module's flash is not machboot's). The
 * menu sees "not an installer image" and never offers it. fw/selfinst.c
 * is the real one.
 */

#include "selfinst.h"

selfinst_t si;

void selfinst_probe(void)
{
	si.state = SI_NONE;
}

int selfinst_run(void (*progress)(int permille, const char *what))
{
	(void)progress;
	return SI_ERR_IMAGE;
}
