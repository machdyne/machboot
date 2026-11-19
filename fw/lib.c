/*
 * machboot
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 */

#include "lib.h"

#ifndef MB_HOST
__attribute__((used)) void *memcpy(void *d, const void *s, size_t n)
{
	uint8_t *dp = d;
	const uint8_t *sp = s;
	while (n--)
		*dp++ = *sp++;
	return d;
}

__attribute__((used)) void *memset(void *d, int c, size_t n)
{
	uint8_t *dp = d;
	while (n--)
		*dp++ = (uint8_t)c;
	return d;
}

__attribute__((used)) int memcmp(const void *a, const void *b, size_t n)
{
	const uint8_t *x = a, *y = b;
	for (; n; n--, x++, y++)
		if (*x != *y)
			return *x - *y;
	return 0;
}

size_t strlen(const char *s)
{
	size_t n = 0;
	while (s[n])
		n++;
	return n;
}
#endif

static void put(char *buf, size_t size, size_t *pos, char c)
{
	if (*pos + 1 < size)
		buf[*pos] = c;
	(*pos)++;
}

int mb_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
	size_t pos = 0;
	char tmp[12];

	for (; *fmt; fmt++) {
		if (*fmt != '%') {
			put(buf, size, &pos, *fmt);
			continue;
		}
		fmt++;
		int left = 0, zero = 0, width = 0;
		if (*fmt == '-') { left = 1; fmt++; }
		if (*fmt == '0') { zero = 1; fmt++; }
		while (*fmt >= '0' && *fmt <= '9')
			width = width * 10 + (*fmt++ - '0');

		const char *s = tmp;
		int len = 0;
		switch (*fmt) {
		case 's':
			s = va_arg(ap, const char *);
			if (!s)
				s = "";
			len = (int)strlen(s);
			break;
		case 'c':
			tmp[0] = (char)va_arg(ap, int);
			len = 1;
			break;
		case 'd':
		case 'u':
		case 'x':
		case 'X': {
			uint32_t v;
			int neg = 0;
			if (*fmt == 'd') {
				int32_t sv = va_arg(ap, int32_t);
				if (sv < 0) { neg = 1; v = (uint32_t)(-sv); } else v = (uint32_t)sv;
			} else {
				v = va_arg(ap, uint32_t);
			}
			unsigned base = (*fmt == 'x' || *fmt == 'X') ? 16 : 10;
			const char *dig = (*fmt == 'X') ? "0123456789ABCDEF" : "0123456789abcdef";
			char rev[12];
			int n = 0;
			do {
				rev[n++] = dig[v % base];
				v /= base;
			} while (v);
			if (neg)
				rev[n++] = '-';
			for (int i = 0; i < n; i++)
				tmp[i] = rev[n - 1 - i];
			len = n;
			break;
		}
		case '%':
			tmp[0] = '%';
			len = 1;
			break;
		default:
			tmp[0] = '?';
			len = 1;
			break;
		}
		int pad = width > len ? width - len : 0;
		if (!left)
			while (pad-- > 0)
				put(buf, size, &pos, zero ? '0' : ' ');
		for (int i = 0; i < len; i++)
			put(buf, size, &pos, s[i]);
		if (left)
			while (pad-- > 0)
				put(buf, size, &pos, ' ');
	}
	if (size)
		buf[pos < size ? pos : size - 1] = 0;
	return (int)pos;
}

int mb_snprintf(char *buf, size_t size, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = mb_vsnprintf(buf, size, fmt, ap);
	va_end(ap);
	return n;
}

uint32_t mb_crc32(uint32_t crc, const void *data, size_t len)
{
	const uint8_t *p = data;
	crc = ~crc;
	while (len--) {
		crc ^= *p++;
		for (int k = 0; k < 8; k++)
			crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
	}
	return ~crc;
}

int strcmp_ci(const char *a, const char *b)
{
	for (;; a++, b++) {
		int x = (unsigned char)*a, y = (unsigned char)*b;
		if (x >= 'a' && x <= 'z') x -= 32;
		if (y >= 'a' && y <= 'z') y -= 32;
		if (x != y || !x)
			return x - y;
	}
}
