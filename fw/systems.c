/*
 * machboot
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The list of systems: systems.h.
 */

#include <stdint.h>
#include <string.h>
#include "systems.h"
#include "mb.h"
#include "lib.h"
#include "console.h"
#include "settings.h"
#include "install.h"

sysent_t sys_list[SYS_MAX];
int sys_n;
const char *systems_phase = "";
static int plan_lost;
#ifndef MB_SLOT
rc_list_t sys_rc;
static int rc_ok;
#endif

int systems_plan_lost(void)
{
	return plan_lost;
}

int systems_on(void)
{
#ifdef MB_SLOT
	return !mb.install_mode && mb.fw_base != 0;	/* machboot installed: the slot is ours */
#else
	return rc_available();
#endif
}

void systems_clear(void)
{
	sys_n = 0;
}

static void copy_str(char *d, const char *s, uint32_t n, uint32_t cap)
{
	if (n > cap - 1)
		n = cap - 1;
	memcpy(d, s, n);
	d[n] = 0;
}

#ifndef MB_SLOT
static uint32_t hexval(const char *s, uint32_t n)
{
	uint32_t v = 0;
	for (uint32_t i = 0; i < n; i++) {
		char c = s[i];
		v = v << 4 | (uint32_t)(c >= 'a' ? c - 'a' + 10 : c >= 'A' ? c - 'A' + 10 : c - '0');
	}
	return v;
}

static uint32_t decval(const char *s, uint32_t n)
{
	uint32_t v = 0;
	for (uint32_t i = 0; i < n && s[i] >= '0' && s[i] <= '9'; i++)
		v = v * 10 + (uint32_t)(s[i] - '0');
	return v;
}

/* "sys=c|<crc hex>|<len>|<name>|<ver>", "sys=m|0|0|<name>|<ver>" */
void systems_line(const char *line, uint32_t len)
{
	const char *f[5];
	uint32_t fl[5];
	int nf = 0;
	if (len < 5 || memcmp(line, "sys=", 4) || sys_n >= SYS_MAX)
		return;
	const char *p = line + 4, *end = line + len;
	while (nf < 5) {
		const char *q = p;
		while (q < end && *q != '|')
			q++;
		f[nf] = p;
		fl[nf++] = (uint32_t)(q - p);
		if (q >= end)
			break;
		p = q + 1;
	}
	if (nf != 5 || fl[0] != 1 || !fl[3])
		return;
	sysent_t *e = &sys_list[sys_n];
	memset(e, 0, sizeof(*e));
	e->module = f[0][0] == 'm';
	e->crc = hexval(f[1], fl[1]);
	e->len = decval(f[2], fl[2]);
	copy_str(e->name, f[3], fl[3], sizeof(e->name));
	copy_str(e->ver, f[4], fl[4], sizeof(e->ver));
	if (!e->module && !e->crc)
		return;
	sys_n++;
}

int systems_text(char *b, int cap)
{
	int n = 0;
	for (int i = 0; i < sys_n; i++) {
		const sysent_t *e = &sys_list[i];
		int k = mb_snprintf(b + n, (size_t)(cap - n), "sys=%s|%x|%u|%s|%s\n",
		                    e->module ? "m" : "c", e->module ? 0 : e->crc, e->module ? 0 : e->len, e->name, e->ver);
		if (k < 0 || n + k >= cap)
			break;
		n += k;
	}
	return n;
}

#endif

sysent_t *systems_find(const char *name)
{
	for (int i = 0; i < sys_n; i++)
#ifdef MB_SLOT
		if (!strcmp_ci(sys_list[i].name, name))	/* one entry per name */
#else
		if (!sys_list[i].module && !strcmp_ci(sys_list[i].name, name))	/* the module's may share a name */
#endif
			return &sys_list[i];
	return 0;
}

#ifndef MB_SLOT
static const rc_item_t *rc_find(uint32_t crc)
{
	for (int i = 0; i < sys_rc.n; i++)
		if (sys_rc.it[i].crc == crc)
			return &sys_rc.it[i];
	return 0;
}

static int in_list(uint32_t crc)
{
	for (int i = 0; i < sys_n; i++)
		if (!sys_list[i].module && sys_list[i].crc == crc)
			return 1;
	return 0;
}

#endif

static void insert(int at, const sysent_t *e)
{
	if (sys_n >= SYS_MAX)
		return;
	for (int i = sys_n; i > at; i--)
		sys_list[i] = sys_list[i - 1];
	sys_list[at] = *e;
	sys_n++;
}

static void drop_at(int i)
{
	for (; i < sys_n - 1; i++)
		sys_list[i] = sys_list[i + 1];
	sys_n--;
}

static int on_card(const char *name)
{
	card_entry_t *ce = card_system(name);
	return ce && ce->kind == CE_SYSTEM && ce->has_bit;
}

#ifndef MB_SLOT
void systems_refresh(void)
{
	int changed = 0;
	rc_ok = rc_list(&sys_rc) == XL_R_OK;
	if (!rc_ok)
		sys_rc.n = 0;

	/* the module's own system: first, the first time */
	int mi = -1;
	for (int i = 0; i < sys_n; i++)
		if (sys_list[i].module)
			mi = i;
	if (mb.sys_ok) {
		sysent_t m;
		memset(&m, 0, sizeof(m));
		m.module = 1;
		copy_str(m.name, mb.sysname, (uint32_t)strlen(mb.sysname), sizeof(m.name));
		copy_str(m.ver, cfg.sys_ver, (uint32_t)strlen(cfg.sys_ver), sizeof(m.ver));
		if (mi < 0) {
			insert(0, &m);
			changed = 1;
		} else if (memcmp(sys_list[mi].name, m.name, sizeof(m.name))) {
			memcpy(sys_list[mi].name, m.name, sizeof(m.name));
			changed = 1;
		}
	} else if (mi >= 0) {
		drop_at(mi);
		changed = 1;
	}

	/* what the cache has that the list does not know (settings lost, or
	   kept from another machboot): at the bottom */
	if (rc_ok)
		for (int i = 0; i < sys_rc.n && sys_n < SYS_MAX; i++)
			if (!in_list(sys_rc.it[i].crc)) {
				sysent_t e;
				memset(&e, 0, sizeof(e));
				memcpy(e.name, sys_rc.it[i].name, sizeof(e.name));
				memcpy(e.ver, sys_rc.it[i].ver, sizeof(e.ver));
				e.crc = sys_rc.it[i].crc;
				e.len = sys_rc.it[i].len;
				insert(sys_n, &e);
				changed = 1;
			}

	for (int i = 0; i < sys_n; i++) {
		sysent_t *e = &sys_list[i];
		e->cached = !e->module && rc_find(e->crc) != 0;
		e->ce = 0;
		if (!e->module) {
			card_entry_t *ce = card_system(e->name);
			if (ce && ce->kind == CE_SYSTEM && ce->has_bit)
				e->ce = ce;
		}
	}
	if (changed)
		settings_save();
}

#endif

int systems_move(int i, int to)
{
	if (i < 0 || i >= sys_n || to < 0 || to >= sys_n || i == to)
		return i;
	sysent_t e = sys_list[i];
	drop_at(i);
	insert(to, &e);
	settings_save();
	return to;
}

int systems_ready(const sysent_t *e)
{
	return e->module || e->cached || e->ce;
}

int systems_needs_copy(const sysent_t *e)
{
	return !e->module && !e->cached && e->ce;
}

#ifndef MB_SLOT
/* -- making room ---------------------------------------------------------- */

static int fits(uint32_t need, const int *gone)
{
	uint32_t pos = 0;
	for (int i = 0; i <= sys_rc.n; i++) {
		if (i < sys_rc.n && gone[i])
			continue;
		uint32_t end = i < sys_rc.n ? sys_rc.it[i].off : sys_rc.size;
		if (end >= pos && end - pos >= need)
			return 1;
		if (i < sys_rc.n)
			pos = sys_rc.it[i].off + RC_EXTENT(sys_rc.it[i].len);
	}
	return 0;
}

static int rc_index(uint32_t crc)
{
	for (int i = 0; i < sys_rc.n; i++)
		if (sys_rc.it[i].crc == crc)
			return i;
	return -1;
}

/* gone[]: which cache items go. self: the old copy of the one being
   copied (it goes first, if anything must) */
static int plan(uint32_t len, uint32_t self, int *gone)
{
	uint32_t need = RC_EXTENT(len);
	memset(gone, 0, sizeof(int) * RC_MAX);
	if (!rc_ok || need > sys_rc.size)
		return -1;
	if (fits(need, gone))
		return 0;
	int k = self ? rc_index(self) : -1;
	if (k >= 0) {
		gone[k] = 1;
		if (fits(need, gone))
			return 0;
	}
	for (int i = 0; i < sys_rc.n; i++)
		if (!in_list(sys_rc.it[i].crc)) {
			gone[i] = 1;
			if (fits(need, gone))
				return 0;
		}
	for (int j = sys_n - 1; j >= 0; j--) {
		if (sys_list[j].module || (k = rc_index(sys_list[j].crc)) < 0 || gone[k])
			continue;
		gone[k] = 1;
		if (fits(need, gone))
			return 0;
	}
	return -1;
}

int systems_plan(uint32_t len, uint32_t self, char *out, int cap)
{
	int gone[RC_MAX], n = 0, pos = 0;
	out[0] = 0;
	if (plan(len, self, gone))
		return -1;
	plan_lost = 0;
	for (int i = 0; i < sys_rc.n; i++) {
		if (!gone[i] || sys_rc.it[i].crc == self)
			continue;
		if (!on_card(sys_rc.it[i].name))
			plan_lost = 1;
		int k = mb_snprintf(out + pos, (size_t)(cap - pos), "%s%s", n ? ", " : "", sys_rc.it[i].name);
		if (k > 0 && pos + k < cap)
			pos += k;
		n++;
	}
	return n;
}

uint32_t systems_copy_s(uint32_t len)
{
	return (len + RC_RATE - 1) / RC_RATE;
}

int systems_copy(sysent_t *e, int (*progress)(uint32_t, uint32_t))
{
	int gone[RC_MAX];
	card_entry_t *ce = e->ce;
	if (!ce)
		return RC_READ;
	rc_ok = rc_list(&sys_rc) == XL_R_OK;
	if (!rc_ok)
		return XL_R_NOLINK;
	if (plan(ce->bit.size, e->crc, gone))
		return XL_R_NOROOM;
	/* the crcs first: dropping one rescans the cache's list */
	uint32_t drop[RC_MAX];
	int nd = 0;
	for (int i = 0; i < sys_rc.n; i++)
		if (gone[i])
			drop[nd++] = sys_rc.it[i].crc;
	for (int i = 0; i < nd; i++) {
		con_printf("systems: making room: %08x out of the RP2040's flash\n", drop[i]);
		int r = rc_drop(drop[i]);
		if (r != XL_R_OK && r != XL_R_NOTFOUND)
			return r;
	}
	uint32_t crc = 0, len = 0, old = e->crc;
	int r = rc_put(ce, &crc, &len, progress);
	if (r == XL_R_OK) {
		e->crc = crc;
		e->len = len;
		copy_str(e->ver, ce->ver, (uint32_t)strlen(ce->ver), sizeof(e->ver));
		copy_str(e->name, ce->name, (uint32_t)strlen(ce->name), sizeof(e->name));
		if (old && old != crc)
			rc_drop(old);		/* the version before: one copy each */
		settings_save();
		con_printf("systems: %s %s is in the RP2040's flash (%u bytes, CRC %08x)\n", e->name, e->ver, len, crc);
	}
	/* sys_list may hold e: refresh keeps it in place */
	systems_refresh();
	return r;
}

int systems_add(card_entry_t *ce, int (*progress)(uint32_t, uint32_t))
{
	sysent_t *e = systems_find(ce->name);
	int added = 0;
	if (!e) {
		if (sys_n >= SYS_MAX)
			return XL_R_NOROOM;
		sysent_t n;
		memset(&n, 0, sizeof(n));
		copy_str(n.name, ce->name, (uint32_t)strlen(ce->name), sizeof(n.name));
		copy_str(n.ver, ce->ver, (uint32_t)strlen(ce->ver), sizeof(n.ver));
		n.ce = ce;
		insert(sys_n, &n);
		e = &sys_list[sys_n - 1];
		added = 1;
	} else {
		e->ce = ce;
	}
	int r = systems_copy(e, progress);
	if (r != XL_R_OK && added) {
		/* not copied: not in the list either */
		e = systems_find(ce->name);
		if (e && !e->crc) {
			drop_at((int)(e - sys_list));
			settings_save();
		}
	}
	return r;
}

int systems_remove(int i)
{
	if (i < 0 || i >= sys_n || sys_list[i].module)
		return XL_R_ERROR;
	int r = XL_R_OK;
	if (sys_list[i].cached)
		r = rc_drop(sys_list[i].crc);
	if (r != XL_R_OK && r != XL_R_NOTFOUND)
		return r;
	con_printf("systems: %s removed\n", sys_list[i].name);
	drop_at(i);
	settings_save();
	systems_refresh();
	return XL_R_OK;
}

#endif

const char *rc_error(int r)
{
	switch (r) {
	case XL_R_OK: return "done";
	case XL_R_NOROOM: return "there is not enough room in flash";
	case XL_R_NOTFOUND: return "it is not in flash";
	case XL_R_CANCELLED: return "cancelled";
	case XL_R_NOLINK: return "the RP2040 did not answer";
	case RC_READ: return "the card could not be read";
	case RC_NOT_BIT: return "it is not an ECP5 bitstream";
	case RC_WRONG_FPGA: return "it is built for another FPGA";
	case RC_TOO_BIG: return "it is too big for the system slot";
	default: return "the flash could not be written";
	}
}

#ifdef MB_SLOT
/* -- Lakritz, Konfekt: one slot ------------------------------------------- */

/* the name of what is in the slot, if anything valid is */
static const char *slot_name(void)
{
	return mb.sys_ok ? mb.sysname : 0;
}

void systems_refresh(void)
{
	const char *sn = slot_name();
	int found = 0;
	for (int i = 0; i < sys_n; i++) {
		sysent_t *e = &sys_list[i];
		e->module = sn && !strcmp_ci(e->name, sn);
		e->cached = 0;
		e->ce = 0;
		card_entry_t *ce = card_system(e->name);
		if (ce && ce->kind == CE_SYSTEM && ce->has_bit)
			e->ce = ce;
		if (e->module) {
			found = 1;
			copy_str(e->ver, cfg.sys_ver, (uint32_t)strlen(cfg.sys_ver), sizeof(e->ver));
		} else if (e->ce) {
			copy_str(e->ver, e->ce->ver, (uint32_t)strlen(e->ce->ver), sizeof(e->ver));
		} else {
			e->ver[0] = 0;
		}
	}
	if (sn && !found && sys_n < SYS_MAX) {
		/* what is in the slot (installed before the list existed): first */
		sysent_t m;
		memset(&m, 0, sizeof(m));
		m.module = 1;
		copy_str(m.name, sn, (uint32_t)strlen(sn), sizeof(m.name));
		copy_str(m.ver, cfg.sys_ver, (uint32_t)strlen(cfg.sys_ver), sizeof(m.ver));
		card_entry_t *ce = card_system(m.name);
		if (ce && ce->kind == CE_SYSTEM && ce->has_bit)
			m.ce = ce;
		insert(0, &m);
		settings_save();
	}
}

int systems_names(char *b, int cap)
{
	int n = 0;
	b[0] = 0;
	for (int i = 0; i < sys_n; i++) {
		int k = mb_snprintf(b + n, (size_t)(cap - n), "%s%s", i ? "|" : "", sys_list[i].name);
		if (k < 0 || n + k >= cap)
			break;
		n += k;
	}
	return n;
}

void systems_from_names(const char *s)
{
	sys_n = 0;
	while (*s && sys_n < SYS_MAX) {
		const char *q = s;
		while (*q && *q != '|')
			q++;
		if (q > s) {
			sysent_t *e = &sys_list[sys_n++];
			memset(e, 0, sizeof(*e));
			copy_str(e->name, s, (uint32_t)(q - s), sizeof(e->name));
		}
		s = *q ? q + 1 : q;
	}
}

/* what copying into the slot pushes out: the one there, if another */
int systems_plan(uint32_t len, uint32_t self, char *out, int cap)
{
	(void)self;
	out[0] = 0;
	plan_lost = 0;
	if (len > ZEITLOS_REGION - SYSTEM_SLOT)
		return -1;
	const char *sn = slot_name();
	if (!sn)
		return 0;
	mb_snprintf(out, (size_t)cap, "%s", sn);
	plan_lost = !on_card(sn);
	return 1;
}

/* the slot: compared at the card's speed, then written (about 60 KB/s) */
uint32_t systems_copy_s(uint32_t len)
{
	return (len + 59999u) / 60000u;
}

static int (*slot_cb)(uint32_t, uint32_t);

static void slot_progress(int pm, const char *what)
{
	systems_phase = what;
	if (slot_cb && slot_cb((uint32_t)pm, 1000) && pm < 300)
		install_cancel = 1;	/* only while checking: nothing written yet */
}

static int from_install(int r)
{
	switch (r) {
	case INST_OK: case INST_SAME: return XL_R_OK;
	case INST_CANCELLED: return XL_R_CANCELLED;
	case INST_READ: return RC_READ;
	case INST_NOT_BIT: return RC_NOT_BIT;
	case INST_WRONG_FPGA: return RC_WRONG_FPGA;
	case INST_TOO_BIG: return RC_TOO_BIG;
	default: return XL_R_ERROR;
	}
}

int systems_copy(sysent_t *e, int (*progress)(uint32_t, uint32_t))
{
	card_entry_t *ce = e->ce;
	if (!ce)
		return RC_READ;
	slot_cb = progress;
	systems_phase = "Checking";
	int r = install(ce, slot_progress);	/* records name, version, CRC */
	slot_cb = 0;
	con_printf("systems: %s %s into the slot: %s\n", ce->name, ce->ver, install_error(r));
	mb_probe_system();
	systems_refresh();
	return from_install(r);
}

int systems_slot_writes(card_entry_t *ce)
{
	const char *sn = slot_name();
	return !sn || !strcmp_ci(sn, ce->name);
}

int systems_add(card_entry_t *ce, int (*progress)(uint32_t, uint32_t))
{
	sysent_t *e = systems_find(ce->name);
	if (!e) {
		if (sys_n >= SYS_MAX)
			return XL_R_NOROOM;
		sysent_t n;
		memset(&n, 0, sizeof(n));
		copy_str(n.name, ce->name, (uint32_t)strlen(ce->name), sizeof(n.name));
		copy_str(n.ver, ce->ver, (uint32_t)strlen(ce->ver), sizeof(n.ver));
		n.ce = ce;
		insert(sys_n, &n);
		settings_save();
		e = &sys_list[sys_n - 1];
	}
	e->ce = ce;
	/* the slot is written only for an update of what is in it, or an empty slot */
	if (systems_slot_writes(ce))
		return systems_copy(e, progress);
	systems_refresh();
	return XL_R_OK;
}

int systems_remove(int i)
{
	if (i < 0 || i >= sys_n || sys_list[i].module)
		return XL_R_ERROR;
	con_printf("systems: %s removed from the list\n", sys_list[i].name);
	drop_at(i);
	settings_save();
	systems_refresh();
	return XL_R_OK;
}
#endif
