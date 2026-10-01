// SPDX-License-Identifier: MIT
/*
 * Audio page. Settings, ranges and step-to-dB curves are stock calibration
 * (libhbas/audio, docs/findings/AUDIO.md). The factory EQ profile follows
 * the engine-running flag from the bike, as stock eqService does.
 *
 * Keys: Up/Down pick a row; Enter starts adjusting it, Left/Right change the
 * value, Enter or Back finishes. While not adjusting, Left/Right still switch
 * pages (handled by ui.c).
 */
#include "ui.h"

#include <stdio.h>
#include <string.h>

enum { ROW_H = 30, ROW_Y0 = 32 };

static const char *const row_names[HBAS_AI_COUNT] = {
	"Volume", "Bass", "Treble", "Fade", "Output",
};
static const char *const output_names[HBAS_OUT_COUNT] = {
	"Speakers", "Driver headset", "Passenger headset",
};

static struct hbas_audio_settings audio;
static const struct hbas_audio_backend *backend;
static int bike_cfg = -1;
static int last_engine = -1;               /* -1 = no engine state yet */
static bool engine_running;
static int sel;
static bool editing;

static lv_obj_t *row[HBAS_AI_COUNT], *row_name[HBAS_AI_COUNT];
static lv_obj_t *row_bar[HBAS_AI_COUNT], *row_val[HBAS_AI_COUNT];
static lv_obj_t *lbl_eq, *lbl_dsp, *lbl_hint;

static bool row_visible(int r)
{
	return r != HBAS_AI_FADE || (audio.speakers == 4 && audio.output == HBAS_OUT_SPEAKERS);
}

static void apply(void)
{
	struct hbas_audio_db db;

	hbas_audio_to_db(&audio, &db);
	if (backend && backend->apply)
		backend->apply(backend->ctx, &db, audio.muted, audio.output);
}

static void load_eq(void)
{
	char name[32];

	if (bike_cfg < 0 && audio.output == HBAS_OUT_SPEAKERS)
		/* bike model unknown: stock falls back to its built-in flat EQ when
		 * the profile file is missing (audioCtrlSvc setEq) */
		snprintf(name, sizeof(name), "%s", HBAS_EQ_BUILTIN_FLAT);
	else
		hbas_eq_profile_name(name, sizeof(name), bike_cfg < 0 ? 0 : (unsigned)bike_cfg,
				     engine_running, audio.output);
	if (backend && backend->load_eq_profile)
		backend->load_eq_profile(backend->ctx, name);
}

static void refresh(void)
{
	int y = ROW_Y0;
	char eq[48];

	for (int r = 0; r < HBAS_AI_COUNT; r++) {
		bool vis = row_visible(r), hi = r == sel;

		if (!vis) {
			lv_obj_add_flag(row[r], LV_OBJ_FLAG_HIDDEN);
			continue;
		}
		lv_obj_remove_flag(row[r], LV_OBJ_FLAG_HIDDEN);
		lv_obj_set_y(row[r], y);
		y += ROW_H + 2;
		lv_obj_set_style_bg_color(row[r], hi ? COL_PANEL_HI : COL_PANEL, 0);
		lv_obj_set_style_border_width(row[r], hi ? 2 : 0, 0);
		lv_obj_set_style_border_color(row[r], editing && hi ? COL_ACCENT : COL_DIM, 0);
		lv_obj_set_style_text_color(row_name[r], editing && hi ? COL_ACCENT : COL_TEXT, 0);
	}

	lv_bar_set_value(row_bar[HBAS_AI_VOLUME], audio.volume[audio.output], LV_ANIM_OFF);
	if (audio.volume[audio.output] == 0)
		lv_label_set_text(row_val[HBAS_AI_VOLUME], "mute");
	else
		lv_label_set_text_fmt(row_val[HBAS_AI_VOLUME], "%u  (%d dB)", audio.volume[audio.output],
				      hbas_volume_step_db(audio.volume[audio.output]));
	lv_bar_set_value(row_bar[HBAS_AI_BASS], audio.bass, LV_ANIM_OFF);
	lv_label_set_text_fmt(row_val[HBAS_AI_BASS], "%d dB", hbas_tone_step_db(audio.bass));
	lv_bar_set_value(row_bar[HBAS_AI_TREBLE], audio.treble, LV_ANIM_OFF);
	lv_label_set_text_fmt(row_val[HBAS_AI_TREBLE], "%d dB", hbas_tone_step_db(audio.treble));
	lv_bar_set_value(row_bar[HBAS_AI_FADE], (int)audio.fade - HBAS_FADE_STEPS / 2, LV_ANIM_OFF);
	if (audio.fade == HBAS_FADE_STEPS / 2)
		lv_label_set_text(row_val[HBAS_AI_FADE], "centre");
	else if (audio.fade < HBAS_FADE_STEPS / 2)
		lv_label_set_text_fmt(row_val[HBAS_AI_FADE], "rear %d", HBAS_FADE_STEPS / 2 - audio.fade);
	else
		lv_label_set_text_fmt(row_val[HBAS_AI_FADE], "front %d", audio.fade - HBAS_FADE_STEPS / 2);
	lv_label_set_text(row_val[HBAS_AI_OUTPUT], output_names[audio.output]);

	if (audio.output == HBAS_OUT_SPEAKERS && bike_cfg < 0)
		snprintf(eq, sizeof(eq), "flat (bike model ?)");
	else
		hbas_eq_profile_name(eq, sizeof(eq), bike_cfg < 0 ? 0 : (unsigned)bike_cfg,
				     engine_running, audio.output);
	lv_label_set_text_fmt(lbl_eq, "EQ auto, engine %s: %s",
			      last_engine < 0 ? "?" : engine_running ? "on" : "off", eq);
	lv_label_set_text(lbl_dsp, backend && backend->apply ? "DSP not mapped yet: settings logged only"
					 : "DSP: no audio backend");
	lv_label_set_text(lbl_hint, editing ? LV_SYMBOL_LEFT LV_SYMBOL_RIGHT " adjust  OK done"
					    : LV_SYMBOL_UP LV_SYMBOL_DOWN " select  OK adjust");
}

static void make_row(lv_obj_t *p, int r, bool symmetric, int min, int max)
{
	lv_obj_t *rw = ui_panel(p, 10, ROW_Y0 + r * (ROW_H + 2), 380, ROW_H);

	row[r] = rw;
	row_name[r] = ui_label(rw, &lv_font_montserrat_14, COL_TEXT, row_names[r]);
	lv_obj_align(row_name[r], LV_ALIGN_LEFT_MID, 10, 0);
	row_val[r] = ui_label(rw, &lv_font_montserrat_14, COL_TEXT, "");
	lv_obj_align(row_val[r], LV_ALIGN_RIGHT_MID, -10, 0);
	if (max > min) {
		lv_obj_t *b = lv_bar_create(rw);

		lv_obj_set_size(b, 150, 8);
		lv_obj_align(b, LV_ALIGN_LEFT_MID, 90, 0);
		lv_bar_set_range(b, min, max);
		if (symmetric)
			lv_bar_set_mode(b, LV_BAR_MODE_SYMMETRICAL);
		lv_obj_set_style_bg_color(b, COL_BG, LV_PART_MAIN);
		lv_obj_set_style_bg_color(b, COL_ACCENT, LV_PART_INDICATOR);
		row_bar[r] = b;
	}
}

void ui_audio_build(lv_obj_t *p)
{
	hbas_audio_defaults(&audio);
	make_row(p, HBAS_AI_VOLUME, false, 0, HBAS_VOL_STEPS - 1);
	make_row(p, HBAS_AI_BASS, false, 0, HBAS_TONE_STEPS - 1);
	make_row(p, HBAS_AI_TREBLE, false, 0, HBAS_TONE_STEPS - 1);
	make_row(p, HBAS_AI_FADE, true, -(HBAS_FADE_STEPS / 2), HBAS_FADE_STEPS / 2);
	make_row(p, HBAS_AI_OUTPUT, false, 0, 0);
	lbl_eq = ui_label(p, &lv_font_montserrat_14, COL_DIM, "");
	lv_obj_set_pos(lbl_eq, 12, 196);
	lbl_dsp = ui_label(p, &lv_font_montserrat_14, COL_DIM, "");
	lv_obj_set_pos(lbl_dsp, 12, 214);
	lbl_hint = ui_label(p, &lv_font_montserrat_14, COL_DIM, "");
	lv_obj_align(lbl_hint, LV_ALIGN_TOP_RIGHT, -12, 8);
	refresh();
}

void ui_set_audio_backend(const struct hbas_audio_backend *b)
{
	backend = b;
	apply();
	refresh();
}

void ui_set_speaker_count(uint8_t speakers)
{
	audio.speakers = speakers == 2 ? 2 : 4;
	if (!row_visible(sel))
		sel = HBAS_AI_VOLUME;
	apply();
	refresh();
}

void ui_set_bike_config(int cfg)
{
	bike_cfg = cfg;
	load_eq();
	refresh();
}

void ui_audio_update(const struct hbas_vehicle *v)
{
	int eng = (v->seen & HBAS_SEEN_ENG3) ? v->engine_running : -1;

	if (eng != last_engine) {
		/* stock switches between the *_OFF and *_ON EQ files with the engine */
		last_engine = eng;
		engine_running = eng == 1;
		if (eng >= 0)
			load_eq();
		refresh();
	}
}

static void move_sel(int dir)
{
	int r = sel;

	do {
		r = (r + dir + HBAS_AI_COUNT) % HBAS_AI_COUNT;
	} while (!row_visible(r));
	sel = r;
}

bool ui_audio_key(enum ui_key key)
{
	enum hbas_audio_output before = audio.output;

	if (editing) {
		if (key == UI_KEY_LEFT || key == UI_KEY_RIGHT) {
			if (hbas_audio_adjust(&audio, (enum hbas_audio_item)sel,
					      key == UI_KEY_RIGHT ? +1 : -1)) {
				apply();
				if (audio.output != before)
					load_eq();       /* speakers vs headset profile */
			}
		} else if (key == UI_KEY_ENTER || key == UI_KEY_BACK) {
			editing = false;
		}
		refresh();
		return true;                    /* nothing leaves the page while editing */
	}
	switch (key) {
	case UI_KEY_UP: move_sel(-1); break;
	case UI_KEY_DOWN: move_sel(+1); break;
	case UI_KEY_ENTER: editing = true; break;
	default: return false;              /* Left/Right/Back: page navigation */
	}
	refresh();
	return true;
}
