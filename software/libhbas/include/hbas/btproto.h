/* SPDX-License-Identifier: MIT */
/*
 * hbas-btd <-> hogtied-ui protocol: one text line per message over a Unix
 * stream socket, "verb key=value key=value ...". Values that contain spaces,
 * quotes or backslashes are double-quoted with \" and \\ escapes. Easy to
 * watch and drive by hand (socat - UNIX-CONNECT:/run/hbas/bt.sock).
 *
 * daemon -> UI
 *   bt powered=0|1 pairable=0|1 agent=0|1 connected=0|1 name="Phone" [player=0|1]
 *      (agent=1: this daemon confirms pairing, so the UI may offer "Pair";
 *       on a PC the desktop pairs and "pairable" is ignored)
 *   track title=".." artist=".." album=".." duration=MS
 *   play status=playing|paused|stopped|forward-seek|reverse-seek|error position=MS
 *   pair device="Phone" passkey=123456      (confirm on screen)
 *   pair-end result=ok|rejected|failed|cancelled
 * UI -> daemon
 *   play | pause | next | previous | stop
 *   pairable on|off                          (visible + pairable for a while)
 *   power on|off                             (adapter power; unit only)
 *   confirm yes|no                           (answer to "pair")
 *   disconnect
 *   forget                                   (remove/unpair the phone; unit only)
 */
#ifndef HBAS_BTPROTO_H
#define HBAS_BTPROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HBAS_BT_LINE_MAX 512
#define HBAS_BT_TEXT_MAX 96          /* longest value kept (titles are cut) */
#define HBAS_BT_ARGS_MAX 8

struct hbas_bt_msg {
	char verb[16];
	unsigned nargs;
	struct { char key[16]; char val[HBAS_BT_TEXT_MAX]; } arg[HBAS_BT_ARGS_MAX];
};

/* Parse one line (without or with its '\n'). Returns 0, or -1 if malformed. */
int hbas_bt_parse(const char *line, struct hbas_bt_msg *m);
/* Value of key, or NULL. */
const char *hbas_bt_get(const struct hbas_bt_msg *m, const char *key);

/*
 * Build a line: verb then key/value pairs (NULL-terminated list of
 * const char * key, value, key, value, ..., NULL). Values are quoted as
 * needed and cut to HBAS_BT_TEXT_MAX - 1 bytes on a UTF-8 boundary. Ends
 * with '\n'. Returns the length, or -1 if it doesn't fit.
 */
int hbas_bt_format(char *buf, size_t len, const char *verb, ...);

/* What the UI shows, folded from daemon messages. */
enum hbas_bt_status { HBAS_BT_STOPPED, HBAS_BT_PLAYING, HBAS_BT_PAUSED, HBAS_BT_SEEKING,
		      HBAS_BT_ERROR };

struct hbas_bt_state {
	bool daemon;                 /* connected to hbas-btd */
	bool powered, pairable, agent, connected, player;
	char device[HBAS_BT_TEXT_MAX];
	char title[HBAS_BT_TEXT_MAX], artist[HBAS_BT_TEXT_MAX], album[HBAS_BT_TEXT_MAX];
	uint32_t duration_ms, position_ms;
	enum hbas_bt_status status;
	bool pair_request;           /* waiting for the rider to confirm */
	char pair_device[HBAS_BT_TEXT_MAX];
	uint32_t pair_passkey;
	char pair_result[16];        /* last pair-end result, "" if none */
	unsigned changes;            /* bumps on every applied message */
};

void hbas_bt_state_init(struct hbas_bt_state *s);
/* Apply one daemon message. Returns true if it was understood. */
bool hbas_bt_apply(struct hbas_bt_state *s, const struct hbas_bt_msg *m);

#endif
