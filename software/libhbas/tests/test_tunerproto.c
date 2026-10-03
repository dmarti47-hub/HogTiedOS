// SPDX-License-Identifier: MIT
#include "hbas/tunerproto.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK(%s) failed\n", \
	__FILE__, __LINE__, #c); failures++; } } while (0)

static void test_freq_and_step(void)
{
	struct hbas_tuner_state t;
	char buf[16];
	const char *unit;

	hbas_tuner_state_init(&t);
	hbas_tuner_freq_str(&t, buf, sizeof(buf), &unit);
	CHECK(!strcmp(buf, "105.7") && !strcmp(unit, "MHz"));
	/* FM steps 200 kHz, wraps at the ends */
	CHECK(hbas_tuner_step(HBAS_BAND_FM, 107900, 1) == 87700);
	CHECK(hbas_tuner_step(HBAS_BAND_FM, 87700, -1) == 107900);
	CHECK(hbas_tuner_step(HBAS_BAND_FM, 105700, 2) == 106100);
	/* AM 10 kHz */
	CHECK(hbas_tuner_step(HBAS_BAND_AM, 530, -1) == 1700);
	CHECK(hbas_tuner_step(HBAS_BAND_AM, 1010, 1) == 1020);
	/* WB channels 1..7 */
	CHECK(hbas_tuner_step(HBAS_BAND_WB, 7, 1) == 1);
	CHECK(hbas_wb_channel_khz(1) == 162400 && hbas_wb_channel_khz(7) == 162550);
	t.band = HBAS_BAND_AM;
	t.freq = 1010;
	hbas_tuner_freq_str(&t, buf, sizeof(buf), &unit);
	CHECK(!strcmp(buf, "1010") && !strcmp(unit, "kHz"));
	t.band = HBAS_BAND_WB;
	t.freq = 3;
	hbas_tuner_freq_str(&t, buf, sizeof(buf), NULL);
	CHECK(!strcmp(buf, "WX3"));
}

static void test_protocol(void)
{
	struct hbas_tuner_state a, b;
	char line[128];

	hbas_tuner_state_init(&a);
	a.powered = true;
	a.band = HBAS_BAND_FM;
	a.freq = 97500;
	a.stereo = true;
	a.rssi = 42;
	a.snr = 18;
	CHECK(hbas_tuner_format(line, sizeof(line), &a) > 0);
	hbas_tuner_state_init(&b);
	CHECK(hbas_tuner_apply(&b, line));
	CHECK(b.powered && b.band == HBAS_BAND_FM && b.freq == 97500 && b.stereo && b.rssi == 42);
	CHECK(b.daemon);

	CHECK(hbas_tuner_format_rds(line, sizeof(line), "WXYZ-FM", "Now Playing: Steppenwolf"));
	CHECK(hbas_tuner_apply(&b, line));
	CHECK(!strcmp(b.ps, "WXYZ-FM") && !strcmp(b.rt, "Now Playing: Steppenwolf"));

	CHECK(!hbas_tuner_apply(&b, "gps link=1"));   /* not ours */

	hbas_tuner_cmd_band(line, sizeof(line), HBAS_BAND_AM);
	CHECK(!strcmp(line, "band AM\n"));
	hbas_tuner_cmd_tune(line, sizeof(line), 1010);
	CHECK(!strcmp(line, "tune 1010\n"));
	hbas_tuner_cmd_seek(line, sizeof(line), true);
	CHECK(!strcmp(line, "seek up\n"));
}

int main(void)
{
	test_freq_and_step();
	test_protocol();
	if (failures)
		fprintf(stderr, "%d failure(s)\n", failures);
	return failures != 0;
}
