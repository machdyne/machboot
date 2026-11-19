/*
 * machboot
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The Card reader page (docs/usb-device.md).
 * Part of the menu (fw/ui.c, fw/ui_int.h); on Lakritz and Konfekt an
 * overlay, loaded from flash when the page is shown (fw/ovl.h).
 */

#include "ui_int.h"

void page_card(void)
{
	static const keyhint_t k_off[] = { { "Enter", "Share" }, { "F5", "Read the card again" }, { G_LEFT, "Menu" } };
	static const keyhint_t k_on[] = { { "Enter", "Stop sharing" }, { G_LEFT, "Menu" } };
	static const keyhint_t k_none[] = { { "F5", "Read the card again" }, { G_LEFT, "Menu" } };
	char buf[64];

	page_items = 0;
	if (!sd.present) {
		text_at(5, "No SD card.", C_WHITE);
		text_at(7, "Put the card in and press F5.", C_GREY);
		draw_keys(k_none, 2);
		return;
	}
	if (!usbd.msc_shared) {
		static char inst[48];
		const char *labels[2], *rights[2];
		int n = 0;
		if (card_offer) {
			mb_snprintf(inst, sizeof(inst), "Install %s %s", card_offer->name, card_offer->ver);
			labels[n] = inst;
			rights[n++] = "new on the card";
		}
		labels[n] = "Share the card with the PC";
		rights[n++] = "USB drive";
		page_items = n;
		int y = draw_items(5, labels, rights, n, sel, 1);
		text_at(y++, "The PC sees the SD card as a USB drive and can", C_GREY);
		text_at(y++, "copy files to it and from it -- systems for", C_GREY);
		text_at(y++, "\\SYS\\GATEWARE too. machboot does not use the card", C_GREY);
		text_at(y++, "while it is shared.", C_GREY);
		y++;
		if (usbd.rp2040 == 1)
			text_at(y, "The RP2040 needs machboot-sv.uf2 (docs/rp2040.md).", C_YELLOW);
		else if (!usbd.configured)
			text_at(y, "Connect the USB-C port to a PC.", C_YELLOW);
		else
			text_at(y, "The USB-C port is connected.", C_GREY);
		draw_keys(k_off, 3);
		return;
	}
	scr_fill(CX - 1, 5, CW + 2, 1, C_BLACK, C_LGREEN, ' ');
	scr_put(CX, 5, "The SD card is on the PC as a USB drive.", C_BLACK, C_LGREEN);
	const char *labels[1] = { "Stop sharing" };
	page_items = 1;
	int y = draw_items(7, labels, 0, 1, sel, 1);
	text_at(y++, "Eject the drive on the PC first. Leaving this", C_GREY);
	text_at(y++, "page stops sharing too.", C_GREY);
	y++;
	/* the totals, and the rate over the last second or so while it moves */
	static uint32_t r_bytes, r_ms, r_rate;
	uint32_t total = usbd.msc_read + usbd.msc_written, now = ms_now();
	if (total < r_bytes || now - r_ms > 4000) {
		r_bytes = total;		/* shared anew, or idle: start over */
		r_ms = now;
		r_rate = 0;
	} else if (now - r_ms >= 1000) {
		r_rate = (total - r_bytes) / (now - r_ms);	/* bytes per ms = KB/s */
		r_bytes = total;
		r_ms = now;
	}
	mb_snprintf(buf, sizeof(buf), "Read %u.%u MB, written %u.%u MB",
	            usbd.msc_read >> 20, (usbd.msc_read >> 10) % 1024 * 10 / 1024,
	            usbd.msc_written >> 20, (usbd.msc_written >> 10) % 1024 * 10 / 1024);
	text_at(y++, buf, C_WHITE);
	if (r_rate) {
		mb_snprintf(buf, sizeof(buf), "%u.%02u MB/s", r_rate / 1000, r_rate % 1000 / 10);
		text_at(y, buf, C_LGREEN);
	}
	draw_keys(k_on, 2);
}

