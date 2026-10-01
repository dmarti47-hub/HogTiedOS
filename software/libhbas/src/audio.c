// SPDX-License-Identifier: MIT
#include "hbas/audio.h"

#include <stdio.h>
#include <string.h>

/* audioCtrlSvc.cfg volume_ctrl_step_curve (PRD 5.8.2 / 5.9.4.1), dB */
static const int8_t volume_curve[HBAS_VOL_STEPS] = {
	-100, -41, -34, -29, -23, -18, -13, -9, -5, -1, 2, 5, 8, 10, 12, 13, 15, 16,
};
/* audioMgr.cfg fade_response_curve (PRD 5.9.12.1), dB */
static const int8_t fade_front[HBAS_FADE_STEPS] = {
	-100, -34, -24, -16, -10, -6, -4, -2, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};
static const int8_t fade_rear[HBAS_FADE_STEPS] = {
	0, 0, 0, 0, 0, 0, 0, 0, 0, -2, -4, -6, -10, -16, -24, -34, -100,
};

void hbas_audio_defaults(struct hbas_audio_settings *s)
{
	memset(s, 0, sizeof(*s));
	for (int i = 0; i < HBAS_OUT_COUNT; i++)
		s->volume[i] = HBAS_VOL_DEFAULT;
	s->fade = HBAS_FADE_STEPS / 2;
	s->output = HBAS_OUT_SPEAKERS;
	s->speakers = 4;
}

int hbas_volume_step_db(unsigned step)
{
	return volume_curve[step < HBAS_VOL_STEPS ? step : HBAS_VOL_STEPS - 1];
}

void hbas_fade_step_db(unsigned step, int *front_db, int *rear_db)
{
	if (step >= HBAS_FADE_STEPS)
		step = HBAS_FADE_STEPS - 1;
	*front_db = fade_front[step];
	*rear_db = fade_rear[step];
}

void hbas_audio_to_db(const struct hbas_audio_settings *s, struct hbas_audio_db *out)
{
	out->volume_db = hbas_volume_step_db(s->volume[s->output]);
	if (s->speakers == 4 && s->output == HBAS_OUT_SPEAKERS) {
		hbas_fade_step_db(s->fade, &out->fade_front_db, &out->fade_rear_db);
	} else {
		out->fade_front_db = out->fade_rear_db = 0;   /* no fade without rear speakers */
	}
}

static bool step(uint8_t *v, int delta, unsigned max)
{
	int n = (int)*v + delta;

	if (n < 0)
		n = 0;
	if (n > (int)max)
		n = (int)max;
	if (n == *v)
		return false;
	*v = (uint8_t)n;
	return true;
}

bool hbas_audio_adjust(struct hbas_audio_settings *s, enum hbas_audio_item item, int delta)
{
	switch (item) {
	case HBAS_AI_VOLUME: return step(&s->volume[s->output], delta, HBAS_VOL_STEPS - 1);
	case HBAS_AI_FADE:
		return s->speakers == 4 && step(&s->fade, delta, HBAS_FADE_STEPS - 1);
	case HBAS_AI_OUTPUT: {
		uint8_t o = (uint8_t)s->output;
		bool changed = step(&o, delta, HBAS_OUT_COUNT - 1);

		s->output = (enum hbas_audio_output)o;
		return changed;
	}
	default:
		return false;
	}
}

void hbas_eq_profile_name(char *buf, unsigned len, unsigned bike_cfg, bool engine_running,
			  enum hbas_audio_output out)
{
	const char *eng = engine_running ? "ON" : "OFF";

	if (out == HBAS_OUT_SPEAKERS)
		snprintf(buf, len, "%02u_%s.bin", bike_cfg, eng);   /* stock "%02d": 00..255 */
	else
		snprintf(buf, len, "HS_%s.bin", eng);
}
