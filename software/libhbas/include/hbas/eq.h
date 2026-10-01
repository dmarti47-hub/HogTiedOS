/* SPDX-License-Identifier: MIT */
/*
 * User graphic EQ: band gains -> biquad coefficients -> SigmaDSP words.
 *
 * Verified (docs/findings/AUDIO.md sec. 6): coefficients are signed Q5.23,
 * the DSP stores the feedback terms negated, wire order is B2 B1 B0 A2 A1,
 * biquads sit at DSP address 222 + 5*i, up to 7 per set. The 7-band EQ fills
 * that set, replacing stock's bass/treble (whose DSP_SET_TONE writes go to
 * this bank).
 * Sample rate 48 kHz: confirmed from the factory tone filters (AUDIO.md
 * sec. 7.2). In stock these 7 slots hold bass, treble, Harley's fixed
 * voicing filters and a speed-dependent bass boost (sec. 7.3); writing the
 * user EQ there replaces all of them. The speed bass boost is dropped on
 * purpose: Speed volume (hbas/audio.h) is the only speed adjustment.
 */
#ifndef HBAS_EQ_H
#define HBAS_EQ_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HBAS_EQ_BANDS     7
#define HBAS_EQ_MAX_DB    10        /* slider range +/-10 dB, 1 dB steps */
#define HBAS_EQ_Q         1.05      /* ~1.33 octaves wide, matching the band spacing */
#define HBAS_DSP_FS       48000.0   /* DSP sample rate, AUDIO.md sec. 7.2 */

/* DSP layout (dsp_layout_map.json) */
#define HBAS_DSP_BIQUAD_BASE    222 /* address of biquad 0 */
#define HBAS_DSP_BIQUAD_STRIDE  5
#define HBAS_DSP_BIQUADS_PER_SET 7
#define HBAS_DSP_SAFELOAD_BASE  3   /* table entry 0 */
#define HBAS_DSP_SAFELOAD_MAX   60  /* words per safe-load */

extern const uint16_t hbas_eq_band_hz[HBAS_EQ_BANDS];   /* 63 .. 16k, 7-band ISO spacing */

enum hbas_eq_preset { HBAS_EQ_FLAT, HBAS_EQ_HARLEY, HBAS_EQ_BASS, HBAS_EQ_VOCAL,
		      HBAS_EQ_HIGHWAY, HBAS_EQ_CUSTOM, HBAS_EQ_PRESET_COUNT };

struct hbas_eq {
	int8_t gain_db[HBAS_EQ_BANDS];
	enum hbas_eq_preset preset;     /* CUSTOM once a band is changed by hand */
};

/* Normalised biquad, standard signs: y = b0 x + b1 x1 + b2 x2 - a1 y1 - a2 y2 */
struct hbas_biquad { double b0, b1, b2, a1, a2; };

/* HBAS_EQ_HARLEY starts flat: its gains depend on the bike, see below. */
void hbas_eq_set_preset(struct hbas_eq *eq, enum hbas_eq_preset p);

/*
 * The Harley preset: the stock radio's tone stage for this bike with bass
 * and treble centred (Harley's loudness contour, which changes with the
 * volume step, plus its fixed voicing), fitted to the 7 bands relative to
 * 1 kHz. Generated from the factory EQ profiles by
 * tools/eq-voicing/gen_harley_eq.py (AUDIO.md sec. 7.3). bike_cfg is
 * HD_Configuration_Options byte 0 (-1 unknown); headsets use the HS profile.
 * Writes the gains and returns true, or writes flat and returns false when
 * there's no factory tuning for this bike (stock falls back to flat too).
 */
bool hbas_eq_harley_gains(int8_t gain_db[HBAS_EQ_BANDS], int bike_cfg, bool headset,
			  bool engine_on, unsigned vol_step);
const char *hbas_eq_preset_name(enum hbas_eq_preset p);
/* Change one band by delta dB (clamped); marks the EQ CUSTOM. */
int hbas_eq_adjust(struct hbas_eq *eq, unsigned band, int delta);

/* RBJ audio-EQ-cookbook peaking filter. gain 0 gives an exact pass-through. */
void hbas_biquad_peaking(struct hbas_biquad *bq, double fs, double f0, double gain_db, double q);
double hbas_biquad_response_db(const struct hbas_biquad *bq, double fs, double f);
/* Combined response of all bands at frequency f. */
double hbas_eq_response_db(const struct hbas_eq *eq, double fs, double f);

/* Q5.23 words in DSP wire order: B2 B1 B0 -A2 -A1. Returns 0, or -1 if a
 * coefficient is outside the representable range (+/-16). */
int hbas_biquad_to_dsp(const struct hbas_biquad *bq, int32_t words[5]);

/*
 * SPI frames for a safe-load of n words to DSP address target, per
 * dsp_layout_map.json (`safeload`): data words to SAFELOAD_BASE + 4, then
 * [0 0 target_hi target_lo] to SAFELOAD_BASE, then [0 0 0 n] to
 * SAFELOAD_BASE + 2. Each frame is [0x00 (write), addr_hi, addr_lo,
 * big-endian 4-byte words...]. Writes up to 3 frames into out (each up to
 * 3 + 4*n bytes, lengths in frame_len) and returns the frame count, or 0 if
 * n is 0 or more than SAFELOAD_MAX.
 */
size_t hbas_dsp_safeload(uint16_t target, const int32_t *words, size_t n,
			 uint8_t out[3][3 + 4 * HBAS_DSP_SAFELOAD_MAX], size_t frame_len[3]);

#endif
