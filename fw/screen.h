/*
 * machboot
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The 80 x 30 text screen: a copy in RAM (the hardware is write-only),
 * drawing helpers, and the code page's graphics (docs/font.md).
 */

#ifndef MB_SCREEN_H
#define MB_SCREEN_H

#include <stdint.h>

/* colours (rtl/mb_text.v's palette) */
enum {
	C_BLACK, C_BLUE, C_GREEN, C_CYAN, C_RED, C_MAGENTA, C_BROWN, C_GREY,
	C_DGREY, C_LBLUE, C_LGREEN, C_LCYAN, C_LRED, C_LMAGENTA, C_YELLOW, C_WHITE
};

/* graphics in the code page (docs/font.md) */
#define G_BULLET	"\x07"
#define G_TRI_R		"\x10"
#define G_TRI_L		"\x11"
#define G_UP		"\x18"
#define G_DOWN		"\x19"
#define G_RIGHT		"\x1A"
#define G_LEFT		"\x1B"
#define G_SHADE1	"\xB0"
#define G_SHADE2	"\xB1"
#define G_SHADE3	"\xB2"
#define G_VLINE		"\xB3"
#define G_HLINE		"\xC4"
#define G_BLOCK		"\xDB"
#define G_DOT		"\xFA"
#define G_CHECK		"\xFB"
#define G_SQUARE	"\xFE"

#define CH_HLINE	0xC4
#define CH_BLOCK	0xDB
#define CH_SHADE1	0xB0

extern uint16_t scr[30][80];

void scr_clear(int fg, int bg);
void scr_fill(int x, int y, int w, int h, int fg, int bg, uint8_t ch);
void scr_put(int x, int y, const char *s, int fg, int bg);	/* bg < 0: keep */
void scr_putf(int x, int y, int fg, int bg, const char *fmt, ...);
void scr_center(int y, const char *s, int fg, int bg);
void scr_hrule(int x, int y, int w, int fg, int bg);
void scr_bar(int x, int y, int w, int permille, int fg, int bg);

/* push the copy to the hardware */
void scr_flush(void);

/* the row as plain ASCII (graphics replaced), trimmed; for the serial mirror */
void scr_row_text(int y, int x0, int x1, char *out, int outsz);

#endif
