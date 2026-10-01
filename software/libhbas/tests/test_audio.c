// SPDX-License-Identifier: MIT
#include "hbas/audio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK(%s) failed\n", \
	__FILE__, __LINE__, #c); failures++; } } while (0)

static void test_stock_curves(void)
{
	int f, r;

	CHECK(hbas_volume_step_db(0) == -100);       /* mute */
	CHECK(hbas_volume_step_db(5) == -18);        /* stock default step */
	CHECK(hbas_volume_step_db(17) == 16);
	CHECK(hbas_volume_step_db(99) == 16);        /* clamped */
	hbas_fade_step_db(8, &f, &r);
	CHECK(f == 0 && r == 0);                     /* centre */
	hbas_fade_step_db(0, &f, &r);
	CHECK(f == -100 && r == 0);                  /* rear only */
	hbas_fade_step_db(16, &f, &r);
	CHECK(f == 0 && r == -100);                  /* front only */
}

static void test_defaults_and_db(void)
{
	struct hbas_audio_settings s;
	struct hbas_audio_db db;

	hbas_audio_defaults(&s);
	CHECK(s.volume[HBAS_OUT_SPEAKERS] == 5 && s.output == HBAS_OUT_SPEAKERS);
	hbas_audio_to_db(&s, &db);
	CHECK(db.volume_db == -18 && db.fade_front_db == 0 && db.fade_rear_db == 0);
}

static void test_adjust_ranges(void)
{
	struct hbas_audio_settings s;

	hbas_audio_defaults(&s);
	for (int i = 0; i < 30; i++)
		hbas_audio_adjust(&s, HBAS_AI_VOLUME, +1);
	CHECK(s.volume[HBAS_OUT_SPEAKERS] == 17);
	CHECK(!hbas_audio_adjust(&s, HBAS_AI_VOLUME, +1));
	for (int i = 0; i < 30; i++)
		hbas_audio_adjust(&s, HBAS_AI_VOLUME, -1);
	CHECK(s.volume[HBAS_OUT_SPEAKERS] == 0 && !hbas_audio_adjust(&s, HBAS_AI_VOLUME, -1));
	/* each output keeps its own volume */
	CHECK(hbas_audio_adjust(&s, HBAS_AI_OUTPUT, +1) && s.output == HBAS_OUT_HEADSET_DRIVER);
	CHECK(s.volume[HBAS_OUT_HEADSET_DRIVER] == 5);
	CHECK(hbas_audio_adjust(&s, HBAS_AI_OUTPUT, +1) && s.output == HBAS_OUT_HEADSET_PASSENGER);
	CHECK(!hbas_audio_adjust(&s, HBAS_AI_OUTPUT, +1));
}

static void test_fade_needs_four_speakers(void)
{
	struct hbas_audio_settings s;
	struct hbas_audio_db db;

	hbas_audio_defaults(&s);
	CHECK(hbas_audio_adjust(&s, HBAS_AI_FADE, +8) && s.fade == 16);
	hbas_audio_to_db(&s, &db);
	CHECK(db.fade_rear_db == -100);
	s.speakers = 2;
	hbas_audio_to_db(&s, &db);
	CHECK(db.fade_front_db == 0 && db.fade_rear_db == 0);
	CHECK(!hbas_audio_adjust(&s, HBAS_AI_FADE, -1));
	/* headsets: no fade either */
	s.speakers = 4;
	s.output = HBAS_OUT_HEADSET_DRIVER;
	hbas_audio_to_db(&s, &db);
	CHECK(db.fade_rear_db == 0);
}

static void test_eq_profile_names(void)
{
	char b[32];

	hbas_eq_profile_name(b, sizeof(b), 0, false, HBAS_OUT_SPEAKERS);
	CHECK(!strcmp(b, "00_OFF.bin"));
	hbas_eq_profile_name(b, sizeof(b), 2, true, HBAS_OUT_SPEAKERS);
	CHECK(!strcmp(b, "02_ON.bin"));
	hbas_eq_profile_name(b, sizeof(b), 128, false, HBAS_OUT_SPEAKERS);
	CHECK(!strcmp(b, "128_OFF.bin"));                /* 3-digit configs exist */
	hbas_eq_profile_name(b, sizeof(b), 2, true, HBAS_OUT_HEADSET_DRIVER);
	CHECK(!strcmp(b, "HS_ON.bin"));
}

int main(void)
{
	test_stock_curves();
	test_defaults_and_db();
	test_adjust_ranges();
	test_fade_needs_four_speakers();
	test_eq_profile_names();
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return EXIT_FAILURE;
	}
	puts("test_audio: all checks passed");
	return EXIT_SUCCESS;
}
