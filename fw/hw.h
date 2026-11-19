/*
 * machboot
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Hardware access. Everything the firmware touches goes through these
 * inline functions, so the same firmware also builds for a PC
 * (MB_HOST, fw/host/), where they are backed by arrays and a script of
 * key presses -- that is how tests/ renders and checks every screen.
 *
 * Register map: docs/memory-map.md.
 */

#ifndef MB_HW_H
#define MB_HW_H

#include <stdint.h>

#define MB_HWVER	2		/* rtl/mb_sysctl.v HWVER this firmware knows */

/* text cells */
#define TEXT_COLS	80
#define TEXT_ROWS	30

/* system registers (offsets) */
#define SYS_MAGIC	0x00
#define SYS_HWVER	0x04
#define SYS_MS		0x08
#define SYS_CYCLES	0x0C
#define SYS_LED		0x10
#define SYS_RECONFIG	0x14
#define SYS_BOARD	0x18
#define SYS_FEATURES	0x1C
#define SYS_IDCODE	0x20
#define SYS_FRAMES	0x24
#define SYS_SLOT	0x28

#define SYS_MAGIC_VAL	0x4D424F54u	/* "MBOT" */
#define RECONFIG_KEY	0x5A524254u	/* "ZRBT" */

#define FEAT_VIDEO	0x01
#define FEAT_USBH	0x02
#define FEAT_USBD	0x04
#define FEAT_SD		0x08
#define FEAT_RPLINK	0x10
#define FEAT_PROGN	0x20
#define FEAT_INSTALLER	0x40		/* installer build: docs/install-mode.md */

/* SPI flash controller registers (rtl/zeitlos/spiflash.v) */
#define FL_MAGIC	0x00
#define FL_STATUS	0x04
#define FL_ID		0x08
#define FL_LOCK		0x0C
#define FL_LOCK_FROM	0x18		/* read: locked from here up too (machboot); a write is CMD */
#define FL_ADDR		0x10
#define FL_ARM		0x14
#define FL_CMD		0x18
#define FL_LEN		0x1C
#define FL_PBUF		0x100		/* 256-byte page buffer, 64 words, write-only */
#define FL_ARM_KEY	0x5A465752u	/* "ZFWR" */
#define FL_ST_BUSY	0x01
#define FL_ST_DONE	0x02
#define FL_ST_REFUSED	0x3C		/* locked, not armed, bad length, busy */

/* SD card SPI master (rtl/zeitlos/spim.v), word registers */
/* USB device (rtl/mb_usbd.v, docs/usb-device.md) */
#define UD_MAGIC	0x000
#define UD_CTRL		0x004		/* bit 0 attach, bits 14:8 address */
#define UD_EVENTS	0x008		/* bit 0 bus reset; write 1 to clear */
#define UD_FRAME	0x00C
#define UD_EPTYPE	0x010		/* 3:0 IN bulk, 7:4 OUT bulk, 11:8 IN interrupt */
#define UD_TOGGLE	0x014		/* 3:0 IN, 7:4 OUT: reset to DATA0 */
#define UD_OUT(n)	(0x040 + 16 * (n))	/* r: 0 full, 1 setup, 14:8 len; w: empty it */
#define UD_IN(n)	(0x044 + 16 * (n))	/* r: bit 8 armed; w: len | 0x100 arms */
#define UD_STALL(n)	(0x048 + 16 * (n))
#define UD_OBUF(n)	(0x400 + 0x80 * (n))
#define UD_IBUF(n)	(0x440 + 0x80 * (n))
#define UD_MAGIC_VAL	0x44425355u	/* "USBD" */

#define SD_DATA		0x00
#define SD_STATUS	0x04
#define SD_CTRL		0x08
#define SD_MAGIC	0x0C
#define SD_CTRL_CS	0x01
#define SD_CTRL_X32	0x02
#define SD_MAGIC_VAL	0x53504931u	/* "SPI1" */

/* UART (16550 layout, word stride) */
#define UART_DATA	0x00
#define UART_IER	0x04
#define UART_FCR	0x08
#define UART_LCR	0x0C
#define UART_LSR	0x14

#ifndef MB_HOST

#define REG32(a)	(*(volatile uint32_t *)(a))
#define REG8(a)		(*(volatile uint8_t *)(a))

static inline uint32_t sys_rd(uint32_t off) { return REG32(0x30000000u + off); }
static inline void sys_wr(uint32_t off, uint32_t v) { REG32(0x30000000u + off) = v; }

static inline void text_wr(int idx, uint16_t cell) { REG32(0x20000000u + ((uint32_t)idx << 2)) = cell; }

static inline uint8_t flash_rd8(uint32_t addr) { return REG8(0x10000000u + addr); }
static inline uint32_t flash_rd32(uint32_t addr) { return REG32(0x10000000u + addr); }
static inline uint32_t flash_reg(uint32_t off) { return REG32(0x1F000000u + off); }

static inline void flash_reg_wr(uint32_t off, uint32_t v) { REG32(0x1F000000u + off) = v; }

static inline uint8_t uart_reg_rd(uint32_t off) { return REG8(0xF0000000u + off); }
static inline void uart_reg_wr(uint32_t off, uint8_t v) { REG8(0xF0000000u + off) = v; }

/* USB host: Zeitlos's register map, at 0x4000_0000 (fw/usb.c) */
static inline uint32_t usbh_rd(uint32_t off) { return REG32(0x40000000u + off); }
static inline void usbh_wr(uint32_t off, uint32_t v) { REG32(0x40000000u + off) = v; }
static inline uint8_t usbh_rb(uint32_t off) { return REG8(0x40000000u + off); }
static inline void usbh_wb(uint32_t off, uint8_t v) { REG8(0x40000000u + off) = v; }

static inline uint32_t sd_rd(uint32_t off) { return REG32(0x50000000u + off); }
static inline void sd_wr(uint32_t off, uint32_t v) { REG32(0x50000000u + off) = v; }

static inline uint32_t usbd_rd(uint32_t off) { return REG32(0x60000000u + off); }
static inline void usbd_wr(uint32_t off, uint32_t v) { REG32(0x60000000u + off) = v; }

/* the XA-XD link to the RP2040 (rtl/mb_xlink.v), Sergei and Mozart */
static inline uint32_t xl_rd(uint32_t off) { return REG32(0x70000000u + off); }
static inline void xl_wr(uint32_t off, uint32_t v) { REG32(0x70000000u + off) = v; }

#else /* MB_HOST: fw/host/hw_host.c */

uint32_t sys_rd(uint32_t off);
void sys_wr(uint32_t off, uint32_t v);
void text_wr(int idx, uint16_t cell);
uint8_t flash_rd8(uint32_t addr);
uint32_t flash_rd32(uint32_t addr);
uint32_t flash_reg(uint32_t off);
void flash_reg_wr(uint32_t off, uint32_t v);
uint8_t uart_reg_rd(uint32_t off);
void uart_reg_wr(uint32_t off, uint8_t v);
uint32_t usbd_rd(uint32_t off);
void usbd_wr(uint32_t off, uint32_t v);
uint32_t xl_rd(uint32_t off);		/* fw/host/xlink_host.c */
void xl_wr(uint32_t off, uint32_t v);
uint32_t usbh_rd(uint32_t off);
void usbh_wr(uint32_t off, uint32_t v);
uint8_t usbh_rb(uint32_t off);
void usbh_wb(uint32_t off, uint8_t v);
uint32_t sd_rd(uint32_t off);
void sd_wr(uint32_t off, uint32_t v);

#endif

#endif
