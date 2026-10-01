/* SPDX-License-Identifier: MIT */
/*
 * Audio settings model. Ranges and step-to-dB tables are the stock
 * calibration from PRE_CAL/cfg/audioMgr.cfg and audioCtrlSvc.cfg
 * (docs/findings/AUDIO.md). Nothing here talks to hardware: the caller
 * passes the resulting dB values to an hbas_audio_backend. How those become
 * SigmaDSP parameter writes is still open (the bench capture is meant to
 * close it).
 */
#ifndef HBAS_AUDIO_H
#define HBAS_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

#define HBAS_VOL_STEPS   18   /* 0..17, step 0 = mute */
#define HBAS_TONE_STEPS  17   /* 0..16 */
#define HBAS_FADE_STEPS  17   /* 0..16, 8 = centre */
#define HBAS_VOL_DEFAULT 5    /* audioMgr.cfg default_volumes */

/* audioMgr.cfg mixer outputs (MIX1_*) */
enum hbas_audio_output {
	HBAS_OUT_SPEAKERS, HBAS_OUT_HEADSET_DRIVER, HBAS_OUT_HEADSET_PASSENGER,
	HBAS_OUT_COUNT
};

struct hbas_audio_settings {
	uint8_t volume[HBAS_OUT_COUNT];   /* media volume step per output */
	uint8_t bass, treble;             /* tone steps */
	uint8_t fade;                     /* 0 = front only .. 8 centre .. 16 rear only */
	enum hbas_audio_output output;    /* where media plays */
	bool muted;
	uint8_t speakers;                 /* 2 or 4; fade only applies to 4 */
};

/* The dB values a backend applies, from the stock curves. */
struct hbas_audio_db {
	int volume_db;                    /* -100 = silent */
	int bass_db, treble_db;
	int fade_front_db, fade_rear_db;
};

void hbas_audio_defaults(struct hbas_audio_settings *s);

/* Step tables (stock): volume_ctrl_step_curve, bass/treble/fade_response_curve */
int hbas_volume_step_db(unsigned step);
int hbas_tone_step_db(unsigned step);
void hbas_fade_step_db(unsigned step, int *front_db, int *rear_db);

void hbas_audio_to_db(const struct hbas_audio_settings *s, struct hbas_audio_db *out);

/* Adjust one setting by +/-1 step within its range; returns true if changed. */
enum hbas_audio_item { HBAS_AI_VOLUME, HBAS_AI_BASS, HBAS_AI_TREBLE, HBAS_AI_FADE,
		       HBAS_AI_OUTPUT, HBAS_AI_COUNT };
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

/* Backend: where settings go. A logging stub exists until the DSP map is known. */
struct hbas_audio_backend {
	void *ctx;
	void (*apply)(void *ctx, const struct hbas_audio_db *db, bool muted,
		      enum hbas_audio_output out);
	void (*load_eq_profile)(void *ctx, const char *name);
};

#endif
