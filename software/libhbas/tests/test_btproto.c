// SPDX-License-Identifier: MIT
#include "hbas/btproto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK(%s) failed\n", \
	__FILE__, __LINE__, #c); failures++; } } while (0)

static void test_roundtrip_and_quoting(void)
{
	char line[HBAS_BT_LINE_MAX];
	struct hbas_bt_msg m;

	CHECK(hbas_bt_format(line, sizeof(line), "track", "title", "Back in \"Black\"",
			     "artist", "AC\\DC", "album", "", "duration", "255000", NULL) > 0);
	CHECK(!strcmp(line, "track title=\"Back in \\\"Black\\\"\" artist=\"AC\\\\DC\" "
			    "album=\"\" duration=255000\n"));
	CHECK(hbas_bt_parse(line, &m) == 0 && !strcmp(m.verb, "track") && m.nargs == 4);
	CHECK(!strcmp(hbas_bt_get(&m, "title"), "Back in \"Black\""));
	CHECK(!strcmp(hbas_bt_get(&m, "artist"), "AC\\DC"));
	CHECK(!strcmp(hbas_bt_get(&m, "album"), "") && !hbas_bt_get(&m, "nope"));
	/* newlines in metadata can't break the one-line framing */
	hbas_bt_format(line, sizeof(line), "track", "title", "a\nb", NULL);
	CHECK(!strcmp(line, "track title=\"a b\"\n"));
	/* bare words for commands */
	CHECK(hbas_bt_parse("pairable on\n", &m) == 0 && m.nargs == 1 &&
	      !strcmp(m.arg[0].val, "on") && !m.arg[0].key[0]);
	CHECK(hbas_bt_parse("play", &m) == 0 && m.nargs == 0);
}

static void test_long_and_bad_input(void)
{
	char line[HBAS_BT_LINE_MAX], big[300];
	struct hbas_bt_msg m;

	/* long UTF-8 titles are cut on a character boundary */
	big[0] = '\0';
	for (int i = 0; i < 60; i++)
		strcat(big, "\xc3\xa9");                    /* e-acute, 2 bytes */
	CHECK(hbas_bt_format(line, sizeof(line), "track", "title", big, NULL) > 0);
	CHECK(hbas_bt_parse(line, &m) == 0);
	CHECK(strlen(hbas_bt_get(&m, "title")) % 2 == 0);
	CHECK(strlen(hbas_bt_get(&m, "title")) < HBAS_BT_TEXT_MAX);
	CHECK(hbas_bt_format(line, 10, "track", "title", "too long for this", NULL) == -1);
	CHECK(hbas_bt_parse("", &m) == -1);
	CHECK(hbas_bt_parse("track title=\"unterminated\n", &m) == -1);
	CHECK(hbas_bt_parse("track =x\n", &m) == -1);
}

static void apply(struct hbas_bt_state *s, const char *line)
{
	struct hbas_bt_msg m;

	CHECK(hbas_bt_parse(line, &m) == 0 && hbas_bt_apply(s, &m));
}

static void test_state(void)
{
	struct hbas_bt_state s;
	struct hbas_bt_msg m;

	hbas_bt_state_init(&s);
	apply(&s, "bt powered=1 pairable=0 connected=1 name=\"Dave's Pixel\" player=1\n");
	CHECK(s.powered && s.connected && s.player && !strcmp(s.device, "Dave's Pixel"));
	CHECK(!s.agent);
	apply(&s, "bt agent=1\n");
	CHECK(s.agent && s.connected);
	apply(&s, "track title=Thunderstruck artist=\"AC/DC\" album=\"The Razors Edge\" duration=292000\n");
	apply(&s, "play status=playing position=12000\n");
	CHECK(s.status == HBAS_BT_PLAYING && s.position_ms == 12000 && s.duration_ms == 292000);
	CHECK(!strcmp(s.title, "Thunderstruck"));
	apply(&s, "play status=paused\n");                 /* position kept */
	CHECK(s.status == HBAS_BT_PAUSED && s.position_ms == 12000);
	apply(&s, "pair device=\"New Phone\" passkey=123456\n");
	CHECK(s.pair_request && s.pair_passkey == 123456 && !strcmp(s.pair_device, "New Phone"));
	apply(&s, "pair-end result=ok\n");
	CHECK(!s.pair_request && !strcmp(s.pair_result, "ok"));
	/* disconnecting clears the track */
	apply(&s, "bt connected=0\n");
	CHECK(!s.connected && !s.player && !s.title[0] && s.status == HBAS_BT_STOPPED);
	/* unknown verbs are ignored */
	CHECK(hbas_bt_parse("future thing=1\n", &m) == 0 && !hbas_bt_apply(&s, &m));
}


static void test_device_list(void)
{
	struct hbas_bt_state s;
	struct hbas_bt_msg m;

	hbas_bt_state_init(&s);
	CHECK(!hbas_bt_parse("devices n=2", &m) && hbas_bt_apply(&s, &m));
	CHECK(s.dev_expected == 2 && s.ndev == 0);
	CHECK(!hbas_bt_parse("device id=\"/org/bluez/hci0/dev_AA\" name=\"Pixel\" paired=1 connected=1",
			     &m) && hbas_bt_apply(&s, &m));
	CHECK(!hbas_bt_parse("device id=\"/org/bluez/hci0/dev_BB\" name=\"Garmin\" paired=1 connected=0",
			     &m) && hbas_bt_apply(&s, &m));
	CHECK(s.ndev == 2);
	CHECK(!strcmp(s.dev[0].name, "Pixel") && s.dev[0].connected && s.dev[0].paired);
	CHECK(!strcmp(s.dev[1].id, "/org/bluez/hci0/dev_BB") && !s.dev[1].connected);
	/* a fresh "devices" resets the list */
	CHECK(!hbas_bt_parse("devices n=0", &m) && hbas_bt_apply(&s, &m));
	CHECK(s.ndev == 0);
}

int main(void)
{
	test_roundtrip_and_quoting();
	test_long_and_bad_input();
	test_state();
	test_device_list();
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return EXIT_FAILURE;
	}
	puts("test_btproto: all checks passed");
	return EXIT_SUCCESS;
}
