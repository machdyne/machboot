/*
 * machboot
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Serial console and key input.
 *
 * Keys come from the serial console today (terminal escape sequences
 * decoded into KEY_* below) and from a USB keyboard;
 * the menu sees only KEY_* codes and does not know which.
 */

#ifndef MB_CONSOLE_H
#define MB_CONSOLE_H

#include <stdint.h>

enum {
	KEY_NONE = 0,
	/* printable characters are their ASCII codes, 0x20-0x7e */
	KEY_ENTER = 0x100,
	KEY_ESC,
	KEY_UP,
	KEY_DOWN,
	KEY_LEFT,
	KEY_RIGHT,
	KEY_TAB,
	KEY_BKSP,
	KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6,
	KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12,
	KEY_DEL,
};

void con_init(void);
void con_putc(char c);
void con_puts(const char *s);
void con_printf(const char *fmt, ...);
int con_getc(void);		/* -1 if nothing waiting */

int key_poll(void);		/* KEY_NONE if no key; also steps the USB keyboards */
extern int kbd_layout;		/* 0 US, 1 German */
const char *key_name(int k);

uint32_t ms_now(void);
void ms_sleep(uint32_t ms);

#endif
