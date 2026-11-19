/*
 * machboot
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The little C library the firmware needs: no libc on the target.
 */

#ifndef MB_LIB_H
#define MB_LIB_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

#ifdef MB_HOST
#include <string.h>
#else
void *memcpy(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
#endif

/* snprintf subset: %s %c %d %u %x %X, with optional '-', '0' and width */
int mb_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int mb_snprintf(char *buf, size_t size, const char *fmt, ...);

uint32_t mb_crc32(uint32_t crc, const void *data, size_t len);

/* ASCII case-insensitive compare; 0 if equal */
int strcmp_ci(const char *a, const char *b);

#endif
