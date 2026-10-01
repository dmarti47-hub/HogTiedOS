// SPDX-License-Identifier: MIT
#include "hbas/eq.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK(%s) failed\n", \
	__FILE__, __LINE__, #c); failures++; } } while (0)
#define NEAR(a, b, tol) (fabs((a) - (b)) <= (tol))

static void test_peaking_math(void)
{
	struct hbas_biquad bq;

	/* 0 dB is an exact pass-through */
	hbas_biquad_peaking(&bq, HBAS_DSP_FS, 1000, 0.0, HBAS_EQ_Q);
	CHECK(NEAR(bq.b0, 1.0, 1e-12) && NEAR(bq.b1, bq.a1, 1e-12) && NEAR(bq.b2, bq.a2, 1e-12));
	CHECK(NEAR(hbas_biquad_response_db(&bq, HBAS_DSP_FS, 50), 0.0, 1e-9));
	/* boost/cut hits its gain at the centre frequency and fades away from it */
	hbas_biquad_peaking(&bq, HBAS_DSP_FS, 1000, 6.0, HBAS_EQ_Q);
	CHECK(NEAR(hbas_biquad_response_db(&bq, HBAS_DSP_FS, 1000), 6.0, 0.01));
	CHECK(fabs(hbas_biquad_response_db(&bq, HBAS_DSP_FS, 20)) < 0.2);
	hbas_biquad_peaking(&bq, HBAS_DSP_FS, 250, -8.0, HBAS_EQ_Q);
	CHECK(NEAR(hbas_biquad_response_db(&bq, HBAS_DSP_FS, 250), -8.0, 0.01));
}

static void test_eq_model(void)
{
	struct hbas_eq eq;

	hbas_eq_set_preset(&eq, HBAS_EQ_FLAT);
	for (double f = 20; f < 20000; f *= 1.5)
		CHECK(fabs(hbas_eq_response_db(&eq, HBAS_DSP_FS, f)) < 1e-6);
	hbas_eq_set_preset(&eq, HBAS_EQ_BASS);
	CHECK(hbas_eq_response_db(&eq, HBAS_DSP_FS, 60) > 6.0);      /* 60 Hz band + overlap */
	CHECK(fabs(hbas_eq_response_db(&eq, HBAS_DSP_FS, 12000)) < 0.5);
	CHECK(eq.preset == HBAS_EQ_BASS);
	CHECK(hbas_eq_adjust(&eq, 2, +1) && eq.gain_db[2] == 1 && eq.preset == HBAS_EQ_CUSTOM);
	for (int i = 0; i < 30; i++)
		hbas_eq_adjust(&eq, 0, +1);
	CHECK(eq.gain_db[0] == HBAS_EQ_MAX_DB && !hbas_eq_adjust(&eq, 0, +1));
	CHECK(!hbas_eq_adjust(&eq, HBAS_EQ_BANDS, 1));               /* bad band */
}

static void test_dsp_words(void)
{
	struct hbas_biquad id = { 1.0, 0, 0, 0, 0 }, bq, huge = { 20.0, 0, 0, 0, 0 };
	int32_t w[5];

	CHECK(hbas_biquad_to_dsp(&id, w) == 0);
	CHECK(w[0] == 0 && w[1] == 0 && w[2] == 0x800000 && w[3] == 0 && w[4] == 0);
	/* feedback negated, Q5.23 */
	hbas_biquad_peaking(&bq, HBAS_DSP_FS, 60, 6.0, HBAS_EQ_Q);
	CHECK(hbas_biquad_to_dsp(&bq, w) == 0);
	CHECK(w[4] == (int32_t)lround(-bq.a1 * 8388608.0));
	CHECK(w[3] == (int32_t)lround(-bq.a2 * 8388608.0));
	/* same structural relation every factory peaking filter has: B1 = -A1(stored) */
	CHECK(w[1] == -w[4]);
	CHECK(hbas_biquad_to_dsp(&huge, w) == -1);                    /* outside +/-16 */
}

static void test_safeload_frames(void)
{
	uint8_t f[3][3 + 4 * HBAS_DSP_SAFELOAD_MAX];
	size_t len[3];
	int32_t words[5] = { 0, 0, 0x800000, -0x800000, 0 };

	CHECK(hbas_dsp_safeload(HBAS_DSP_BIQUAD_BASE, words, 5, f, len) == 3);
	/* data to safe-load base + 4 */
	CHECK(len[0] == 23 && f[0][0] == 0x00 && f[0][1] == 0x00 && f[0][2] == 0x07);
	CHECK(f[0][11] == 0x00 && f[0][12] == 0x80 && f[0][13] == 0x00 && f[0][14] == 0x00);
	/* -1.0 as a 28-bit word */
	CHECK(f[0][15] == 0x0F && f[0][16] == 0x80 && f[0][17] == 0x00 && f[0][18] == 0x00);
	/* target address 222 to base, count 5 to base + 2 */
	CHECK(len[1] == 7 && f[1][2] == 0x03 && f[1][5] == 0x00 && f[1][6] == 222);
	CHECK(len[2] == 7 && f[2][2] == 0x05 && f[2][6] == 5);
	CHECK(hbas_dsp_safeload(222, words, 0, f, len) == 0);
	CHECK(hbas_dsp_safeload(222, words, HBAS_DSP_SAFELOAD_MAX + 1, f, len) == 0);
}

int main(void)
{
	test_peaking_math();
	test_eq_model();
	test_dsp_words();
	test_safeload_frames();
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return EXIT_FAILURE;
	}
	puts("test_eq: all checks passed");
	return EXIT_SUCCESS;
}
