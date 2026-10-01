// SPDX-License-Identifier: MIT
#define _GNU_SOURCE
#include "hbas/settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK(%s) failed\n", \
	__FILE__, __LINE__, #c); failures++; } } while (0)

static void defaults(struct hbas_audio_settings *a, struct hbas_eq *e)
{
	hbas_audio_defaults(a);
	hbas_eq_set_preset(e, HBAS_EQ_FLAT);
}

static void test_roundtrip(void)
{
	struct hbas_audio_settings a, b;
	struct hbas_eq e, f;
	char buf[HBAS_SETTINGS_MAX];

	defaults(&a, &e);
	a.volume[HBAS_OUT_SPEAKERS] = 12;
	a.volume[HBAS_OUT_HEADSET_PASSENGER] = 3;
	a.fade = 11;
	a.system = HBAS_SYS_CUSTOM;
	a.headset = HBAS_HS_DRIVER;
	hbas_eq_set_preset(&e, HBAS_EQ_HIGHWAY);
	hbas_eq_adjust(&e, 0, -7);                 /* -> custom */
	CHECK(hbas_settings_format(&a, &e, buf, sizeof(buf)) > 0);
	defaults(&b, &f);
	CHECK(hbas_settings_parse(buf, &b, &f) > 0);
	CHECK(!memcmp(b.volume, a.volume, sizeof(a.volume)));
	CHECK(b.fade == 11 && b.system == HBAS_SYS_CUSTOM && b.headset == HBAS_HS_DRIVER);
	CHECK(f.preset == HBAS_EQ_CUSTOM && !memcmp(f.gain_db, e.gain_db, sizeof(e.gain_db)));
	/* buffer too small is an error, not a truncated file */
	CHECK(hbas_settings_format(&a, &e, buf, 40) == -1);
}

static void test_preset_by_name(void)
{
	struct hbas_audio_settings a;
	struct hbas_eq e;

	defaults(&a, &e);
	CHECK(hbas_settings_parse("version=1\neq.preset=bass\n", &a, &e) == 1);
	CHECK(e.preset == HBAS_EQ_BASS && e.gain_db[0] == 6);
	CHECK(hbas_settings_parse("version=1\neq.preset=harley\n", &a, &e) == 1);
	CHECK(e.preset == HBAS_EQ_HARLEY);           /* gains come from the bike at runtime */
	/* "custom" without gains stays on the defaults' EQ */
	defaults(&a, &e);
	hbas_settings_parse("version=1\neq.preset=custom\n", &a, &e);
	CHECK(e.preset == HBAS_EQ_FLAT);
}

static void test_damaged_input_is_safe(void)
{
	struct hbas_audio_settings a, ref;
	struct hbas_eq e, eref;

	defaults(&a, &e);
	ref = a;
	eref = e;
	/* garbage, wrong version, out-of-range and malformed values change nothing */
	CHECK(hbas_settings_parse("\x01\x02garbage\n===\n", &a, &e) == -1);
	CHECK(hbas_settings_parse("version=2\nfade=3\n", &a, &e) == -1);
	CHECK(hbas_settings_parse("volume.speakers=5\n", &a, &e) == -1);    /* no version */
	CHECK(hbas_settings_parse("version=1\nvolume.speakers=99\nfade=-1\noutput=loud\n"
				  "headset=both\neq.gains=1,2,3\neq.gains=0,0,0,0,0,0,11\n"
				  "eq.gains=1,2,3,4,5,6,7,8\nvolume.speakers=7x\n", &a, &e) == 0);
	CHECK(!memcmp(&a, &ref, sizeof(a)) && !memcmp(&e, &eref, sizeof(e)));
	/* a truncated file applies only the complete, valid lines */
	CHECK(hbas_settings_parse("version=1\nvolume.speakers=9\nfade=1", &a, &e) == 2);
	CHECK(a.volume[HBAS_OUT_SPEAKERS] == 9 && a.fade == 1);
	/* CRLF, comments, unknown keys */
	defaults(&a, &e);
	CHECK(hbas_settings_parse("# hi\r\nversion=1\r\noutput=custom\r\nfuture.key=1\r\n",
				  &a, &e) == 1);
	CHECK(a.system == HBAS_SYS_CUSTOM);
}

static void test_atomic_save_and_load(void)
{
	char dir[] = "/tmp/hbas-settings-XXXXXX", path[256], tmp[300], buf[HBAS_SETTINGS_MAX];
	struct stat st;

	CHECK(mkdtemp(dir) != NULL);
	snprintf(path, sizeof(path), "%s/settings.conf", dir);
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	CHECK(hbas_settings_save(path, "version=1\nfade=4\n") == 0);
	CHECK(stat(tmp, &st) != 0);                   /* no temp file left behind */
	CHECK(hbas_settings_load(path, buf, sizeof(buf)) == 0 && !strcmp(buf, "version=1\nfade=4\n"));
	CHECK(hbas_settings_save(path, "version=1\nfade=5\n") == 0);   /* replace */
	CHECK(hbas_settings_load(path, buf, sizeof(buf)) == 0 && strstr(buf, "fade=5"));
	CHECK(hbas_settings_load("/nonexistent/x", buf, sizeof(buf)) == -1);
	CHECK(hbas_settings_save("/nonexistent/dir/x", "a") == -1);
	unlink(path);
	rmdir(dir);
}

int main(void)
{
	test_roundtrip();
	test_preset_by_name();
	test_damaged_input_is_safe();
	test_atomic_save_and_load();
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return EXIT_FAILURE;
	}
	puts("test_settings: all checks passed");
	return EXIT_SUCCESS;
}
