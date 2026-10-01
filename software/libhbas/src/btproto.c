// SPDX-License-Identifier: MIT
#include "hbas/btproto.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Copy src into dst (cap bytes incl. NUL), cutting on a UTF-8 boundary. */
static void copy_utf8(char *dst, size_t cap, const char *src, size_t n)
{
	if (n >= cap) {
		n = cap - 1;
		while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80)
			n--;                    /* don't split a multi-byte character */
	}
	memcpy(dst, src, n);
	dst[n] = '\0';
}

int hbas_bt_parse(const char *line, struct hbas_bt_msg *m)
{
	const char *p = line;
	size_t n;

	memset(m, 0, sizeof(*m));
	while (*p == ' ')
		p++;
	n = strcspn(p, " \r\n");
	if (n == 0 || n >= sizeof(m->verb))
		return -1;
	memcpy(m->verb, p, n);
	p += n;
	for (;;) {
		char val[HBAS_BT_LINE_MAX];
		size_t vn = 0;
		const char *eq;

		while (*p == ' ')
			p++;
		if (!*p || *p == '\n' || *p == '\r')
			return 0;
		eq = p + strcspn(p, "= \r\n");
		if (m->nargs >= HBAS_BT_ARGS_MAX)
			return -1;
		if (*eq != '=') {
			/* bare word: "pairable on" -> key "" value "on" */
			n = strcspn(p, " \r\n");
			copy_utf8(m->arg[m->nargs].val, HBAS_BT_TEXT_MAX, p, n);
			m->nargs++;
			p += n;
			continue;
		}
		n = (size_t)(eq - p);
		if (n == 0 || n >= sizeof(m->arg[0].key))
			return -1;
		memcpy(m->arg[m->nargs].key, p, n);
		p = eq + 1;
		if (*p == '"') {
			for (p++; *p && *p != '"'; p++) {
				if (*p == '\\' && (p[1] == '"' || p[1] == '\\'))
					p++;
				if (vn < sizeof(val) - 1)
					val[vn++] = *p;
			}
			if (*p != '"')
				return -1;                  /* unterminated quote */
			p++;
		} else {
			n = strcspn(p, " \r\n");
			if (n >= sizeof(val))
				n = sizeof(val) - 1;
			memcpy(val, p, n);
			vn = n;
			p += n;
		}
		val[vn] = '\0';
		copy_utf8(m->arg[m->nargs].val, HBAS_BT_TEXT_MAX, val, vn);
		m->nargs++;
	}
}

const char *hbas_bt_get(const struct hbas_bt_msg *m, const char *key)
{
	for (unsigned i = 0; i < m->nargs; i++)
		if (!strcmp(m->arg[i].key, key))
			return m->arg[i].val;
	return NULL;
}

int hbas_bt_format(char *buf, size_t len, const char *verb, ...)
{
	va_list ap;
	size_t n;
	const char *k;

	if (snprintf(buf, len, "%s", verb) >= (int)len)
		return -1;
	n = strlen(buf);
	va_start(ap, verb);
	while ((k = va_arg(ap, const char *))) {
		const char *v = va_arg(ap, const char *);
		char cut[HBAS_BT_TEXT_MAX];
		bool quote;

		copy_utf8(cut, sizeof(cut), v ? v : "", strlen(v ? v : ""));
		quote = !cut[0] || strpbrk(cut, " \"\\\t\r\n") != NULL;
		if (n + strlen(k) + 3 >= len)
			goto too_long;
		n += (size_t)sprintf(buf + n, " %s=%s", k, quote ? "\"" : "");
		for (const char *c = cut; *c; c++) {
			if (n + 3 >= len)
				goto too_long;
			if (*c == '\r' || *c == '\n' || *c == '\t') {
				buf[n++] = ' ';         /* keep it one line */
				continue;
			}
			if (*c == '"' || *c == '\\')
				buf[n++] = '\\';
			buf[n++] = *c;
		}
		if (quote) {
			if (n + 2 >= len)
				goto too_long;
			buf[n++] = '"';
		}
		buf[n] = '\0';
	}
	va_end(ap);
	if (n + 2 > len)
		return -1;
	buf[n++] = '\n';
	buf[n] = '\0';
	return (int)n;
too_long:
	va_end(ap);
	return -1;
}

void hbas_bt_state_init(struct hbas_bt_state *s)
{
	memset(s, 0, sizeof(*s));
	s->status = HBAS_BT_STOPPED;
}

static uint32_t num(const struct hbas_bt_msg *m, const char *key, uint32_t dflt)
{
	const char *v = hbas_bt_get(m, key);
	char *end;
	unsigned long x;

	if (!v)
		return dflt;
	x = strtoul(v, &end, 10);
	return *end ? dflt : (uint32_t)x;
}

static bool flag(const struct hbas_bt_msg *m, const char *key, bool dflt)
{
	const char *v = hbas_bt_get(m, key);

	return v ? !strcmp(v, "1") : dflt;
}

static void text(char *dst, const struct hbas_bt_msg *m, const char *key)
{
	const char *v = hbas_bt_get(m, key);

	snprintf(dst, HBAS_BT_TEXT_MAX, "%s", v ? v : "");
}

bool hbas_bt_apply(struct hbas_bt_state *s, const struct hbas_bt_msg *m)
{
	if (!strcmp(m->verb, "bt")) {
		s->powered = flag(m, "powered", s->powered);
		s->pairable = flag(m, "pairable", s->pairable);
		s->agent = flag(m, "agent", s->agent);
		s->connected = flag(m, "connected", s->connected);
		s->player = flag(m, "player", s->connected && s->player);
		if (hbas_bt_get(m, "name"))
			text(s->device, m, "name");
		if (!s->connected) {
			s->player = false;
			s->title[0] = s->artist[0] = s->album[0] = '\0';
			s->duration_ms = s->position_ms = 0;
			s->status = HBAS_BT_STOPPED;
		}
	} else if (!strcmp(m->verb, "track")) {
		text(s->title, m, "title");
		text(s->artist, m, "artist");
		text(s->album, m, "album");
		s->duration_ms = num(m, "duration", 0);
	} else if (!strcmp(m->verb, "play")) {
		static const struct { const char *name; enum hbas_bt_status st; } map[] = {
			{ "playing", HBAS_BT_PLAYING }, { "paused", HBAS_BT_PAUSED },
			{ "stopped", HBAS_BT_STOPPED }, { "forward-seek", HBAS_BT_SEEKING },
			{ "reverse-seek", HBAS_BT_SEEKING }, { "error", HBAS_BT_ERROR },
		};
		const char *st = hbas_bt_get(m, "status");

		for (unsigned i = 0; st && i < sizeof(map) / sizeof(map[0]); i++)
			if (!strcmp(st, map[i].name))
				s->status = map[i].st;
		s->position_ms = num(m, "position", s->position_ms);
	} else if (!strcmp(m->verb, "pair")) {
		s->pair_request = true;
		text(s->pair_device, m, "device");
		s->pair_passkey = num(m, "passkey", 0);
		s->pair_result[0] = '\0';
	} else if (!strcmp(m->verb, "pair-end")) {
		const char *r = hbas_bt_get(m, "result");

		s->pair_request = false;
		snprintf(s->pair_result, sizeof(s->pair_result), "%s", r ? r : "failed");
	} else {
		return false;
	}
	s->changes++;
	return true;
}
