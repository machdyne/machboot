/*
 * machboot supervisor
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * What the PC sees on the carrier's USB-C port: the same as the
 * dirtyJTAG firmware the boards ship with, so openFPGALoader -c
 * dirtyJtag and the serial port work unchanged (docs/rp2040.md):
 *
 *   1209:C0CA
 *   interface 0    vendor: JTAG, bulk OUT 0x01, bulk IN 0x82
 *   interface 1+2  CDC-ACM: the module's UART
 */

#include "tusb.h"
#include "pico/unique_id.h"

enum { ITF_JTAG = 0, ITF_CDC, ITF_CDC_DATA, ITF_MSC, ITF_COUNT };

static const tusb_desc_device_t dev = {
	.bLength = sizeof(tusb_desc_device_t),
	.bDescriptorType = TUSB_DESC_DEVICE,
	.bcdUSB = 0x0200,
	.bDeviceClass = TUSB_CLASS_MISC,
	.bDeviceSubClass = MISC_SUBCLASS_COMMON,
	.bDeviceProtocol = MISC_PROTOCOL_IAD,
	.bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
	.idVendor = 0x1209,
	.idProduct = 0xC0CA,
	.bcdDevice = 0x0012,		/* the supervisor's release: lsusb -v shows it */
	.iManufacturer = 1,
	.iProduct = 2,
	.iSerialNumber = 3,
	.bNumConfigurations = 1,
};

const uint8_t *tud_descriptor_device_cb(void)
{
	return (const uint8_t *)&dev;
}

#define CFG_LEN (TUD_CONFIG_DESC_LEN + TUD_VENDOR_DESC_LEN + TUD_CDC_DESC_LEN + TUD_MSC_DESC_LEN)

static const uint8_t cfg[] = {
	TUD_CONFIG_DESCRIPTOR(1, ITF_COUNT, 0, CFG_LEN, 0, 500),
	TUD_VENDOR_DESCRIPTOR(ITF_JTAG, 4, 0x01, 0x82, 64),
	TUD_CDC_DESCRIPTOR(ITF_CDC, 5, 0x83, 8, 0x04, 0x84, 64),
	TUD_MSC_DESCRIPTOR(ITF_MSC, 6, 0x05, 0x85, 64),	/* the card reader (docs/xlink.md) */
};

const uint8_t *tud_descriptor_configuration_cb(uint8_t index)
{
	(void)index;
	return cfg;
}

static const char *const str[] = {
	0,
	"Machdyne",
	"machboot supervisor r12",
	0,			/* serial: the flash's unique ID */
	"JTAG",
	"UART",
	"SD card",
};

static uint16_t sbuf[33];

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
	(void)langid;
	char serial[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
	const char *s;
	uint32_t n;
	if (index == 0) {
		sbuf[1] = 0x0409;
		n = 1;
	} else {
		if (index == 3) {
			pico_get_unique_board_id_string(serial, sizeof(serial));
			s = serial;
		} else if (index < sizeof(str) / sizeof(str[0])) {
			s = str[index];
		} else {
			return 0;
		}
		for (n = 0; s[n] && n < 32; n++)
			sbuf[1 + n] = (uint8_t)s[n];
	}
	sbuf[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * n + 2));
	return sbuf;
}
