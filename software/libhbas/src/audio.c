// SPDX-License-Identifier: MIT
#include "hbas/audio.h"

#include <stdio.h>
#include <string.h>

/* audioCtrlSvc.cfg volume_ctrl_step_curve (PRD 5.8.2 / 5.9.4.1), dB */
static const int8_t volume_curve[HBAS_VOL_STEPS] = {
	-100, -41, -34, -29, -23, -18, -13, -9, -5, -1, 2, 5, 8, 10, 12, 13, 15, 16,
};
#define STOCK_VOL_MAX_DB 16
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
	s->system = HBAS_SYS_STOCK;
	s->headset = HBAS_HS_OFF;
	s->speakers = 4;
}

enum hbas_audio_output hbas_audio_output(const struct hbas_audio_settings *s)
{
	switch (s->headset) {
	case HBAS_HS_DRIVER: return HBAS_OUT_HEADSET_DRIVER;
	case HBAS_HS_PASSENGER: return HBAS_OUT_HEADSET_PASSENGER;
	default: return HBAS_OUT_SPEAKERS;
	}
}

int hbas_volume_step_db(unsigned step)
{
	return volume_curve[step < HBAS_VOL_STEPS ? step : HBAS_VOL_STEPS - 1];
}

int hbas_volume_step_db_sys(unsigned step, enum hbas_speaker_system sys)
{
	int db = hbas_volume_step_db(step);

	if (sys != HBAS_SYS_CUSTOM || db <= -100)
		return db;
	/* same shape, every step still audible, top step exactly 0 dB */
	return db - STOCK_VOL_MAX_DB;
}

void hbas_fade_step_db(unsigned step, int *front_db, int *rear_db)
{
	if (step >= HBAS_FADE_STEPS)
		step = HBAS_FADE_STEPS - 1;
	*front_db = fade_front[step];
	*rear_db = fade_rear[step];
}

bool hbas_audio_fade_available(const struct hbas_audio_settings *s)
{
	if (s->headset != HBAS_HS_OFF)
		return false;
	/* stock speakers: only 4-speaker bikes have rears; custom systems: assume they might */
	return s->system == HBAS_SYS_CUSTOM || s->speakers == 4;
}

static int eq_max_boost(const struct hbas_eq *eq)
{
	int m = 0;

	for (int i = 0; eq && i < HBAS_EQ_BANDS; i++)
		if (eq->gain_db[i] > m)
			m = eq->gain_db[i];
	return m;
}

void hbas_audio_to_db(const struct hbas_audio_settings *s, const struct hbas_eq *eq,
		      struct hbas_audio_db *out)
{
	enum hbas_audio_output o = hbas_audio_output(s);
	bool custom_spk = s->system == HBAS_SYS_CUSTOM && o == HBAS_OUT_SPEAKERS;

	out->volume_db = hbas_volume_step_db_sys(s->volume[o], custom_spk ? HBAS_SYS_CUSTOM
									    : HBAS_SYS_STOCK);
	/* custom system: keep EQ boosts from pushing the signal past 0 dB */
	if (custom_spk && out->volume_db > -100)
		out->volume_db -= eq_max_boost(eq);
	if (hbas_audio_fade_available(s))
		hbas_fade_step_db(s->fade, &out->fade_front_db, &out->fade_rear_db);
	else
		out->fade_front_db = out->fade_rear_db = 0;
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

static bool step_enum(unsigned *v, int delta, unsigned count)
{
	uint8_t x = (uint8_t)*v;
	bool changed = step(&x, delta, count - 1);

	*v = x;
	return changed;
}

bool hbas_audio_adjust(struct hbas_audio_settings *s, enum hbas_audio_item item, int delta)
{
	switch (item) {
	case HBAS_AI_VOLUME:
		return step(&s->volume[hbas_audio_output(s)], delta, HBAS_VOL_STEPS - 1);
	case HBAS_AI_FADE:
		return hbas_audio_fade_available(s) && step(&s->fade, delta, HBAS_FADE_STEPS - 1);
	case HBAS_AI_SYSTEM: {
		unsigned v = s->system;
		bool c = step_enum(&v, delta, HBAS_SYS_COUNT);

		s->system = (enum hbas_speaker_system)v;
		return c;
	}
	case HBAS_AI_HEADSET: {
		unsigned v = s->headset;
		bool c = step_enum(&v, delta, HBAS_HS_COUNT);

		s->headset = (enum hbas_headset)v;
		return c;
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

void hbas_audio_factory_eq(char *buf, unsigned len, const struct hbas_audio_settings *s,
			   int bike_cfg, bool engine_running)
{
	enum hbas_audio_output o = hbas_audio_output(s);

	if (o == HBAS_OUT_SPEAKERS && (s->system == HBAS_SYS_CUSTOM || bike_cfg < 0))
		snprintf(buf, len, "%s", HBAS_EQ_BUILTIN_FLAT);
	else
		hbas_eq_profile_name(buf, len, bike_cfg < 0 ? 0 : (unsigned)bike_cfg,
				     engine_running, o);
}
