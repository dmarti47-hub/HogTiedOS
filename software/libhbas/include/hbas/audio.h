/* SPDX-License-Identifier: MIT */
/*
 * Audio settings model. Ranges and step-to-dB tables are the stock
 * calibration from PRE_CAL/cfg/audioMgr.cfg and audioCtrlSvc.cfg
 * (docs/findings/AUDIO.md). Nothing here talks to hardware: the caller
 * passes the resulting dB values to an hbas_audio_backend. How those become
 * SigmaDSP parameter writes is still open (the bench capture is meant to
 * close it).
 *
 * Two speaker systems:
 *  - STOCK: Harley's speakers. Factory EQ profile for the bike model
 *    (compensates for those speakers) and the stock volume curve, which
 *    goes up to +16 dB of digital gain.
 *  - CUSTOM: aftermarket speakers/amp. Factory speaker EQ bypassed (flat),
 *    and the signal never clips: the volume curve is shifted down so its top
 *    step is 0 dB, and the level drops by the largest user-EQ boost. The
 *    external amp provides the loudness.
 * Headsets are the Harley comm system's wired helmet jacks (audioMgr.cfg
 * MIX1_HDST_D / MIX1_HDST_P); when one is selected, media plays there.
 */
#ifndef HBAS_AUDIO_H
#define HBAS_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

#include "hbas/eq.h"

#define HBAS_VOL_STEPS   18   /* 0..17, step 0 = mute */
#define HBAS_FADE_STEPS  17   /* 0..16, 8 = centre */
#define HBAS_VOL_DEFAULT 5    /* audioMgr.cfg default_volumes */

/* audioMgr.cfg mixer outputs (MIX1_*): where media actually plays */
enum hbas_audio_output {
	HBAS_OUT_SPEAKERS, HBAS_OUT_HEADSET_DRIVER, HBAS_OUT_HEADSET_PASSENGER,
	HBAS_OUT_COUNT
};

enum hbas_speaker_system { HBAS_SYS_STOCK, HBAS_SYS_CUSTOM, HBAS_SYS_COUNT };
enum hbas_headset { HBAS_HS_OFF, HBAS_HS_DRIVER, HBAS_HS_PASSENGER, HBAS_HS_COUNT };

struct hbas_audio_settings {
	uint8_t volume[HBAS_OUT_COUNT];   /* volume step per output */
	uint8_t fade;                     /* 0 = rear only .. 8 centre .. 16 front only */
	enum hbas_speaker_system system;
	enum hbas_headset headset;
	bool speed_volume;                /* raise the volume with road speed */
	bool muted;
	uint8_t speakers;                 /* stock speaker count, 2 or 4 (bike model) */
	int8_t speed_boost_db;            /* current speed boost (hbas_speed_boost_update) */
	/* No bass/treble: tone is the 7-band user EQ (hbas/eq.h). */
};

/* The dB values a backend applies. */
struct hbas_audio_db {
	int volume_db;                    /* -100 = silent */
	int fade_front_db, fade_rear_db;
};

void hbas_audio_defaults(struct hbas_audio_settings *s);

/* Where media plays: the selected headset, else the speakers. */
enum hbas_audio_output hbas_audio_output(const struct hbas_audio_settings *s);

/* Stock tables: volume_ctrl_step_curve, fade_response_curve */
int hbas_volume_step_db(unsigned step);
/* Volume curve for a speaker system (CUSTOM: stock curve shifted to top out at 0 dB). */
int hbas_volume_step_db_sys(unsigned step, enum hbas_speaker_system sys);
void hbas_fade_step_db(unsigned step, int *front_db, int *rear_db);
/* Whether the fade control applies with these settings. */
bool hbas_audio_fade_available(const struct hbas_audio_settings *s);

/*
 * Speed volume (stock "AVC", AUDIO.md sec. 7.6): Harley's curves from the
 * factory profiles (tag 0x62) at stock AVC level 2: index = km/h x 0.25,
 * gain linear between breakpoints. Speakers reach about +8 dB at highway
 * speed (+10 dB max), headsets +2.7 dB max.
 */
double hbas_speed_boost_db(bool headset, unsigned kph_x10);
/*
 * Recompute s->speed_boost_db for the current speed (have_speed false =
 * unknown: no boost). Whole dB, with 0.75 dB of hysteresis so a steady speed
 * doesn't flip between steps. Returns true if it changed.
 */
bool hbas_speed_boost_update(struct hbas_audio_settings *s, bool have_speed, unsigned kph_x10);

/*
 * eq may be NULL; in CUSTOM mode its largest boost is taken off the volume.
 * The speed boost is added to the volume (never to mute); CUSTOM still never
 * goes past 0 dB after EQ headroom.
 */
void hbas_audio_to_db(const struct hbas_audio_settings *s, const struct hbas_eq *eq,
		      struct hbas_audio_db *out);

/* Adjust one setting by +/-1 step within its range; returns true if changed. */
enum hbas_audio_item { HBAS_AI_VOLUME, HBAS_AI_FADE, HBAS_AI_SYSTEM, HBAS_AI_HEADSET,
		       HBAS_AI_SPEED_VOLUME, HBAS_AI_COUNT };
bool hbas_audio_adjust(struct hbas_audio_settings *s, enum hbas_audio_item item, int delta);

/* What stock loads when the profile file is missing ("BUILT IN FLAT EQ"). */
#define HBAS_EQ_BUILTIN_FLAT "builtin-flat"

/*
 * Factory EQ profile file the stock eqService would load (getSlotFilename):
 * "<bikecfg>_<ON|OFF>.bin" for speakers, "HS_<ON|OFF>.bin" for headsets.
 * bike_cfg is HD_Configuration_Options value[1] (from IOC diag IDs; not yet
 * decoded on our side).
 */
void hbas_eq_profile_name(char *buf, unsigned len, unsigned bike_cfg, bool engine_running,
			  enum hbas_audio_output out);

/*
 * The factory profile to load for these settings: the headset profile when a
 * headset is selected; for speakers, the bike's profile on STOCK (bike_cfg
 * >= 0) and the built-in flat EQ on CUSTOM or when the bike model is unknown.
 */
void hbas_audio_factory_eq(char *buf, unsigned len, const struct hbas_audio_settings *s,
			   int bike_cfg, bool engine_running);

/* Backend: where settings go. A logging stub exists until the DSP map is known. */
struct hbas_audio_backend {
	void *ctx;
	void (*apply)(void *ctx, const struct hbas_audio_db *db, bool muted,
		      enum hbas_audio_output out);
	void (*load_eq_profile)(void *ctx, const char *name);
	void (*apply_eq)(void *ctx, const struct hbas_eq *eq);   /* user graphic EQ */
};

#endif
