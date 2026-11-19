/*
 * machboot
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 */

#include "screen.h"
#include "hw.h"
#include "lib.h"

uint16_t scr[30][80];

#define CELL(ch, fg, bg) ((uint16_t)(((bg) & 15) << 12 | ((fg) & 15) << 8 | ((ch) & 0xff)))

void scr_clear(int fg, int bg)
{
	scr_fill(0, 0, TEXT_COLS, TEXT_ROWS, fg, bg, ' ');
}

void scr_fill(int x, int y, int w, int h, int fg, int bg, uint8_t ch)
{
	for (int j = y; j < y + h; j++)
		for (int i = x; i < x + w; i++)
			if (i >= 0 && i < TEXT_COLS && j >= 0 && j < TEXT_ROWS)
				scr[j][i] = CELL(ch, fg, bg);
}

void scr_put(int x, int y, const char *s, int fg, int bg)
{
	if (y < 0 || y >= TEXT_ROWS)
		return;
	for (; *s; s++, x++) {
		if (x < 0 || x >= TEXT_COLS)
			continue;
		int b = bg < 0 ? (scr[y][x] >> 12) & 15 : bg;
		scr[y][x] = CELL((uint8_t)*s, fg, b);
	}
}

void scr_putf(int x, int y, int fg, int bg, const char *fmt, ...)
{
	char buf[96];
	va_list ap;
	va_start(ap, fmt);
	mb_vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	scr_put(x, y, buf, fg, bg);
}

void scr_center(int y, const char *s, int fg, int bg)
{
	scr_put((TEXT_COLS - (int)strlen(s)) / 2, y, s, fg, bg);
}

void scr_hrule(int x, int y, int w, int fg, int bg)
{
	scr_fill(x, y, w, 1, fg, bg, CH_HLINE);
}

void scr_bar(int x, int y, int w, int permille, int fg, int bg)
{
	int n = (w * permille + 500) / 1000;
	scr_fill(x, y, n, 1, fg, bg, CH_BLOCK);
	scr_fill(x + n, y, w - n, 1, C_DGREY, bg, CH_SHADE1);
}

/* all 2400 cells: about 0.2 ms, cheaper than a second copy to compare against */
void scr_flush(void)
{
	const uint16_t *c = &scr[0][0];
	for (int i = 0; i < TEXT_ROWS * TEXT_COLS; i++)
		text_wr(i, c[i]);
}

static char ascii_of(uint8_t c)
{
	if (c >= 0x20 && c < 0x7f)
		return (char)c;
	switch (c) {
	case 0x10: return '>';
	case 0x11: return '<';
	case 0x18: return '^';
	case 0x19: return 'v';
	case 0x1A: return '>';
	case 0x1B: return '<';
	case 0x07: return '*';
	case 0xFB: return '+';
	case 0xFA: return '.';
	case 0xC4: case 0xCD: return '-';
	case 0xB3: case 0xBA: return '|';
	case 0xDB: case 0xB2: return '#';
	case 0xB0: case 0xB1: return '.';
	case 0x84: return 'a';
	case 0x94: return 'o';
	case 0x81: return 'u';
	case 0x8E: return 'A';
	case 0x99: return 'O';
	case 0x9A: return 'U';
	case 0xE1: return 's';
	default: return ' ';
	}
}

void scr_row_text(int y, int x0, int x1, char *out, int outsz)
{
	int n = 0;
	for (int x = x0; x < x1 && n < outsz - 1; x++)
		out[n++] = ascii_of(scr[y][x] & 0xff);
	while (n > 0 && out[n - 1] == ' ')
		n--;
	out[n] = 0;
}
