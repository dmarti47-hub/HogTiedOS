// SPDX-License-Identifier: MIT
/*
 * Audio page: volume, fade, output (stock speakers or a custom system) and
 * headset. Tone is the 7-band EQ page (ui_eq.c), which replaces stock's
 * bass/treble. Ranges and step-to-dB curves are stock calibration
 * (libhbas/audio, docs/findings/AUDIO.md).
 *
 * Output:
 *  - Stock speakers: Harley's factory EQ for the bike model, stock volume.
 *  - Custom system: factory speaker EQ bypassed (flat), volume tops out at
 *    0 dB and drops by the largest EQ boost, so the amp gets a clean signal.
 * Headset: the Harley comm system's wired helmet jacks; media plays there
 * when one is selected (it has its own volume and the headset EQ profile).
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
	"Volume", "Fade", "Output", "Headset",
};
static const char *const system_names[HBAS_SYS_COUNT] = {
	"Stock speakers", "Custom system",
};
static const char *const headset_names[HBAS_HS_COUNT] = {
	"Off", "Driver", "Passenger",
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
	return r != HBAS_AI_FADE || hbas_audio_fade_available(&audio);
}

static void apply(void)
{
	struct hbas_audio_db db;

	hbas_audio_to_db(&audio, ui_eq_current(), &db);
	if (backend && backend->apply)
		backend->apply(backend->ctx, &db, audio.muted, hbas_audio_output(&audio));
}

static void load_eq(void)
{
	char name[32];

	/* custom system or unknown bike model: built-in flat EQ, as stock falls
	 * back to when a profile file is missing (audioCtrlSvc setEq) */
	hbas_audio_factory_eq(name, sizeof(name), &audio, bike_cfg, engine_running);
	if (backend && backend->load_eq_profile)
		backend->load_eq_profile(backend->ctx, name);
}

static void refresh(void)
{
	struct hbas_audio_db db;
	enum hbas_audio_output out = hbas_audio_output(&audio);
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

	hbas_audio_to_db(&audio, ui_eq_current(), &db);
	lv_bar_set_value(row_bar[HBAS_AI_VOLUME], audio.volume[out], LV_ANIM_OFF);
	if (audio.volume[out] == 0)
		lv_label_set_text(row_val[HBAS_AI_VOLUME], "mute");
	else
		lv_label_set_text_fmt(row_val[HBAS_AI_VOLUME], "%u  (%d dB)", audio.volume[out],
				      db.volume_db);
	lv_bar_set_value(row_bar[HBAS_AI_FADE], (int)audio.fade - HBAS_FADE_STEPS / 2, LV_ANIM_OFF);
	if (audio.fade == HBAS_FADE_STEPS / 2)
		lv_label_set_text(row_val[HBAS_AI_FADE], "centre");
	else if (audio.fade < HBAS_FADE_STEPS / 2)
		lv_label_set_text_fmt(row_val[HBAS_AI_FADE], "rear %d", HBAS_FADE_STEPS / 2 - audio.fade);
	else
		lv_label_set_text_fmt(row_val[HBAS_AI_FADE], "front %d", audio.fade - HBAS_FADE_STEPS / 2);
	lv_label_set_text(row_val[HBAS_AI_SYSTEM], system_names[audio.system]);
	lv_label_set_text(row_val[HBAS_AI_HEADSET], headset_names[audio.headset]);

	hbas_audio_factory_eq(eq, sizeof(eq), &audio, bike_cfg, engine_running);
	if (!strcmp(eq, HBAS_EQ_BUILTIN_FLAT))
		snprintf(eq, sizeof(eq), "%s", audio.system == HBAS_SYS_CUSTOM && out == HBAS_OUT_SPEAKERS
						 ? "flat (custom system)" : "flat (bike model ?)");
	lv_label_set_text_fmt(lbl_eq, "Factory EQ, engine %s: %s",
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
	make_row(p, HBAS_AI_FADE, true, -(HBAS_FADE_STEPS / 2), HBAS_FADE_STEPS / 2);
	make_row(p, HBAS_AI_SYSTEM, false, 0, 0);
	make_row(p, HBAS_AI_HEADSET, false, 0, 0);
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
	ui_eq_set_backend(b);
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

void ui_audio_eq_changed(void)
{
	/* custom system: EQ boosts change the volume headroom */
	if (audio.system == HBAS_SYS_CUSTOM) {
		apply();
		refresh();
	}
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
	if (editing) {
		if (key == UI_KEY_LEFT || key == UI_KEY_RIGHT) {
			if (hbas_audio_adjust(&audio, (enum hbas_audio_item)sel,
					      key == UI_KEY_RIGHT ? +1 : -1)) {
				apply();
				if (sel == HBAS_AI_SYSTEM || sel == HBAS_AI_HEADSET)
					load_eq();       /* factory profile depends on both */
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
