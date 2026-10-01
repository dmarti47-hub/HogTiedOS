// SPDX-License-Identifier: MIT
#include "hbas/eq.h"

#include <math.h>
#include <string.h>

#include "harley_eq_table.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

const uint16_t hbas_eq_band_hz[HBAS_EQ_BANDS] = { 63, 160, 400, 1000, 2500, 6300, 16000 };

/* Starting points; easy to retune once heard on the bike. */
static const int8_t presets[HBAS_EQ_PRESET_COUNT][HBAS_EQ_BANDS] = {
	/*                   63 160 400 1k 2.5k 6.3k 16k */
	[HBAS_EQ_FLAT]    = { 0,  0,  0, 0,  0,   0,  0 },
	[HBAS_EQ_HARLEY]  = { 0,  0,  0, 0,  0,   0,  0 },   /* per bike: hbas_eq_harley_gains() */
	[HBAS_EQ_BASS]    = { 6,  4,  1, 0,  0,   0,  0 },
	[HBAS_EQ_VOCAL]   = { -2, -1, 0, 2,  3,   1,  0 },
	[HBAS_EQ_HIGHWAY] = { 3,  1, -1, 1,  3,   4,  3 },   /* lift over wind/engine noise */
	[HBAS_EQ_CUSTOM]  = { 0,  0,  0, 0,  0,   0,  0 },
};
static const char *const preset_names[HBAS_EQ_PRESET_COUNT] = {
	"Flat", "Harley", "Bass", "Vocal", "Highway", "Custom",
};

void hbas_eq_set_preset(struct hbas_eq *eq, enum hbas_eq_preset p)
{
	if (p >= HBAS_EQ_PRESET_COUNT)
		p = HBAS_EQ_FLAT;
	if (p != HBAS_EQ_CUSTOM)
		memcpy(eq->gain_db, presets[p], sizeof(eq->gain_db));
	eq->preset = p;
}

const char *hbas_eq_preset_name(enum hbas_eq_preset p)
{
	return p < HBAS_EQ_PRESET_COUNT ? preset_names[p] : "?";
}

bool hbas_eq_harley_gains(int8_t gain_db[HBAS_EQ_BANDS], int bike_cfg, bool headset,
			  bool engine_on, unsigned vol_step)
{
	int cfg = headset ? HBAS_HARLEY_HEADSET : bike_cfg;

	if (vol_step >= HBAS_VOL_STEPS)
		vol_step = HBAS_VOL_STEPS - 1;
	for (unsigned i = 0; cfg >= 0 && i < hbas_harley_voicing_count; i++) {
		const struct hbas_harley_voicing *v = &hbas_harley_voicing[i];

		if (v->cfg == cfg && v->engine_on == engine_on) {
			memcpy(gain_db, v->gain_db[vol_step], HBAS_EQ_BANDS);
			return true;
		}
	}
	memset(gain_db, 0, HBAS_EQ_BANDS);
	return false;
}

int hbas_eq_adjust(struct hbas_eq *eq, unsigned band, int delta)
{
	int g;

	if (band >= HBAS_EQ_BANDS)
		return 0;
	g = eq->gain_db[band] + delta;
	if (g > HBAS_EQ_MAX_DB)
		g = HBAS_EQ_MAX_DB;
	if (g < -HBAS_EQ_MAX_DB)
		g = -HBAS_EQ_MAX_DB;
	if (g == eq->gain_db[band])
		return 0;
	eq->gain_db[band] = (int8_t)g;
	eq->preset = HBAS_EQ_CUSTOM;
	return 1;
}

void hbas_biquad_peaking(struct hbas_biquad *bq, double fs, double f0, double gain_db, double q)
{
	double A = pow(10.0, gain_db / 40.0);
	double w0 = 2.0 * M_PI * f0 / fs;
	double alpha = sin(w0) / (2.0 * q);
	double c = cos(w0);
	double a0 = 1.0 + alpha / A;

	bq->b0 = (1.0 + alpha * A) / a0;
	bq->b1 = (-2.0 * c) / a0;
	bq->b2 = (1.0 - alpha * A) / a0;
	bq->a1 = (-2.0 * c) / a0;
	bq->a2 = (1.0 - alpha / A) / a0;
}

double hbas_biquad_response_db(const struct hbas_biquad *bq, double fs, double f)
{
	double w = 2.0 * M_PI * f / fs;
	/* H(e^jw) = (b0 + b1 z^-1 + b2 z^-2) / (1 + a1 z^-1 + a2 z^-2) */
	double nr = bq->b0 + bq->b1 * cos(w) + bq->b2 * cos(2 * w);
	double ni = -(bq->b1 * sin(w) + bq->b2 * sin(2 * w));
	double dr = 1.0 + bq->a1 * cos(w) + bq->a2 * cos(2 * w);
	double di = -(bq->a1 * sin(w) + bq->a2 * sin(2 * w));

	return 10.0 * log10((nr * nr + ni * ni) / (dr * dr + di * di));
}

double hbas_eq_response_db(const struct hbas_eq *eq, double fs, double f)
{
	double total = 0.0;

	for (int i = 0; i < HBAS_EQ_BANDS; i++) {
		struct hbas_biquad bq;

		hbas_biquad_peaking(&bq, fs, hbas_eq_band_hz[i], eq->gain_db[i], HBAS_EQ_Q);
		total += hbas_biquad_response_db(&bq, fs, f);
	}
	return total;
}

static int q523(double v, int32_t *out)
{
	double s = v * (double)(1 << 23);

	/* 28 valid bits on the wire: Q5.23 covers [-16, 16) */
	if (s >= 134217728.0 || s < -134217728.0)
		return -1;
	*out = (int32_t)lround(s);
	return 0;
}

int hbas_biquad_to_dsp(const struct hbas_biquad *bq, int32_t w[5])
{
	/* wire order B2 B1 B0 A2 A1; feedback stored negated (verified) */
	return q523(bq->b2, &w[0]) | q523(bq->b1, &w[1]) | q523(bq->b0, &w[2]) |
	       q523(-bq->a2, &w[3]) | q523(-bq->a1, &w[4]);
}

static size_t frame(uint8_t *f, uint16_t addr, const int32_t *words, size_t n)
{
	f[0] = 0x00;                    /* write */
	f[1] = (uint8_t)(addr >> 8);
	f[2] = (uint8_t)addr;
	for (size_t i = 0; i < n; i++) {
		uint32_t v = (uint32_t)words[i] & 0x0FFFFFFF;   /* 28 valid bits */

		f[3 + 4 * i] = (uint8_t)(v >> 24);
		f[4 + 4 * i] = (uint8_t)(v >> 16);
		f[5 + 4 * i] = (uint8_t)(v >> 8);
		f[6 + 4 * i] = (uint8_t)v;
	}
	return 3 + 4 * n;
}

size_t hbas_dsp_safeload(uint16_t target, const int32_t *words, size_t n,
			 uint8_t out[3][3 + 4 * HBAS_DSP_SAFELOAD_MAX], size_t frame_len[3])
{
	const int32_t tgt = target, cnt = (int32_t)n;

	if (n == 0 || n > HBAS_DSP_SAFELOAD_MAX)
		return 0;
	frame_len[0] = frame(out[0], HBAS_DSP_SAFELOAD_BASE + 4, words, n);
	frame_len[1] = frame(out[1], HBAS_DSP_SAFELOAD_BASE, &tgt, 1);
	frame_len[2] = frame(out[2], HBAS_DSP_SAFELOAD_BASE + 2, &cnt, 1);
	return 3;
}
