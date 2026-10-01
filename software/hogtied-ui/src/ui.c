// SPDX-License-Identifier: MIT
/*
 * HogTiedOS main UI: 400x240, pages switched with LEFT/RIGHT.
 *   Dash   - speed, gear, rpm, warnings, clock, ambient temperature
 *   Audio  - volume, tone, fade, output, automatic EQ (ui_audio.c)
 *   EQ     - 5-band graphic EQ with response curve (ui_eq.c)
 *   Tires  - TPMS values
 *   System - build and data status
 * Values the stock software doesn't convert (gear meaning, tire units) are
 * shown raw rather than guessed.
 */
#include "ui.h"

#include "hbas/bike.h"

#include <stdio.h>


#define RPM_MAX 6000      /* display scale only */

static lv_obj_t *pages[UI_PAGE_COUNT];
static lv_obj_t *dots[UI_PAGE_COUNT];
static enum ui_page page;

/* Dash */
static lv_obj_t *lbl_speed, *lbl_speed_unit, *lbl_gear, *bar_rpm, *lbl_rpm;
static lv_obj_t *lbl_clock, *lbl_ambient, *lbl_ign;
static lv_obj_t *ind_engine, *ind_oil, *ind_fuel, *ind_temp;
/* Tires */
static lv_obj_t *lbl_tire[HBAS_TIRE_COUNT], *lbl_tpms_state;
static lv_obj_t *tire_panel[HBAS_TIRE_COUNT], *lbl_tire_name[HBAS_TIRE_COUNT];
static bool tires_trike, cfg_trike;
static int bike_cfg = -1;
/* System */
static lv_obj_t *lbl_sys;

lv_obj_t *ui_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t col, const char *txt)
{
	lv_obj_t *l = lv_label_create(parent);

	lv_obj_set_style_text_font(l, font, 0);
	lv_obj_set_style_text_color(l, col, 0);
	lv_label_set_text(l, txt);
	return l;
}

lv_obj_t *ui_panel(lv_obj_t *parent, int x, int y, int w, int h)
{
	lv_obj_t *p = lv_obj_create(parent);

	lv_obj_remove_style_all(p);
	lv_obj_set_pos(p, x, y);
	lv_obj_set_size(p, w, h);
	lv_obj_set_style_bg_color(p, COL_PANEL, 0);
	lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(p, 8, 0);
	lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
	return p;
}

static lv_obj_t *indicator(lv_obj_t *parent, const char *txt)
{
	lv_obj_t *l = ui_label(parent, &lv_font_montserrat_14, COL_DIM, txt);

	lv_obj_set_style_pad_hor(l, 6, 0);
	lv_obj_set_style_pad_ver(l, 2, 0);
	lv_obj_set_style_radius(l, 4, 0);
	lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(l, COL_PANEL, 0);
	return l;
}

static void indicator_set(lv_obj_t *ind, bool on, lv_color_t on_col)
{
	lv_obj_set_style_text_color(ind, on ? COL_BG : COL_DIM, 0);
	lv_obj_set_style_bg_color(ind, on ? on_col : COL_PANEL, 0);
}

static lv_obj_t *page_create(lv_obj_t *scr, const char *title)
{
	lv_obj_t *p = lv_obj_create(scr);
	lv_obj_t *t;

	lv_obj_remove_style_all(p);
	lv_obj_set_size(p, UI_WIDTH, UI_HEIGHT);
	lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
	t = ui_label(p, &lv_font_montserrat_14, COL_DIM, title);
	lv_obj_set_pos(t, 12, 8);
	return p;
}

static void build_dash(lv_obj_t *p)
{
	lv_obj_t *speed_panel = ui_panel(p, 10, 32, 250, 138);
	lv_obj_t *gear_panel = ui_panel(p, 270, 32, 120, 138);
	lv_obj_t *row;

	lbl_speed = ui_label(speed_panel, &lv_font_montserrat_48, COL_TEXT, "--");
	lv_obj_align(lbl_speed, LV_ALIGN_CENTER, 0, -10);
	lv_obj_set_style_transform_scale(lbl_speed, 384, 0);     /* 1.5x of 48 px */
	lv_obj_set_style_transform_pivot_x(lbl_speed, LV_PCT(50), 0);
	lv_obj_set_style_transform_pivot_y(lbl_speed, LV_PCT(50), 0);
	lbl_speed_unit = ui_label(speed_panel, &lv_font_montserrat_20, COL_DIM, "mph");
	lv_obj_align(lbl_speed_unit, LV_ALIGN_BOTTOM_MID, 0, -8);

	ui_label(gear_panel, &lv_font_montserrat_14, COL_DIM, "GEAR");
	lv_obj_align(lv_obj_get_child(gear_panel, 0), LV_ALIGN_TOP_MID, 0, 8);
	lbl_gear = ui_label(gear_panel, &lv_font_montserrat_48, COL_ACCENT, "-");
	lv_obj_align(lbl_gear, LV_ALIGN_CENTER, 0, 8);
	lv_obj_set_style_transform_scale(lbl_gear, 384, 0);
	lv_obj_set_style_transform_pivot_x(lbl_gear, LV_PCT(50), 0);
	lv_obj_set_style_transform_pivot_y(lbl_gear, LV_PCT(50), 0);

	bar_rpm = lv_bar_create(p);
	lv_obj_set_pos(bar_rpm, 10, 178);
	lv_obj_set_size(bar_rpm, 290, 12);
	lv_bar_set_range(bar_rpm, 0, RPM_MAX);
	lv_obj_set_style_bg_color(bar_rpm, COL_PANEL, LV_PART_MAIN);
	lv_obj_set_style_bg_color(bar_rpm, COL_ACCENT, LV_PART_INDICATOR);
	lbl_rpm = ui_label(p, &lv_font_montserrat_14, COL_DIM, "---- rpm");
	lv_obj_set_pos(lbl_rpm, 310, 176);

	row = lv_obj_create(p);
	lv_obj_remove_style_all(row);
	lv_obj_set_pos(row, 10, 200);
	lv_obj_set_size(row, 380, 26);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(row, 6, 0);
	ind_engine = indicator(row, "ENGINE");
	ind_oil = indicator(row, "OIL");
	ind_fuel = indicator(row, "FUEL");
	ind_temp = indicator(row, "HOT");
	lbl_ign = ui_label(row, &lv_font_montserrat_14, COL_DIM, "KEY --");
	lbl_ambient = ui_label(row, &lv_font_montserrat_14, COL_TEXT, "--.- C");

	lbl_clock = ui_label(p, &lv_font_montserrat_20, COL_TEXT, "--:--");
	lv_obj_align(lbl_clock, LV_ALIGN_TOP_RIGHT, -12, 4);
}

/*
 * Tire panels. Stock names the fields F, R_or_RR and LR: a motorcycle has
 * front + rear; a trike (BODY_CTRL_DATA2 trike bit) has front, right rear
 * and left rear. The layout follows the bike's own trike flag.
 */
static void tires_layout(bool trike)
{
	if (trike) {
		for (int i = 0; i < HBAS_TIRE_COUNT; i++) {
			lv_obj_set_pos(tire_panel[i], 10 + i * 128, 40);
			lv_obj_set_width(tire_panel[i], 120);
		}
		lv_label_set_text(lbl_tire_name[HBAS_TIRE_REAR], "RIGHT REAR");
		lv_obj_remove_flag(tire_panel[HBAS_TIRE_LEFT_REAR], LV_OBJ_FLAG_HIDDEN);
	} else {
		lv_obj_set_pos(tire_panel[HBAS_TIRE_FRONT], 10, 40);
		lv_obj_set_pos(tire_panel[HBAS_TIRE_REAR], 205, 40);
		lv_obj_set_width(tire_panel[HBAS_TIRE_FRONT], 185);
		lv_obj_set_width(tire_panel[HBAS_TIRE_REAR], 185);
		lv_label_set_text(lbl_tire_name[HBAS_TIRE_REAR], "REAR");
		lv_obj_add_flag(tire_panel[HBAS_TIRE_LEFT_REAR], LV_OBJ_FLAG_HIDDEN);
	}
	tires_trike = trike;
}

static void build_tires(lv_obj_t *p)
{
	static const char *const names[HBAS_TIRE_COUNT] = { "FRONT", "REAR", "LEFT REAR" };

	for (int i = 0; i < HBAS_TIRE_COUNT; i++) {
		lv_obj_t *pn = ui_panel(p, 10 + i * 128, 40, 120, 120);
		lv_obj_t *t = ui_label(pn, &lv_font_montserrat_14, COL_DIM, names[i]);

		tire_panel[i] = pn;
		lbl_tire_name[i] = t;
		lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 8);
		lbl_tire[i] = ui_label(pn, &lv_font_montserrat_20, COL_TEXT, "--");
		lv_obj_set_style_text_align(lbl_tire[i], LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_align(lbl_tire[i], LV_ALIGN_CENTER, 0, 10);
	}
	lbl_tpms_state = ui_label(p, &lv_font_montserrat_14, COL_DIM, "");
	lv_obj_set_pos(lbl_tpms_state, 12, 172);
	lv_obj_t *note = ui_label(p, &lv_font_montserrat_14, COL_DIM,
			       "Raw bike values (P / T): units not yet confirmed");
	lv_obj_set_pos(note, 12, 196);
	tires_layout(false);            /* motorcycle until the bike says trike */
}

static void build_system(lv_obj_t *p)
{
	lbl_sys = ui_label(p, &lv_font_montserrat_14, COL_TEXT, "");
	lv_obj_set_pos(lbl_sys, 12, 36);
}

static void show_page(enum ui_page n)
{
	page = n;
	for (int i = 0; i < UI_PAGE_COUNT; i++) {
		if (i == (int)n)
			lv_obj_remove_flag(pages[i], LV_OBJ_FLAG_HIDDEN);
		else
			lv_obj_add_flag(pages[i], LV_OBJ_FLAG_HIDDEN);
		lv_obj_set_style_bg_color(dots[i], i == (int)n ? COL_ACCENT : COL_DIM, 0);
	}
}

void ui_create(void)
{
	lv_obj_t *scr = lv_screen_active();

	lv_obj_set_style_bg_color(scr, COL_BG, 0);
	lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
	lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

	pages[UI_PAGE_DASH] = page_create(scr, "HOGTIED");
	pages[UI_PAGE_AUDIO] = page_create(scr, "AUDIO");
	pages[UI_PAGE_EQ] = page_create(scr, "EQ");
	pages[UI_PAGE_TIRES] = page_create(scr, "TIRES");
	pages[UI_PAGE_SYSTEM] = page_create(scr, "SYSTEM");
	build_dash(pages[UI_PAGE_DASH]);
	ui_audio_build(pages[UI_PAGE_AUDIO]);
	ui_eq_build(pages[UI_PAGE_EQ]);
	build_tires(pages[UI_PAGE_TIRES]);
	build_system(pages[UI_PAGE_SYSTEM]);

	for (int i = 0; i < UI_PAGE_COUNT; i++) {
		dots[i] = lv_obj_create(scr);
		lv_obj_remove_style_all(dots[i]);
		lv_obj_set_size(dots[i], 6, 6);
		lv_obj_set_style_radius(dots[i], LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_bg_opa(dots[i], LV_OPA_COVER, 0);
		lv_obj_align(dots[i], LV_ALIGN_TOP_MID, i * 12 - (UI_PAGE_COUNT - 1) * 6, 12);
	}
	show_page(UI_PAGE_DASH);
}

static void fmt_raw_u8(char *buf, size_t n, uint8_t v)
{
	if (v == HBAS_U8_NOVAL)
		snprintf(buf, n, "--");
	else if (v == HBAS_U8_ERROR)
		snprintf(buf, n, "ERR");
	else
		snprintf(buf, n, "%u", v);
}

static void fmt_tire(char *buf, size_t n, uint8_t p, uint8_t t)
{
	char ps[8], ts[8];

	fmt_raw_u8(ps, sizeof(ps), p);
	fmt_raw_u8(ts, sizeof(ts), t);
	snprintf(buf, n, "P %s\nT %s", ps, ts);
}

void ui_update(const struct hbas_vehicle *v)
{
	char buf[200], bike[48];
	unsigned sp;
	int amb;

	/* Speed in the bike's own unit setting (0x5C0), mph until it's known */
	if (v->metric ? hbas_speed_kph_x10(v, &sp) : hbas_speed_mph_x10(v, &sp))
		lv_label_set_text_fmt(lbl_speed, "%u", (sp + 5) / 10);
	else
		lv_label_set_text(lbl_speed, "--");
	lv_label_set_text(lbl_speed_unit, v->metric ? "km/h" : "mph");

	if (v->seen & HBAS_SEEN_ENG2) {
		/* Gear value meanings aren't defined in stock code; show the number. */
		lv_label_set_text_fmt(lbl_gear, "%u", v->gear_raw);
		lv_bar_set_value(bar_rpm, v->rpm > RPM_MAX ? RPM_MAX : v->rpm, LV_ANIM_OFF);
		lv_label_set_text_fmt(lbl_rpm, "%u rpm", v->rpm);
	}

	if (v->seen & HBAS_SEEN_INST2) {
		unsigned h = v->clock_h;

		if (!v->clock_24h)
			h = (h % 12) ? h % 12 : 12;
		lv_label_set_text_fmt(lbl_clock, "%u:%02u", h, v->clock_m);
	}
	if (hbas_ambient_c_x10(v, &amb))
		lv_label_set_text_fmt(lbl_ambient, "%d.%d C", amb / 10, (amb < 0 ? -amb : amb) % 10);
	lv_label_set_text_fmt(lbl_ign, "KEY %s", hbas_ignition_name(v->ignition));

	ui_audio_update(v);
	indicator_set(ind_engine, v->engine_running, COL_OK);
	indicator_set(ind_oil, v->oil_pressure_warning, COL_ALARM);
	indicator_set(ind_fuel, v->low_fuel, COL_WARN);
	indicator_set(ind_temp, v->overtemp, COL_ALARM);

	/* trike: the bike's own TPMS flag, or a trike configuration */
	if ((((v->seen & HBAS_SEEN_BODY2) && v->tpms.trike) || cfg_trike) != tires_trike)
		tires_layout(!tires_trike);
	for (int i = 0; i < HBAS_TIRE_COUNT; i++) {
		fmt_tire(buf, sizeof(buf), v->tpms.pressure_raw[i], v->tpms.temp_raw[i]);
		lv_label_set_text(lbl_tire[i], buf);
		lv_obj_set_style_text_color(lbl_tire[i], v->tpms.low_pressure[i] ? COL_ALARM : COL_TEXT, 0);
	}
	lv_label_set_text(lbl_tpms_state, !(v->seen & HBAS_SEEN_BODY2) ? "TPMS: no data" :
			  v->tpms.enabled ? "TPMS: enabled" : "TPMS: not enabled");

	{
		struct hbas_bike_info b;

		hbas_bike_info((uint8_t)(bike_cfg < 0 ? 0 : bike_cfg), &b);
		snprintf(bike, sizeof(bike), bike_cfg < 0 ? "not detected yet" : "%s (config %d)",
			 b.name, bike_cfg);
	}
	snprintf(buf, sizeof(buf),
		 "HogTiedOS\n\n"
		 "Bike: %s\n"
		 "Bike data seen: 0x%02x\n"
		 "Total distance: %lu m\n"
		 "Display: 400 x 240 RGB565",
		 bike, (unsigned)v->seen, (unsigned long)v->total_distance_m);
	lv_label_set_text(lbl_sys, buf);
}

void ui_set_bike(int cfg)
{
	struct hbas_bike_info b;

	if (cfg > 255)
		cfg = -1;
	bike_cfg = cfg;
	hbas_bike_info((uint8_t)(cfg < 0 ? 0 : cfg), &b);
	cfg_trike = cfg >= 0 && b.trike;
	if (cfg >= 0)
		ui_set_speaker_count(b.speakers);
	ui_set_bike_config(cfg);                /* factory EQ + Harley preset */
}

void ui_key(enum ui_key key)
{
	if (page == UI_PAGE_AUDIO && ui_audio_key(key))
		return;
	if (page == UI_PAGE_EQ && ui_eq_key(key))
		return;
	if (key == UI_KEY_RIGHT)
		show_page((page + 1) % UI_PAGE_COUNT);
	else if (key == UI_KEY_LEFT)
		show_page((page + UI_PAGE_COUNT - 1) % UI_PAGE_COUNT);
	else if (key == UI_KEY_BACK)
		show_page(UI_PAGE_DASH);
}

enum ui_page ui_current_page(void)
{
	return page;
}
