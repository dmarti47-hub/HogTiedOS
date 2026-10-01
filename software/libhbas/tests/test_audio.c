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

static void test_custom_volume_curve(void)
{
	int prev = -1000;

	CHECK(hbas_volume_step_db_sys(17, HBAS_SYS_CUSTOM) == 0);     /* tops out at 0 dB */
	CHECK(hbas_volume_step_db_sys(0, HBAS_SYS_CUSTOM) == -100);   /* mute stays mute */
	CHECK(hbas_volume_step_db_sys(17, HBAS_SYS_STOCK) == 16);
	for (unsigned s = 0; s < HBAS_VOL_STEPS; s++) {             /* every step audible, rising */
		int db = hbas_volume_step_db_sys(s, HBAS_SYS_CUSTOM);

		CHECK(db > prev && db <= 0);
		prev = db;
	}
}

static void test_defaults_and_db(void)
{
	struct hbas_audio_settings s;
	struct hbas_audio_db db;

	hbas_audio_defaults(&s);
	CHECK(s.volume[HBAS_OUT_SPEAKERS] == 5 && s.system == HBAS_SYS_STOCK);
	CHECK(s.headset == HBAS_HS_OFF && hbas_audio_output(&s) == HBAS_OUT_SPEAKERS);
	hbas_audio_to_db(&s, NULL, &db);
	CHECK(db.volume_db == -18 && db.fade_front_db == 0 && db.fade_rear_db == 0);
}

static void test_custom_never_clips_with_eq(void)
{
	struct hbas_audio_settings s;
	struct hbas_audio_db db;
	struct hbas_eq eq;

	hbas_audio_defaults(&s);
	s.system = HBAS_SYS_CUSTOM;
	s.volume[HBAS_OUT_SPEAKERS] = 17;
	hbas_eq_set_preset(&eq, HBAS_EQ_FLAT);
	hbas_audio_to_db(&s, &eq, &db);
	CHECK(db.volume_db == 0);
	eq.gain_db[0] = 10;                          /* +10 dB bass boost */
	hbas_audio_to_db(&s, &eq, &db);
	CHECK(db.volume_db == -10);                  /* headroom for the boost */
	eq.gain_db[0] = -6;                          /* cuts need no headroom */
	hbas_audio_to_db(&s, &eq, &db);
	CHECK(db.volume_db == 0);
	/* stock speakers keep the stock curve and no automatic headroom */
	s.system = HBAS_SYS_STOCK;
	eq.gain_db[0] = 10;
	hbas_audio_to_db(&s, &eq, &db);
	CHECK(db.volume_db == 16);
	/* headsets aren't the custom speaker system: stock curve */
	s.system = HBAS_SYS_CUSTOM;
	s.headset = HBAS_HS_DRIVER;
	s.volume[HBAS_OUT_HEADSET_DRIVER] = 17;
	hbas_audio_to_db(&s, &eq, &db);
	CHECK(db.volume_db == 16);
	/* mute stays mute */
	s.headset = HBAS_HS_OFF;
	s.volume[HBAS_OUT_SPEAKERS] = 0;
	hbas_audio_to_db(&s, &eq, &db);
	CHECK(db.volume_db == -100);
}

static void test_adjust_ranges(void)
{
	struct hbas_audio_settings s;

	hbas_audio_defaults(&s);
	for (int i = 0; i < 30; i++)
		hbas_audio_adjust(&s, HBAS_AI_VOLUME, +1);
	CHECK(s.volume[HBAS_OUT_SPEAKERS] == 17 && !hbas_audio_adjust(&s, HBAS_AI_VOLUME, +1));
	/* speaker system: two options */
	CHECK(hbas_audio_adjust(&s, HBAS_AI_SYSTEM, +1) && s.system == HBAS_SYS_CUSTOM);
	CHECK(!hbas_audio_adjust(&s, HBAS_AI_SYSTEM, +1));
	/* headset: off -> driver -> passenger; each output keeps its own volume */
	CHECK(hbas_audio_adjust(&s, HBAS_AI_HEADSET, +1) && hbas_audio_output(&s) == HBAS_OUT_HEADSET_DRIVER);
	CHECK(s.volume[HBAS_OUT_HEADSET_DRIVER] == 5);
	CHECK(hbas_audio_adjust(&s, HBAS_AI_HEADSET, +1) && hbas_audio_output(&s) == HBAS_OUT_HEADSET_PASSENGER);
	CHECK(!hbas_audio_adjust(&s, HBAS_AI_HEADSET, +1));
}

static void test_fade_availability(void)
{
	struct hbas_audio_settings s;
	struct hbas_audio_db db;

	hbas_audio_defaults(&s);
	CHECK(hbas_audio_adjust(&s, HBAS_AI_FADE, +8) && s.fade == 16);
	hbas_audio_to_db(&s, NULL, &db);
	CHECK(db.fade_rear_db == -100);
	s.speakers = 2;                              /* stock 2-speaker bike: no fade */
	hbas_audio_to_db(&s, NULL, &db);
	CHECK(db.fade_rear_db == 0 && !hbas_audio_fade_available(&s));
	s.system = HBAS_SYS_CUSTOM;                  /* custom system: fade allowed */
	CHECK(hbas_audio_fade_available(&s));
	s.headset = HBAS_HS_DRIVER;                  /* headset: never */
	CHECK(!hbas_audio_fade_available(&s));
}

static void test_factory_eq_choice(void)
{
	struct hbas_audio_settings s;
	char b[32];

	hbas_audio_defaults(&s);
	hbas_audio_factory_eq(b, sizeof(b), &s, 2, true);
	CHECK(!strcmp(b, "02_ON.bin"));              /* stock speakers, known bike */
	hbas_audio_factory_eq(b, sizeof(b), &s, -1, true);
	CHECK(!strcmp(b, HBAS_EQ_BUILTIN_FLAT));     /* bike unknown */
	s.system = HBAS_SYS_CUSTOM;
	hbas_audio_factory_eq(b, sizeof(b), &s, 2, true);
	CHECK(!strcmp(b, HBAS_EQ_BUILTIN_FLAT));     /* custom: Harley speaker EQ bypassed */
	s.headset = HBAS_HS_PASSENGER;
	hbas_audio_factory_eq(b, sizeof(b), &s, 2, false);
	CHECK(!strcmp(b, "HS_OFF.bin"));             /* headsets keep their profile */
	hbas_eq_profile_name(b, sizeof(b), 128, false, HBAS_OUT_SPEAKERS);
	CHECK(!strcmp(b, "128_OFF.bin"));            /* 3-digit configs exist */
}

int main(void)
{
	test_stock_curves();
	test_custom_volume_curve();
	test_defaults_and_db();
	test_custom_never_clips_with_eq();
	test_adjust_ranges();
	test_fade_availability();
	test_factory_eq_choice();
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return EXIT_FAILURE;
	}
	puts("test_audio: all checks passed");
	return EXIT_SUCCESS;
}
