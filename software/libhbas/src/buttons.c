// SPDX-License-Identifier: MIT
/* Tables transcribed from stock iocInterface; addresses in CROSS_CHECKS sec. 13. */
#include "hbas/buttons.h"

#include <string.h>

struct bit_key { uint8_t byte, mask, code; };

/* 0x104C0C */
static const struct bit_key right_a[] = {
	{1, 0x01, 'X'}, {1, 0x08, '\\'}, {1, 0x10, ','}, {1, 0x20, '.'},
	{2, 0x01, 'G'}, {3, 0x01, 'U'}, {3, 0x02, 'H'}, {3, 0x04, 13},
	{3, 0x08, 'K'}, {3, 0x10, 'J'}, {4, 0x01, 'T'}, {4, 0x04, 'R'},
	{4, 0x10, 'M'},
};
/* 0x104B84 (unused by stock) */
static const struct bit_key right_b[] = {
	{0, 0x10, 'U'}, {0, 0x20, 'J'}, {2, 0x10, 'H'}, {1, 0x02, 'K'}, {0, 0x40, 13},
};
/* 0x104D34 */
static const struct bit_key left_a[] = {
	{1, 0x01, '+'}, {1, 0x08, ';'}, {1, 0x10, '\''}, {1, 0x20, '`'},
	{2, 0x01, 'F'}, {3, 0x01, 'W'}, {3, 0x02, 'A'}, {3, 0x04, ' '},
	{3, 0x08, 'D'}, {3, 0x10, 'S'}, {4, 0x01, '/'}, {4, 0x04, 'Z'},
	{4, 0x10, 'L'}, {5, 0x01, 'V'},
};
/* 0x104E70 (unused by stock) */
static const struct bit_key left_b[] = {
	{0, 0x80, 'D'}, {1, 0x04, 'W'}, {1, 0x08, ' '}, {1, 0x10, 'S'},
	{1, 0x20, 'T'}, {1, 0x40, 'R'}, {1, 0x80, 'M'}, {2, 0x10, 'A'},
	{2, 0x80, 'V'},
};
/* table at 0x108768: key = front[byte * 8 + bit], 0 = no key */
static const uint8_t front[32] = {
	'O', 'I', 'C', 'N', 0, 0, 0, 0,
	'1', '2', '3', '4', '5', '6', '7', '8',
	'9', '0', '-', ']', 'Q', 'E', 0, 0,
	'Y', 'U', 'I', 'O', 'P', '[', 0, 0,
};

void hbas_buttons_init(struct hbas_buttons *b, enum hbas_handlebar_layout layout)
{
	memset(b, 0, sizeof(*b));
	b->layout = layout;
}

static size_t emit(struct hbas_key_event *out, size_t n, size_t max, uint8_t code, bool pressed)
{
	if (n < max) {
		out[n].code = code;
		out[n].pressed = pressed;
		return n + 1;
	}
	return n;
}

static size_t decode_table(const struct bit_key *t, size_t nt, const uint8_t *p, size_t plen,
			   uint8_t *prev, size_t prevlen, struct hbas_key_event *out, size_t max)
{
	uint8_t cur[8] = { 0 };
	size_t n = 0;

	memcpy(cur, p, plen < prevlen ? plen : prevlen);
	for (size_t i = 0; i < nt; i++) {
		uint8_t now = cur[t[i].byte] & t[i].mask, was = prev[t[i].byte] & t[i].mask;

		if (now != was)
			n = emit(out, n, max, t[i].code, now != 0);
	}
	memcpy(prev, cur, prevlen);
	return n;
}

size_t hbas_buttons_decode(struct hbas_buttons *b, const uint8_t *msg, size_t len,
			   struct hbas_key_event *out, size_t max)
{
	uint16_t id;
	const uint8_t *p;
	size_t plen;

	if (len < 3)
		return 0;
	id = (uint16_t)(msg[1] << 8 | msg[0]);
	p = msg + 3;
	plen = len - 3;
	if (msg[2] < plen)
		plen = msg[2];

	switch (id) {
	case HBAS_MSG_RIGHT_HANDLEBAR:
		return b->layout == HBAS_LAYOUT_A
			? decode_table(right_a, sizeof(right_a) / sizeof(right_a[0]), p, plen,
				       b->prev_right, sizeof(b->prev_right), out, max)
			: decode_table(right_b, sizeof(right_b) / sizeof(right_b[0]), p, plen,
				       b->prev_right, sizeof(b->prev_right), out, max);
	case HBAS_MSG_LEFT_HANDLEBAR:
		return b->layout == HBAS_LAYOUT_A
			? decode_table(left_a, sizeof(left_a) / sizeof(left_a[0]), p, plen,
				       b->prev_left, sizeof(b->prev_left), out, max)
			: decode_table(left_b, sizeof(left_b) / sizeof(left_b[0]), p, plen,
				       b->prev_left, sizeof(b->prev_left), out, max);
	case HBAS_MSG_FRONT_CONTROLS: {
		/* Single keys only; stock's multi-key combo lookup isn't reproduced. */
		uint8_t cur[4] = { 0 };
		size_t n = 0;

		memcpy(cur, p, plen < 4 ? plen : 4);
		for (int byte = 0; byte < 4; byte++)
			for (int bit = 0; bit < 8; bit++) {
				uint8_t m = 1u << bit, code = front[byte * 8 + bit];

				if (code && (cur[byte] & m) != (b->prev_front[byte] & m))
					n = emit(out, n, max, code, cur[byte] & m);
			}
		memcpy(b->prev_front, cur, 4);
		return n;
	}
	default:
		return 0;
	}
}

const char *hbas_key_name(uint8_t code)
{
	switch (code) {
	case 13: return "RIGHT_CONTROL_CENTER";
	case 32: return "LEFT_CONTROL_CENTER";
	case 'U': return "RIGHT_CONTROL_UP";
	case 'J': return "RIGHT_CONTROL_DOWN";
	case 'H': return "RIGHT_CONTROL_LEFT";
	case 'K': return "RIGHT_CONTROL_RIGHT";
	case 'W': return "LEFT_CONTROL_UP";
	case 'S': return "LEFT_CONTROL_DOWN";
	case 'A': return "LEFT_CONTROL_LEFT";
	case 'D': return "LEFT_CONTROL_RIGHT";
	case 'X': return "SYSTEM_INFO";
	case 'T': return "DRIVER_SQUELCH_UP";
	case 'M': return "DRIVER_SQUELCH_DN";
	case 'R': return "DRIVER_PTT";
	case 'O': return "POWER";
	case 'I': return "HOME";
	case 'C': return "FAV";
	case 'N': return "NAV";
	case 'E': return "REAR_PTT";
	case '1': return "SK_1"; case '2': return "SK_2"; case '3': return "SK_3";
	case '4': return "SK_4"; case '5': return "SK_5"; case '6': return "SK_6";
	case '7': return "SK_7"; case '8': return "SK_8";
	default: return NULL;
	}
}
