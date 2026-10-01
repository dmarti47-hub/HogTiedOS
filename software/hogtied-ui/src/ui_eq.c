// SPDX-License-Identifier: MIT
/*
 * EQ page: 7-band graphic EQ (libhbas/eq) with a live response curve. It is
 * the only tone control: it takes the biquad set stock uses for bass/treble.
 *
 * Keys: Up/Down choose a preset. Enter starts adjusting: Left/Right pick a
 * band, Up/Down move its slider, Enter or Back finishes. While not adjusting,
 * Left/Right switch pages (ui.c).
 */
#include "ui.h"

#include <math.h>
#include <stdio.h>

#define CURVE_POINTS 60
#define CURVE_RANGE  12                     /* chart shows +/-12 dB */

static struct hbas_eq eq;
static const struct hbas_audio_backend *backend;
static int band;
static bool editing;

static lv_obj_t *chart;
static lv_chart_series_t *series;
static lv_obj_t *slider[HBAS_EQ_BANDS], *val[HBAS_EQ_BANDS], *freq[HBAS_EQ_BANDS];
static lv_obj_t *lbl_preset, *lbl_hint;

static void apply(void)
{
	if (backend && backend->apply_eq)
		backend->apply_eq(backend->ctx, &eq);
	ui_audio_eq_changed();
}

const struct hbas_eq *ui_eq_current(void)
{
	return &eq;
}

static void refresh(void)
{
	/* response curve, log-spaced 20 Hz .. 20 kHz, in tenths of a dB */
	for (int i = 0; i < CURVE_POINTS; i++) {
		double f = 20.0 * pow(1000.0, (double)i / (CURVE_POINTS - 1));
		double db = hbas_eq_response_db(&eq, HBAS_DSP_FS, f);

		lv_chart_set_value_by_id(chart, series, i, (int32_t)lround(db * 10.0));
	}
	lv_chart_refresh(chart);

	for (int b = 0; b < HBAS_EQ_BANDS; b++) {
		bool hi = editing && b == band;

		lv_bar_set_value(slider[b], eq.gain_db[b], LV_ANIM_OFF);
		lv_label_set_text_fmt(val[b], "%+d", eq.gain_db[b]);
		lv_obj_set_style_text_color(val[b], hi ? COL_ACCENT : COL_TEXT, 0);
		lv_obj_set_style_text_color(freq[b], hi ? COL_ACCENT : COL_DIM, 0);
		lv_obj_set_style_bg_color(slider[b], hi ? COL_ACCENT : COL_TEXT, LV_PART_INDICATOR);
		lv_obj_set_style_border_width(slider[b], hi ? 2 : 0, 0);
		lv_obj_set_style_bg_color(slider[b], hi ? lv_color_hex(0x4A2A12) : COL_PANEL_HI, LV_PART_MAIN);
	}
	lv_label_set_text_fmt(lbl_preset, "Preset: %s", hbas_eq_preset_name(eq.preset));
	lv_label_set_text(lbl_hint, editing ? LV_SYMBOL_LEFT LV_SYMBOL_RIGHT " band  "
					      LV_SYMBOL_UP LV_SYMBOL_DOWN " level  OK done"
					    : LV_SYMBOL_UP LV_SYMBOL_DOWN " preset  OK adjust");
}

void ui_eq_build(lv_obj_t *p)
{
	static const char *const names[HBAS_EQ_BANDS] = { "63", "160", "400", "1k", "2.5k", "6.3k", "16k" };
	lv_obj_t *zero;

	hbas_eq_set_preset(&eq, HBAS_EQ_FLAT);

	chart = lv_chart_create(p);
	lv_obj_set_pos(chart, 10, 30);
	lv_obj_set_size(chart, 380, 74);
	lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
	lv_chart_set_point_count(chart, CURVE_POINTS);
	lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, -CURVE_RANGE * 10, CURVE_RANGE * 10);
	lv_chart_set_div_line_count(chart, 3, 0);
	lv_obj_set_style_bg_color(chart, COL_PANEL, 0);
	lv_obj_set_style_border_width(chart, 0, 0);
	lv_obj_set_style_radius(chart, 8, 0);
	lv_obj_set_style_line_color(chart, COL_PANEL_HI, LV_PART_MAIN);
	lv_obj_set_style_size(chart, 0, 0, LV_PART_INDICATOR);   /* no point markers */
	lv_obj_set_style_line_width(chart, 2, LV_PART_ITEMS);
	series = lv_chart_add_series(chart, COL_ACCENT, LV_CHART_AXIS_PRIMARY_Y);

	for (int b = 0; b < HBAS_EQ_BANDS; b++) {
		int x = 16 + b * 54;

		val[b] = ui_label(p, &lv_font_montserrat_14, COL_TEXT, "0");
		lv_obj_set_pos(val[b], x, 108);
		lv_obj_set_width(val[b], 40);
		lv_obj_set_style_text_align(val[b], LV_TEXT_ALIGN_CENTER, 0);

		slider[b] = lv_bar_create(p);
		lv_obj_set_size(slider[b], 12, 66);
		lv_obj_set_pos(slider[b], x + 14, 128);
		lv_bar_set_range(slider[b], -HBAS_EQ_MAX_DB, HBAS_EQ_MAX_DB);
		lv_bar_set_mode(slider[b], LV_BAR_MODE_SYMMETRICAL);
		/* visible track, square-ish ends so small values read as a level */
		lv_obj_set_style_bg_color(slider[b], COL_PANEL_HI, LV_PART_MAIN);
		lv_obj_set_style_bg_opa(slider[b], LV_OPA_COVER, LV_PART_MAIN);
		lv_obj_set_style_radius(slider[b], 3, LV_PART_MAIN);
		lv_obj_set_style_radius(slider[b], 2, LV_PART_INDICATOR);
		lv_obj_set_style_border_color(slider[b], COL_ACCENT, 0);
		lv_obj_set_style_pad_all(slider[b], 0, 0);

		freq[b] = ui_label(p, &lv_font_montserrat_14, COL_DIM, names[b]);
		lv_obj_set_pos(freq[b], x, 198);
		lv_obj_set_width(freq[b], 40);
		lv_obj_set_style_text_align(freq[b], LV_TEXT_ALIGN_CENTER, 0);
	}
	/* 0 dB reference line across the sliders */
	zero = lv_obj_create(p);
	lv_obj_remove_style_all(zero);
	lv_obj_set_pos(zero, 16, 160);
	lv_obj_set_size(zero, (HBAS_EQ_BANDS - 1) * 54 + 40, 1);
	lv_obj_set_style_bg_color(zero, COL_DIM, 0);
	lv_obj_set_style_bg_opa(zero, LV_OPA_50, 0);
	lv_obj_move_background(zero);

	lbl_preset = ui_label(p, &lv_font_montserrat_14, COL_TEXT, "");
	lv_obj_set_pos(lbl_preset, 12, 218);
	lbl_hint = ui_label(p, &lv_font_montserrat_14, COL_DIM, "");
	lv_obj_align(lbl_hint, LV_ALIGN_BOTTOM_RIGHT, -12, -6);
	refresh();
}

void ui_eq_set_backend(const struct hbas_audio_backend *b)
{
	backend = b;
	apply();
}

bool ui_eq_key(enum ui_key key)
{
	bool changed = false;

	if (editing) {
		switch (key) {
		case UI_KEY_LEFT: band = (band + HBAS_EQ_BANDS - 1) % HBAS_EQ_BANDS; break;
		case UI_KEY_RIGHT: band = (band + 1) % HBAS_EQ_BANDS; break;
		case UI_KEY_UP: changed = hbas_eq_adjust(&eq, (unsigned)band, +1); break;
		case UI_KEY_DOWN: changed = hbas_eq_adjust(&eq, (unsigned)band, -1); break;
		case UI_KEY_ENTER: case UI_KEY_BACK: editing = false; break;
		}
	} else {
		switch (key) {
		case UI_KEY_UP:
		case UI_KEY_DOWN: {
			/* cycle the built-in presets (Custom is reached by editing) */
			int n = HBAS_EQ_PRESET_COUNT - 1;
			int p = eq.preset == HBAS_EQ_CUSTOM ? 0 : (int)eq.preset;

			p = (p + (key == UI_KEY_DOWN ? 1 : n - 1)) % n;
			hbas_eq_set_preset(&eq, (enum hbas_eq_preset)p);
			changed = true;
			break;
		}
		case UI_KEY_ENTER: editing = true; break;
		default: return false;          /* Left/Right/Back: page navigation */
		}
	}
	if (changed)
		apply();
	refresh();
	return true;
}
