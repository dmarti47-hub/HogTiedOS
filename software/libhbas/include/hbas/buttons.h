/* SPDX-License-Identifier: MIT */
/*
 * Button decoding for IOC channel 3, matching stock iocInterface
 * (docs/findings/CROSS_CHECKS.md sec. 13). Keys are reported with the stock
 * key codes (ASCII), which Harley's btnCfg.xml names.
 */
#ifndef HBAS_BUTTONS_H
#define HBAS_BUTTONS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HBAS_MSG_LEFT_HANDLEBAR  0x0550
#define HBAS_MSG_RIGHT_HANDLEBAR 0x0570
#define HBAS_MSG_FRONT_CONTROLS  0xFACE
#define HBAS_MSG_TOUCH           0xFEE1

/* Stock key codes named in btnCfg.xml */
enum hbas_key {
	HBAS_KEY_RIGHT_CENTER = 13,  HBAS_KEY_LEFT_CENTER = 32,
	HBAS_KEY_RIGHT_UP = 'U',     HBAS_KEY_RIGHT_DOWN = 'J',
	HBAS_KEY_RIGHT_LEFT = 'H',   HBAS_KEY_RIGHT_RIGHT = 'K',
	HBAS_KEY_LEFT_UP = 'W',      HBAS_KEY_LEFT_DOWN = 'S',
	HBAS_KEY_LEFT_LEFT = 'A',    HBAS_KEY_LEFT_RIGHT = 'D',
	HBAS_KEY_SYSTEM_INFO = 'X',  HBAS_KEY_SQUELCH_UP = 'T',
	HBAS_KEY_SQUELCH_DOWN = 'M', HBAS_KEY_DRIVER_PTT = 'R',
	HBAS_KEY_POWER = 'O',        HBAS_KEY_HOME = 'I',
	HBAS_KEY_FAV = 'C',          HBAS_KEY_NAV = 'N',
	HBAS_KEY_REAR_PTT = 'E',
	/* '1'..'8' = soft keys / presets 1-8 */
};

/* Handlebar decoder variant; stock firmware always uses A (sec. 13). */
enum hbas_handlebar_layout { HBAS_LAYOUT_A = 0, HBAS_LAYOUT_B = 1 };

struct hbas_key_event {
	uint8_t code;      /* stock key code */
	bool pressed;      /* false = released */
};

struct hbas_buttons {
	enum hbas_handlebar_layout layout;
	uint8_t prev_left[8], prev_right[8], prev_front[4];
};

void hbas_buttons_init(struct hbas_buttons *b, enum hbas_handlebar_layout layout);

/*
 * Decode one channel-3 message ([ID lo, ID hi, length, payload...]). Writes up
 * to max key events for bits that changed since the previous message of the
 * same ID and returns the count. Touch and other IDs produce none.
 */
size_t hbas_buttons_decode(struct hbas_buttons *b, const uint8_t *msg, size_t len,
			   struct hbas_key_event *out, size_t max);

/* btnCfg.xml name for a stock key code, or NULL if it has none. */
const char *hbas_key_name(uint8_t code);

#endif
