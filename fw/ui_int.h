/*
 * machboot
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * What the menu's pages share (fw/ui.c, fw/page_*.c). Each page is its
 * own file so that Lakritz and Konfekt can keep it in flash and load it
 * when it is shown (fw/ovl.h, docs/overlays.md).
 */

#ifndef MB_UI_INT_H
#define MB_UI_INT_H

#include "mb.h"
#include "hw.h"
#include "lib.h"
#include "screen.h"
#include "console.h"
#include "settings.h"
#include "card.h"
#include "install.h"
#include "usb.h"
#include "sd.h"
#include "selfinst.h"
#include "usbd.h"
#include "ovl.h"

#define SB	22		/* sidebar width */
#define CX	(SB + 3)	/* content left */
#define CW	(80 - CX - 3)	/* content width */

enum { S_START, S_UPDATE, S_CARD, S_SETTINGS, S_RUN, S_DIAG, S_BOARD, S_COUNT };

typedef struct {
	const char *name;
	const char *title;
	char letter;
	int avail;
} section_t;

typedef struct {
	const char *key;
	const char *label;
} keyhint_t;

extern section_t sections[S_COUNT];
extern int cur;			/* section */
extern int focus_side;		/* 1: the sidebar has the keys */
extern int sel;			/* selected item on the page */
extern int details;		/* F1 */
extern int page_items;		/* how many selectable items the page has */

void draw_keys(const keyhint_t *k, int n);
int draw_items(int y, const char *const *labels, const char *const *rights, int n, int selected, int gap);
void text_at(int y, const char *s, int fg);
void kv_at(int y, const char *k, const char *v);
void mirror(int with_sidebar);
void redraw(void);
void rescan(void);

/* installing into the system slot (fw/install.c) */
card_entry_t *card_copy_of_installed(void);
int can_install(void);
void do_install(card_entry_t *ce);
int update_list(card_entry_t **out, int max);
extern int inst_running, inst_permille, inst_msg_ok;
extern const char *inst_what;
extern char inst_msg[64];
extern char page_lab[CARD_MAX][44];	/* a page's list labels */

/* the card reader */
card_entry_t *newer_on_card(void);
extern card_entry_t *card_offer;	/* after the PC had the card: what is new on it */
void card_unshare(void);

/* install mode */
enum { IM_OFFER, IM_CONFIRM, IM_RUNNING, IM_DONE, IM_FAILED };
extern int im_phase;

/* the pages: fw/page_*.c */
void page_start(void);
void page_update(void);
void page_card(void);
void page_settings(void);
int settings_key(int k);
void settings_leave(void);
void page_diag(void);
int diag_key(int k);
void page_board(void);
void page_install_mode(void);
int install_mode_key(int k);

/* the startup screen, and starting the system in the slot or module (fw/ui.c) */
void draw_startup(const char *msg, int msg_fg, int permille, int show_bar);
void boot_module(void);

#ifdef MB_SYSTEMS
/* the list of systems (fw/ui_systems.c, fw/page_systems.c, fw/page_sysinst.c) */
#include "systems.h"
extern char sys_msg[80];
extern int sys_msg_ok, rm_ask;
extern int startup_copying;	/* 1: copying, Esc cancels; 2: writing, it cannot stop */
int multi(void);
const char *sys_where(const sysent_t *e);
const char *src_name(const card_entry_t *ce);
uint32_t about_s(uint32_t len);
int copy_hint(int y, const char *verb_line, card_entry_t *ce, uint32_t self);
int do_copy(sysent_t *e, card_entry_t *ce, int inline_);
void start_entry(int i);
int systems_countdown(char *msg, int cap, int left);
void page_systems(void);
void page_install(void);
int systems_start_key(int k);
int systems_install_key(int k);
#endif

#endif
