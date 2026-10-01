// SPDX-License-Identifier: MIT
#include "hbas/dsp.h"

#include <math.h>

int32_t hbas_dsp_gain_word(int db)
{
	if (db <= -100)
		return 0;
	if (db > 24)
		db = 24;
	/* stock keeps 1.0 = 2^24 and halves it on the way out; floor matches
	 * its table to the last bit except 3 entries (1 LSB of 2^-24) */
	return (int32_t)floor(pow(10.0, db / 20.0) * 16777216.0) >> 1;
}

size_t hbas_dsp_write_frame(uint8_t out[HBAS_DSP_FRAME_MAX], const struct hbas_dsp_write *w)
{
	uint32_t v = (uint32_t)w->word & 0x0FFFFFFFu;     /* 28 valid bits */

	out[0] = 0x00;                                    /* write */
	out[1] = (uint8_t)(w->addr >> 8);
	out[2] = (uint8_t)w->addr;
	out[3] = (uint8_t)(v >> 24);
	out[4] = (uint8_t)(v >> 16);
	out[5] = (uint8_t)(v >> 8);
	out[6] = (uint8_t)v;
	return HBAS_DSP_FRAME_MAX;
}

size_t hbas_dsp_audio_writes(const struct hbas_audio_db *db, bool muted,
			     enum hbas_audio_output out, struct hbas_dsp_write *w)
{
	static const enum hbas_dsp_mix_out left[] = {
		[HBAS_OUT_SPEAKERS] = HBAS_MIX_OUT_SPK_L,
		[HBAS_OUT_HEADSET_DRIVER] = HBAS_MIX_OUT_HDST_D_L,
		[HBAS_OUT_HEADSET_PASSENGER] = HBAS_MIX_OUT_HDST_P_L,
	};
	int32_t vol = muted ? 0 : hbas_dsp_gain_word(db->volume_db);
	size_t n = 0;

	/* media L -> left columns, R -> right columns (stock routes stereo
	 * inputs straight across), only on the selected output */
	for (int o = 0; o < HBAS_MIX_OUT_COUNT; o++) {
		bool on = (o & ~1) == (int)left[out];

		w[n++] = (struct hbas_dsp_write){
			hbas_dsp_mixer_addr(HBAS_MIX_IN_MEDIA_L, (enum hbas_dsp_mix_out)o),
			on && !(o & 1) ? vol : 0 };
		w[n++] = (struct hbas_dsp_write){
			hbas_dsp_mixer_addr(HBAS_MIX_IN_MEDIA_R, (enum hbas_dsp_mix_out)o),
			on && (o & 1) ? vol : 0 };
	}
	w[n++] = (struct hbas_dsp_write){ HBAS_DSP_OUTGAIN_BASE + HBAS_OUTGAIN_FRONT_L,
					  hbas_dsp_gain_word(db->fade_front_db) };
	w[n++] = (struct hbas_dsp_write){ HBAS_DSP_OUTGAIN_BASE + HBAS_OUTGAIN_FRONT_R,
					  hbas_dsp_gain_word(db->fade_front_db) };
	w[n++] = (struct hbas_dsp_write){ HBAS_DSP_OUTGAIN_BASE + HBAS_OUTGAIN_REAR_L,
					  hbas_dsp_gain_word(db->fade_rear_db) };
	w[n++] = (struct hbas_dsp_write){ HBAS_DSP_OUTGAIN_BASE + HBAS_OUTGAIN_REAR_R,
					  hbas_dsp_gain_word(db->fade_rear_db) };
	return n;
}
