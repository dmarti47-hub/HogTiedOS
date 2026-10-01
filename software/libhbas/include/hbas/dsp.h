/* SPDX-License-Identifier: MIT */
/*
 * SigmaDSP parameter writes for the audio settings, from static analysis of
 * stock audioCtrlSvc (docs/findings/AUDIO.md sec. 7). Not yet observed on
 * hardware.
 *
 * Gains are linear Q5.23 words (0x00800000 = 1.0 = 0 dB), written one word
 * per SPI frame: [0x00, addr_hi, addr_lo, 4 bytes big-endian, top nibble 0].
 *
 *   main_mixer1 (DSP_SET_CLKLESS_XBAR_MIXER, 0x92A): 10 inputs x 8 outputs,
 *     word at 295 + 16*input + output. Stock puts each output's volume here
 *     (harleyManager updateMainMixer1 -> setMixer(input, output, dB)).
 *   output gain (DSP_SET_OUTPUT_GAIN, 0x906): 10 channels at 1109 + ch.
 *     setFade() drives ch 0-1 (front) and 2-3 (rear).
 */
#ifndef HBAS_DSP_H
#define HBAS_DSP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hbas/audio.h"

#define HBAS_DSP_FRAME_MAX      7       /* single-word write */

#define HBAS_DSP_MIXER_BASE     295
#define HBAS_DSP_MIXER_STRIDE   16      /* per input */
#define HBAS_DSP_OUTGAIN_BASE   1109

/* main_mixer1 inputs (rows); MEDIA is stereo, the rest mono */
enum hbas_dsp_mix_in {
	HBAS_MIX_IN_MEDIA_L, HBAS_MIX_IN_MEDIA_R, HBAS_MIX_IN_PHONE, HBAS_MIX_IN_NAV,
	HBAS_MIX_IN_VOX_D, HBAS_MIX_IN_VOX_P, HBAS_MIX_IN_CB_RX, HBAS_MIX_IN_BEEP,
	HBAS_MIX_IN_RINGTONE, HBAS_MIX_IN_TA, HBAS_MIX_IN_COUNT
};

/* main_mixer1 outputs (columns), stereo pairs: speakers, then the driver,
 * passenger and fourth ("S") headset jacks */
enum hbas_dsp_mix_out {
	HBAS_MIX_OUT_SPK_L, HBAS_MIX_OUT_SPK_R, HBAS_MIX_OUT_HDST_D_L, HBAS_MIX_OUT_HDST_D_R,
	HBAS_MIX_OUT_HDST_P_L, HBAS_MIX_OUT_HDST_P_R, HBAS_MIX_OUT_HDST_S_L, HBAS_MIX_OUT_HDST_S_R,
	HBAS_MIX_OUT_COUNT
};

/* output-gain channels driven by fade */
enum { HBAS_OUTGAIN_FRONT_L, HBAS_OUTGAIN_FRONT_R, HBAS_OUTGAIN_REAR_L, HBAS_OUTGAIN_REAR_R };

struct hbas_dsp_write {
	uint16_t addr;
	int32_t word;                       /* Q5.23 */
};

/*
 * Stock's dB -> gain conversion (table at 0x142154): 1 dB steps, -100 and
 * below = 0 (off), clamped to +24 dB (the DSP's limit for these cells).
 */
int32_t hbas_dsp_gain_word(int db);

static inline uint16_t hbas_dsp_mixer_addr(enum hbas_dsp_mix_in in, enum hbas_dsp_mix_out out)
{
	return (uint16_t)(HBAS_DSP_MIXER_BASE + HBAS_DSP_MIXER_STRIDE * in + out);
}

/* One parameter write as an SPI frame; returns its length (7). */
size_t hbas_dsp_write_frame(uint8_t out[HBAS_DSP_FRAME_MAX], const struct hbas_dsp_write *w);

/*
 * The writes that apply the Audio page: media goes to the selected output
 * at the volume (muted = off), every other media crosspoint is off, and the
 * fade sets the front/rear output gains. Returns the number of writes
 * (at most HBAS_DSP_AUDIO_WRITES).
 */
#define HBAS_DSP_AUDIO_WRITES (2 * HBAS_MIX_OUT_COUNT + 4)
size_t hbas_dsp_audio_writes(const struct hbas_audio_db *db, bool muted,
			     enum hbas_audio_output out, struct hbas_dsp_write *w);

#endif
