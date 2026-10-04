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

#include <math.h>
#include <stdio.h>


static lv_obj_t *pages[UI_PAGE_COUNT];
static enum ui_page page;
static void show_page(enum ui_page n);

/* where the back / Home key goes from each page (one level up) */
static const enum ui_page ui_parent[UI_PAGE_COUNT] = {
	[UI_PAGE_HOME] = UI_PAGE_HOME,
	[UI_PAGE_NAV] = UI_PAGE_HOME,
	[UI_PAGE_MEDIA] = UI_PAGE_HOME,
	[UI_PAGE_INFO] = UI_PAGE_HOME,
	[UI_PAGE_SETTINGS] = UI_PAGE_HOME,
	[UI_PAGE_AUDIO] = UI_PAGE_SETTINGS,
	[UI_PAGE_EQ] = UI_PAGE_AUDIO,
	[UI_PAGE_BLUETOOTH] = UI_PAGE_SETTINGS,
};

/* Home (audio): media box + nav buttons + status icons */
static lv_obj_t *lbl_home_clock, *lbl_home_track, *lbl_home_src;
static lv_obj_t *st_temp, *st_tire, *st_fuel, *st_oil;
#define HOME_BTNS 4
static lv_obj_t *home_btns[HOME_BTNS];
static const enum ui_page home_targets[HOME_BTNS] = {
	UI_PAGE_NAV, UI_PAGE_MEDIA, UI_PAGE_INFO, UI_PAGE_SETTINGS,
};
static int home_sel;

static void home_highlight(void)
{
	for (int i = 0; i < HOME_BTNS; i++) {
		bool on = i == home_sel;

		lv_obj_set_style_border_width(home_btns[i], on ? 2 : 1, 0);
		lv_obj_set_style_border_color(home_btns[i], on ? COL_ACCENT : COL_LINE, 0);
		lv_obj_set_style_bg_color(home_btns[i], on ? COL_SURFACE_HI : COL_SURFACE, 0);
		lv_obj_set_style_text_color(lv_obj_get_child(home_btns[i], 0),
					    on ? COL_ACCENT : COL_TEXT, 0);
	}
}

bool ui_home_key(enum ui_key key)
{
	switch (key) {
	case UI_KEY_UP: case UI_KEY_LEFT: home_sel = (home_sel + HOME_BTNS - 1) % HOME_BTNS; break;
	case UI_KEY_DOWN: case UI_KEY_RIGHT: home_sel = (home_sel + 1) % HOME_BTNS; break;
	case UI_KEY_ENTER: show_page(home_targets[home_sel]); return true;
	default: return false;
	}
	home_highlight();
	return true;
}
/* Info: vehicle sensor outputs */
static lv_obj_t *iv_ign, *iv_gear, *iv_engine, *iv_rpm, *iv_speed, *iv_ambient;
static lv_obj_t *iv_oil, *iv_fuel, *iv_etemp, *iv_ctemp;
/* Tires (a card on the Info page) */
static lv_obj_t *lbl_tire[HBAS_TIRE_COUNT], *lbl_tpms_state;
static lv_obj_t *tire_panel[HBAS_TIRE_COUNT], *lbl_tire_name[HBAS_TIRE_COUNT];
static bool tires_trike, cfg_trike;
static int bike_cfg = -1;
/* Settings: about text */
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

static lv_obj_t *page_create(lv_obj_t *scr, const char *title)
{
	lv_obj_t *p = lv_obj_create(scr);
	lv_obj_t *t;

	lv_obj_remove_style_all(p);
	lv_obj_set_size(p, UI_WIDTH, UI_HEIGHT);
	lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
	t = ui_label(p, FONT_SM, COL_DIM, title);
	lv_obj_set_pos(t, 58, 8);           /* clear of the floating Home button */
	return p;
}

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

static void nav_cb(lv_event_t *e);

/* a small back button (top-left) that goes up one level */
static lv_obj_t *add_back(lv_obj_t *page, enum ui_page to)
{
	lv_obj_t *b = ui_card(page, 6, 6, 42, 28);
	lv_obj_t *l = ui_label(b, FONT_RG, COL_ACCENT, LV_SYMBOL_LEFT);

	lv_obj_center(l);
	lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_style_bg_color(b, COL_SURFACE_HI, LV_STATE_PRESSED);
	lv_obj_add_event_cb(b, nav_cb, LV_EVENT_CLICKED, (void *)(intptr_t)to);
	return b;
}

/* nav button callback: go to the page passed as user data */
static void nav_cb(lv_event_t *e)
{
	show_page((enum ui_page)(intptr_t)lv_event_get_user_data(e));
}

static lv_obj_t *nav_button(lv_obj_t *parent, int x, int y, int w, int h, const char *label,
			    enum ui_page to)
{
	lv_obj_t *c = ui_card(parent, x, y, w, h);
	lv_obj_t *l = ui_label(c, FONT_RG, COL_TEXT, label);

	lv_obj_center(l);
	lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_style_bg_color(c, COL_SURFACE_HI, LV_STATE_PRESSED);
	lv_obj_set_style_border_color(c, COL_ACCENT, LV_STATE_PRESSED);
	lv_obj_add_event_cb(c, nav_cb, LV_EVENT_CLICKED, (void *)(intptr_t)to);
	return c;
}

/* ---- Home (audio) ------------------------------------------------------- */

/* a small status icon tile; recoloured by status_set (gray/yellow/red) */
static lv_obj_t *status_tile(lv_obj_t *p, int x, int y, const char *label)
{
	lv_obj_t *t = ui_card(p, x, y, 44, 38);
	lv_obj_t *l = ui_label(t, FONT_TINY, COL_DIM, label);

	lv_obj_center(l);
	lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_border_width(t, 2, 0);
	lv_obj_set_style_border_color(t, COL_DIM, 0);
	return t;
}

static void status_set(lv_obj_t *tile, int level)
{
	lv_color_t c = level >= 2 ? COL_ALARM : level == 1 ? COL_WARN : COL_DIM;

	lv_obj_set_style_border_color(tile, c, 0);
	lv_obj_set_style_text_color(lv_obj_get_child(tile, 0), c, 0);
}

static void build_home(lv_obj_t *p)
{
	lv_obj_t *media, *cap;

	lv_obj_set_style_bg_color(p, lv_color_hex(0x0A0612), 0);
	lv_obj_set_style_bg_grad_color(p, lv_color_hex(0x1A0A26), 0);
	lv_obj_set_style_bg_grad_dir(p, LV_GRAD_DIR_VER, 0);
	lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);

	/* media box (left) */
	media = ui_card(p, 6, 6, 264, 150);
	cap = ui_label(media, FONT_TINY, COL_ACCENT, "NOW PLAYING");
	lv_obj_set_pos(cap, 12, 10);
	lbl_home_clock = ui_label(media, FONT_SM, COL_DIM, "--:--");
	lv_obj_align(lbl_home_clock, LV_ALIGN_TOP_RIGHT, -12, 8);
	lbl_home_track = ui_label(media, FONT_LG, COL_TEXT, "Nothing playing");
	lv_obj_set_width(lbl_home_track, 240);
	lv_label_set_long_mode(lbl_home_track, LV_LABEL_LONG_DOT);
	lv_obj_align(lbl_home_track, LV_ALIGN_LEFT_MID, 12, -6);
	lbl_home_src = ui_label(media, FONT_SM, COL_DIM, "");
	lv_obj_set_width(lbl_home_src, 240);
	lv_label_set_long_mode(lbl_home_src, LV_LABEL_LONG_DOT);
	lv_obj_align(lbl_home_src, LV_ALIGN_LEFT_MID, 12, 26);

	/* status icons under the media box */
	st_temp = status_tile(p, 6, 166, "TEMP");
	st_tire = status_tile(p, 56, 166, "TIRES");
	st_fuel = status_tile(p, 106, 166, "FUEL");
	st_oil = status_tile(p, 156, 166, "OIL");

	/* navigation buttons (vertical, right) */
	home_btns[0] = nav_button(p, 278, 6, 116, 52, "Navigation", UI_PAGE_NAV);
	home_btns[1] = nav_button(p, 278, 64, 116, 52, "Music", UI_PAGE_MEDIA);
	home_btns[2] = nav_button(p, 278, 122, 116, 52, "Info", UI_PAGE_INFO);
	home_btns[3] = nav_button(p, 278, 180, 116, 52, "Settings", UI_PAGE_SETTINGS);
	home_highlight();
}

/* ---- Info (sensor outputs) ---------------------------------------------- */

static lv_obj_t *info_row(lv_obj_t *card, int row, const char *name)
{
	lv_obj_t *n = ui_label(card, FONT_XS, COL_DIM, name);
	lv_obj_t *val = ui_label(card, FONT_SM, COL_TEXT, "--");
	int y = 26 + row / 2 * 23;
	int x = row & 1 ? 200 : 10;

	lv_obj_set_pos(n, x, y + 1);
	lv_obj_set_pos(val, x + 86, y - 1);
	return val;
}

static void build_info(lv_obj_t *p)
{
	lv_obj_t *veh, *tw, *gw, *cap;

	lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(p, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(p, 8, 0);
	lv_obj_set_style_pad_top(p, 30, 0);
	lv_obj_set_style_pad_bottom(p, 12, 0);
	lv_obj_add_flag(p, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_scroll_dir(p, LV_DIR_VER);

	/* vehicle sensors */
	veh = ui_card(p, 0, 0, 384, 172);
	cap = ui_label(veh, FONT_XS, COL_ACCENT, "VEHICLE");
	lv_obj_set_pos(cap, 10, 8);
	iv_ign = info_row(veh, 0, "Key");
	iv_engine = info_row(veh, 1, "Engine");
	iv_gear = info_row(veh, 2, "Gear");
	iv_rpm = info_row(veh, 3, "RPM");
	iv_speed = info_row(veh, 4, "Speed");
	iv_ambient = info_row(veh, 5, "Ambient");
	iv_oil = info_row(veh, 6, "Oil");
	iv_fuel = info_row(veh, 7, "Fuel");
	iv_etemp = info_row(veh, 8, "Eng T");
	iv_ctemp = info_row(veh, 9, "Clt T");
	cap = ui_label(veh, FONT_TINY, COL_DIM, "temps are raw values (units unverified)");
	lv_obj_align(cap, LV_ALIGN_BOTTOM_LEFT, 10, -6);

	/* tires */
	tw = ui_card(p, 0, 0, 384, 210);
	cap = ui_label(tw, FONT_XS, COL_ACCENT, "TIRES");
	lv_obj_set_pos(cap, 10, 8);
	build_tires(tw);

	/* GPS / satellites */
	gw = ui_card(p, 0, 0, 384, 236);
	cap = ui_label(gw, FONT_XS, COL_ACCENT, "GPS");
	lv_obj_set_pos(cap, 10, 8);
	ui_gps_build(gw);
}

/* ---- Settings ----------------------------------------------------------- */

static void build_settings(lv_obj_t *p)
{
	lv_obj_t *cap;

	cap = ui_label(p, FONT_RG, COL_TEXT, "Settings");
	lv_obj_align(cap, LV_ALIGN_TOP_LEFT, 58, 8);

	nav_button(p, 8, 46, 186, 54, "Audio & EQ", UI_PAGE_AUDIO);
	nav_button(p, 206, 46, 186, 54, "Bluetooth", UI_PAGE_BLUETOOTH);
	ui_button(p, 8, 108, 186, 54, "Theme", NULL, NULL);   /* placeholder */

	lbl_sys = ui_label(p, FONT_TINY, COL_DIM, "");
	lv_obj_align(lbl_sys, LV_ALIGN_BOTTOM_LEFT, 12, -8);
}

/* ---- navigation / chrome ----------------------------------------------- */

/* Touch: swipe right returns to Home (like the bezel/handlebar Home button);
 * swipe up/down scroll a list. Handlebar keys still work. */
static void screen_gesture_cb(lv_event_t *e)
{
	lv_indev_t *indev = lv_event_get_indev(e);

	if (!indev)
		return;
	switch (lv_indev_get_gesture_dir(indev)) {
	case LV_DIR_RIGHT: show_page(UI_PAGE_HOME); break;
	case LV_DIR_TOP: ui_key(UI_KEY_DOWN); break;
	case LV_DIR_BOTTOM: ui_key(UI_KEY_UP); break;
	default: break;
	}
}

static void show_page(enum ui_page n)
{
	page = n;
	for (int i = 0; i < UI_PAGE_COUNT; i++)
		(i == (int)n ? lv_obj_remove_flag : lv_obj_add_flag)(pages[i], LV_OBJ_FLAG_HIDDEN);
}

void ui_create(void)
{
	lv_obj_t *scr = lv_screen_active();

	lv_obj_set_style_bg_color(scr, COL_BG, 0);
	lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
	lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

	pages[UI_PAGE_HOME] = page_create(scr, "");
	pages[UI_PAGE_NAV] = page_create(scr, "");
	pages[UI_PAGE_MEDIA] = page_create(scr, "MUSIC");
	pages[UI_PAGE_INFO] = page_create(scr, "");
	pages[UI_PAGE_SETTINGS] = page_create(scr, "");
	pages[UI_PAGE_AUDIO] = page_create(scr, "AUDIO");
	pages[UI_PAGE_EQ] = page_create(scr, "EQ");
	pages[UI_PAGE_BLUETOOTH] = page_create(scr, "BLUETOOTH");

	build_home(pages[UI_PAGE_HOME]);
	ui_map_build(pages[UI_PAGE_NAV]);
	ui_media_build(pages[UI_PAGE_MEDIA]);
	build_info(pages[UI_PAGE_INFO]);
	build_settings(pages[UI_PAGE_SETTINGS]);
	ui_audio_build(pages[UI_PAGE_AUDIO]);
	ui_eq_build(pages[UI_PAGE_EQ]);
	ui_bt_build(pages[UI_PAGE_BLUETOOTH]);
	/* Equalizer lives inside Audio settings */
	nav_button(pages[UI_PAGE_AUDIO], 300, 206, 92, 30, "EQ", UI_PAGE_EQ);
	/* back buttons through the settings hierarchy */
	add_back(pages[UI_PAGE_AUDIO], UI_PAGE_SETTINGS);
	add_back(pages[UI_PAGE_EQ], UI_PAGE_AUDIO);
	add_back(pages[UI_PAGE_BLUETOOTH], UI_PAGE_SETTINGS);

	lv_obj_add_event_cb(scr, screen_gesture_cb, LV_EVENT_GESTURE, NULL);
	show_page(UI_PAGE_HOME);
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

void ui_home_now_playing(const char *track, const char *artist, bool connected)
{
	lv_label_set_text(lbl_home_track, track && track[0] ? track :
			  connected ? "Nothing playing" : "No phone connected");
	lv_label_set_text(lbl_home_src, artist ? artist : "");
}

static void set_val(lv_obj_t *l, const char *s, bool alarm)
{
	lv_label_set_text(l, s);
	lv_obj_set_style_text_color(l, alarm ? COL_ALARM : COL_TEXT, 0);
}

void ui_update(const struct hbas_vehicle *v)
{
	char buf[200], bike[48], tmp[24];
	unsigned sp;
	int amb;

	ui_gps_set_metric(v->metric);
	ui_map_set_metric(v->metric);

	/* ---- Info page: vehicle sensor outputs ---- */
	set_val(iv_ign, hbas_ignition_name(v->ignition), false);
	if (v->seen & HBAS_SEEN_ENG2) {
		set_val(iv_engine, v->engine_running ? "running" : "stopped", false);
		snprintf(tmp, sizeof(tmp), "%u", v->gear_raw);
		set_val(iv_gear, tmp, false);
		snprintf(tmp, sizeof(tmp), "%u", v->rpm);
		set_val(iv_rpm, tmp, false);
		snprintf(tmp, sizeof(tmp), "%u (raw)", v->engine_temp_raw);
		set_val(iv_etemp, tmp, v->overtemp);
		snprintf(tmp, sizeof(tmp), "%u (raw)", v->coolant_temp_raw);
		set_val(iv_ctemp, tmp, v->overtemp);
	}
	if (v->metric ? hbas_speed_kph_x10(v, &sp) : hbas_speed_mph_x10(v, &sp)) {
		snprintf(tmp, sizeof(tmp), "%u %s", (sp + 5) / 10, v->metric ? "km/h" : "mph");
		set_val(iv_speed, tmp, false);
	}
	if (hbas_ambient_c_x10(v, &amb)) {
		snprintf(tmp, sizeof(tmp), "%d.%d C", amb / 10, (amb < 0 ? -amb : amb) % 10);
		set_val(iv_ambient, tmp, false);
	}
	snprintf(tmp, sizeof(tmp), "%u kPa%s", v->oil_pressure_bcm * 2,
		 v->oil_pressure_warning ? " LOW" : "");
	set_val(iv_oil, tmp, v->oil_pressure_warning);
	set_val(iv_fuel, v->low_fuel ? "LOW" : "ok", v->low_fuel);

	/* ---- Home page: status icons (gray normal, yellow warn, red alarm) ---- */
	{
		int tire = 0;

		for (int i = 0; i < HBAS_TIRE_COUNT; i++) {
			if (v->tpms.low_pressure[i])
				tire = 2;
			else if (tire < 1 && v->tpms.low_battery[i])
				tire = 1;
		}
		if (tire < 1 && v->tpms.telltale)
			tire = 1;
		status_set(st_temp, v->overtemp ? 2 : 0);
		status_set(st_tire, tire);
		status_set(st_fuel, v->low_fuel ? 1 : 0);
		status_set(st_oil, v->oil_pressure_warning ? 2 : 0);
	}

	/* ---- Home page: clock + now playing ---- */
	if (v->seen & HBAS_SEEN_INST2) {
		unsigned h = v->clock_h;

		if (!v->clock_24h)
			h = (h % 12) ? h % 12 : 12;
		lv_label_set_text_fmt(lbl_home_clock, "%u:%02u", h, v->clock_m);
	}

	ui_audio_update(v);

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
	snprintf(buf, sizeof(buf), "HogTiedOS   Bike: %s   seen 0x%02x", bike, (unsigned)v->seen);
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
	if (ui_kbd_key(key))
		return;
	if (ui_media_pairing_key(key))
		return;
	if (page == UI_PAGE_NAV && ui_map_key(key))
		return;
	if (page == UI_PAGE_MEDIA && ui_media_key(key))
		return;
	if (page == UI_PAGE_AUDIO && ui_audio_key(key))
		return;
	if (page == UI_PAGE_EQ && ui_eq_key(key))
		return;
	if (page == UI_PAGE_BLUETOOTH && ui_bt_key(key))
		return;
	if (page == UI_PAGE_HOME && ui_home_key(key))
		return;
	/* the handlebar back / Home key goes up one level (Home from the top) */
	if (key == UI_KEY_BACK)
		show_page(ui_parent[page]);
}

void ui_goto(enum ui_page p)
{
	show_page(p);
}

enum ui_page ui_current_page(void)
{
	return page;
}
