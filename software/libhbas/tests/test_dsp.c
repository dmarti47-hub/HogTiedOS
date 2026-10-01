// SPDX-License-Identifier: MIT
#include "hbas/dsp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK(%s) failed\n", \
	__FILE__, __LINE__, #c); failures++; } } while (0)

static void test_gain_words_match_stock_table(void)
{
	/* stock audioCtrlSvc table at 0x142154 (1.0 = 2^24), halved for the DSP */
	CHECK(hbas_dsp_gain_word(0) == 0x01000000 >> 1);
	CHECK(hbas_dsp_gain_word(-1) == 0x00e42905 >> 1);
	CHECK(hbas_dsp_gain_word(-6) == 0x00804dce >> 1);
	CHECK(hbas_dsp_gain_word(-20) == 0x00199999 >> 1);
	CHECK(hbas_dsp_gain_word(-99) == 0x000000bc >> 1);
	CHECK(hbas_dsp_gain_word(20) == 0x0a000000 >> 1);
	CHECK(hbas_dsp_gain_word(-100) == 0 && hbas_dsp_gain_word(-120) == 0);
	CHECK(hbas_dsp_gain_word(40) == hbas_dsp_gain_word(24));      /* clamped */
}

static void test_frame(void)
{
	struct hbas_dsp_write w = { 1109, 0x00800000 };
	uint8_t f[HBAS_DSP_FRAME_MAX];
	static const uint8_t want[] = { 0x00, 0x04, 0x55, 0x00, 0x80, 0x00, 0x00 };

	CHECK(hbas_dsp_write_frame(f, &w) == 7 && !memcmp(f, want, 7));
	w.word = -1;                                   /* only 28 bits go out */
	hbas_dsp_write_frame(f, &w);
	CHECK(f[3] == 0x0f && f[6] == 0xff);
}

static int32_t find(const struct hbas_dsp_write *w, size_t n, uint16_t addr)
{
	for (size_t i = 0; i < n; i++)
		if (w[i].addr == addr)
			return w[i].word;
	return -1;
}

static void test_audio_writes(void)
{
	struct hbas_dsp_write w[HBAS_DSP_AUDIO_WRITES];
	struct hbas_audio_db db = { .volume_db = -18, .fade_front_db = 0, .fade_rear_db = -100 };
	size_t n = hbas_dsp_audio_writes(&db, false, HBAS_OUT_SPEAKERS, w);

	CHECK(n == HBAS_DSP_AUDIO_WRITES);
	CHECK(hbas_dsp_mixer_addr(HBAS_MIX_IN_MEDIA_L, HBAS_MIX_OUT_SPK_L) == 295);
	CHECK(hbas_dsp_mixer_addr(HBAS_MIX_IN_MEDIA_R, HBAS_MIX_OUT_SPK_R) == 312);
	CHECK(find(w, n, 295) == hbas_dsp_gain_word(-18));         /* L -> L */
	CHECK(find(w, n, 312) == hbas_dsp_gain_word(-18));         /* R -> R */
	CHECK(find(w, n, 296) == 0 && find(w, n, 311) == 0);       /* no cross-feed */
	CHECK(find(w, n, 297) == 0 && find(w, n, 314) == 0);       /* headset off */
	CHECK(find(w, n, 1109) == 0x00800000 && find(w, n, 1110) == 0x00800000);
	CHECK(find(w, n, 1111) == 0 && find(w, n, 1112) == 0);     /* full front */

	n = hbas_dsp_audio_writes(&db, false, HBAS_OUT_HEADSET_PASSENGER, w);
	CHECK(find(w, n, 295) == 0 && find(w, n, 312) == 0);       /* speakers off */
	CHECK(find(w, n, 299) == hbas_dsp_gain_word(-18));         /* HDST_P L */
	CHECK(find(w, n, 316) == hbas_dsp_gain_word(-18));         /* HDST_P R */

	n = hbas_dsp_audio_writes(&db, true, HBAS_OUT_SPEAKERS, w);
	for (size_t i = 0; i < n; i++)
		if (w[i].addr < HBAS_DSP_OUTGAIN_BASE)
			CHECK(w[i].word == 0);                     /* mute: every media crosspoint off */
}

int main(void)
{
	test_gain_words_match_stock_table();
	test_frame();
	test_audio_writes();
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return EXIT_FAILURE;
	}
	puts("test_dsp: all checks passed");
	return EXIT_SUCCESS;
}
