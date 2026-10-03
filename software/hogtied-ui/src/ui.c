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


#define RPM_MAX 6000      /* display scale only */

static lv_obj_t *pages[UI_PAGE_COUNT];
static lv_obj_t *dots[UI_PAGE_COUNT];
static enum ui_page page;
static void show_page(enum ui_page n);

/* Dash: a retrowave two-dial cluster (tach + speed). */
#define DIAL_R 80                         /* scene radius */
#define DIAL_W (DIAL_R * 2)
static lv_obj_t *sc_tach, *sc_speed, *ndl_tach, *ndl_speed;
static lv_obj_t *lbl_speed, *lbl_speed_unit, *lbl_gear, *lbl_rpm;
static lv_obj_t *lbl_clock, *lbl_ambient;
static lv_obj_t *tt_oil, *tt_fuel, *tt_temp;   /* telltales, shown only when active */
static bool dash_metric;
static uint16_t scene_buf[2][DIAL_W * DIAL_W];
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

static inline uint16_t rgb565(int r, int g, int b)
{
	r = r < 0 ? 0 : r > 255 ? 255 : r;
	g = g < 0 ? 0 : g > 255 ? 255 : g;
	b = b < 0 ? 0 : b > 255 ? 255 : b;
	return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

/* Paint one retrowave dial face (sunset sky, neon sun, perspective grid)
 * into a DIAL_W x DIAL_W RGB565 buffer. Pixels outside the circle are dark
 * (the circular parent clips them). */
static void paint_scene(uint16_t *buf)
{
	const int W = DIAL_W, c = DIAL_R;
	const int hy = (int)(DIAL_R * 1.12);           /* horizon */
	const int gh = W - hy;
	const double sunr = DIAL_R * 0.66;

	for (int y = 0; y < W; y++) {
		for (int x = 0; x < W; x++) {
			int dx = x - c, dy = y - c;
			uint16_t px;

			if (dx * dx + dy * dy > c * c) {
				buf[y * W + x] = rgb565(8, 6, 16);
				continue;
			}
			if (y < hy) {
				/* sky: magenta (top) -> orange (horizon) */
				double t = (double)y / hy;
				int r = (int)(0xE0 + (0xFF - 0xE0) * t);
				int g = (int)(0x1C + (0x8A - 0x1C) * t);
				int b = (int)(0x8E + (0x3D - 0x8E) * t);
				double sdx = x - c, sdy = y - (hy - sunr * 0.15);

				if (sdx * sdx + sdy * sdy < sunr * sunr) {
					/* the sun, with dark scanline gaps */
					double st = (double)y / hy;
					int band = (hy - y) / 5;

					if (!(band & 1) || y > hy - sunr * 0.9) {
						r = (int)(0xFF);
						g = (int)(0xE8 - 0xB0 * st);
						b = (int)(0x6A + 0x30 * st);
					}
				}
				px = rgb565(r, g, b);
			} else {
				/* ground: dark with a cyan perspective grid */
				int gy = y - hy;
				double p = (double)gy / gh;
				int r = 12, g = 7, b = 26;
				/* horizontal lines, spacing grows toward the viewer */
				double lines[] = { 0.10, 0.24, 0.42, 0.65, 0.92 };

				for (unsigned k = 0; k < sizeof(lines) / sizeof(lines[0]); k++)
					if (p > lines[k] - 0.012 && p < lines[k] + 0.012) {
						r = 0x1F; g = 0xE0; b = 0xD6;
					}
				/* vertical lines fanning from the vanishing point */
				if (gy > 0) {
					double xb = c + (double)(x - c) * gh / gy;
					double step = DIAL_R * 0.5;

					double m = fmod(fabs(xb - c), step);
					if (m < 2.0 || m > step - 2.0) {
						r = 0x1F; g = 0xE0; b = 0xD6;
					}
				}
				px = rgb565(r, g, b);
			}
			buf[y * W + x] = px;
		}
	}
}

/* A circular dial face: the painted scene clipped to a circle, with a neon
 * rim. Returns the circular container (the scale is placed over it). */
static lv_obj_t *build_scene(lv_obj_t *parent, int cx, int cy, uint16_t *buf)
{
	lv_obj_t *circ = lv_obj_create(parent);
	lv_obj_t *cv;

	paint_scene(buf);
	lv_obj_remove_style_all(circ);
	lv_obj_set_size(circ, DIAL_W, DIAL_W);
	lv_obj_set_pos(circ, cx - DIAL_R, cy - DIAL_R);
	lv_obj_set_style_radius(circ, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_clip_corner(circ, true, 0);
	lv_obj_set_style_border_width(circ, 0, 0);
	lv_obj_remove_flag(circ, LV_OBJ_FLAG_SCROLLABLE);

	cv = lv_canvas_create(circ);
	lv_canvas_set_buffer(cv, buf, DIAL_W, DIAL_W, LV_COLOR_FORMAT_RGB565);
	lv_obj_center(cv);
	return circ;
}

/* A round tick scale around a dial, with a glowing line needle. */
static lv_obj_t *build_dial(lv_obj_t *parent, int cx, int cy, int min, int max, int ticks,
			    int major_every, int redline, lv_obj_t **needle)
{
	lv_obj_t *scale = lv_scale_create(parent);
	lv_obj_t *n;

	lv_obj_remove_style_all(scale);          /* drop the theme's box border/bg */
	lv_scale_set_mode(scale, LV_SCALE_MODE_ROUND_INNER);
	lv_obj_set_size(scale, DIAL_W + 28, DIAL_W + 28);
	lv_obj_set_pos(scale, cx - DIAL_R - 14, cy - DIAL_R - 14);
	lv_scale_set_rotation(scale, 135);
	lv_scale_set_angle_range(scale, 270);
	lv_scale_set_range(scale, min, max);
	lv_scale_set_total_tick_count(scale, ticks);
	lv_scale_set_major_tick_every(scale, major_every);
	lv_obj_set_style_bg_opa(scale, LV_OPA_TRANSP, LV_PART_MAIN);
	lv_obj_set_style_border_opa(scale, LV_OPA_TRANSP, LV_PART_MAIN);
	lv_obj_set_style_outline_opa(scale, LV_OPA_TRANSP, LV_PART_MAIN);
	lv_obj_set_style_line_opa(scale, LV_OPA_TRANSP, LV_PART_MAIN);
	lv_obj_set_style_pad_all(scale, 0, LV_PART_MAIN);
	/* minor ticks: dim cyan; major ticks + labels: neon */
	lv_obj_set_style_line_color(scale, COL_COOL, LV_PART_ITEMS);
	lv_obj_set_style_line_width(scale, 2, LV_PART_ITEMS);
	lv_obj_set_style_length(scale, 6, LV_PART_ITEMS);
	lv_obj_set_style_line_color(scale, COL_ACCENT, LV_PART_INDICATOR);
	lv_obj_set_style_line_width(scale, 3, LV_PART_INDICATOR);
	lv_obj_set_style_length(scale, 11, LV_PART_INDICATOR);
	lv_obj_set_style_text_color(scale, COL_TEXT, LV_PART_INDICATOR);
	lv_obj_set_style_text_font(scale, FONT_TINY, LV_PART_INDICATOR);

	if (redline > min) {
		lv_scale_section_t *sec = lv_scale_add_section(scale);
		static lv_style_t rs, ri;

		lv_style_init(&rs);
		lv_style_set_line_color(&rs, COL_ALARM);
		lv_style_set_line_width(&rs, 4);
		lv_style_init(&ri);
		lv_style_set_line_color(&ri, COL_ALARM);
		lv_style_set_line_width(&ri, 3);
		lv_scale_section_set_range(sec, redline, max);
		lv_scale_section_set_style(sec, LV_PART_INDICATOR, &rs);
		lv_scale_section_set_style(sec, LV_PART_ITEMS, &ri);
	}

	n = lv_line_create(scale);
	lv_obj_set_style_line_color(n, COL_COOL, 0);
	lv_obj_set_style_line_width(n, 4, 0);
	lv_obj_set_style_line_rounded(n, true, 0);
	lv_scale_set_line_needle_value(scale, n, DIAL_R - 14, min);
	*needle = n;
	return scale;
}

static lv_obj_t *telltale(lv_obj_t *parent, const char *txt, lv_color_t col)
{
	lv_obj_t *l = ui_label(parent, FONT_XS, col, txt);

	lv_obj_set_style_pad_hor(l, 5, 0);
	lv_obj_set_style_pad_ver(l, 1, 0);
	lv_obj_set_style_radius(l, 4, 0);
	lv_obj_set_style_border_color(l, col, 0);
	lv_obj_set_style_border_width(l, 1, 0);
	lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
	return l;
}

static void build_dash(lv_obj_t *p)
{
	lv_obj_t *tt_row, *cap;
	const int cyl = 112, lx = 92, rx = 308;

	/* no page title on the dash; the dials own the screen */
	lv_obj_add_flag(lv_obj_get_child(p, 0), LV_OBJ_FLAG_HIDDEN);

	/* deep purple-black backdrop */
	lv_obj_set_style_bg_color(p, lv_color_hex(0x0A0612), 0);
	lv_obj_set_style_bg_grad_color(p, lv_color_hex(0x16081F), 0);
	lv_obj_set_style_bg_grad_dir(p, LV_GRAD_DIR_VER, 0);
	lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);

	sc_tach = build_scene(p, lx, cyl, scene_buf[0]);
	sc_speed = build_scene(p, rx, cyl, scene_buf[1]);

	/* tach 0-8 (x1000), redline from 6; speed 0-160 mph (relabeled if metric) */
	build_dial(p, lx, cyl, 0, 8, 41, 5, 6, &ndl_tach);
	build_dial(p, rx, cyl, 0, 160, 33, 4, 0, &ndl_speed);

	/* digital readouts low in each dial, over the dark grid for contrast */
	lbl_rpm = ui_label(p, FONT_MD, COL_TEXT, "----");
	lv_obj_set_width(lbl_rpm, 100);
	lv_obj_set_style_text_align(lbl_rpm, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_align(lbl_rpm, LV_ALIGN_CENTER, lx - 200, 42);
	cap = ui_label(p, FONT_TINY, COL_DIM, "RPM");
	lv_obj_set_width(cap, 100);
	lv_obj_set_style_text_align(cap, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_align(cap, LV_ALIGN_CENTER, lx - 200, 66);

	lbl_speed = ui_label(p, FONT_XL, COL_TEXT, "--");
	lv_obj_set_width(lbl_speed, 100);
	lv_obj_set_style_text_align(lbl_speed, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_align(lbl_speed, LV_ALIGN_CENTER, rx - 200, 40);
	lbl_speed_unit = ui_label(p, FONT_TINY, COL_DIM, "mph");
	lv_obj_set_width(lbl_speed_unit, 100);
	lv_obj_set_style_text_align(lbl_speed_unit, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_align(lbl_speed_unit, LV_ALIGN_CENTER, rx - 200, 66);

	/* centre column: clock (top-left), gear (middle) */
	lbl_clock = ui_label(p, FONT_RG, COL_TEXT, "--:--");
	lv_obj_align(lbl_clock, LV_ALIGN_TOP_LEFT, 12, 6);
	cap = ui_label(p, FONT_TINY, COL_COOL, "GEAR");
	lv_obj_align(cap, LV_ALIGN_CENTER, 0, -18);
	lbl_gear = ui_label(p, FONT_XL, COL_ACCENT, "-");
	lv_obj_align(lbl_gear, LV_ALIGN_CENTER, 0, 10);

	/* ambient (real units) and telltales along the bottom */
	lbl_ambient = ui_label(p, FONT_XS, COL_DIM, "--.- C");
	lv_obj_align(lbl_ambient, LV_ALIGN_BOTTOM_LEFT, 10, -8);

	tt_row = lv_obj_create(p);
	lv_obj_remove_style_all(tt_row);
	lv_obj_set_size(tt_row, 220, 24);
	lv_obj_align(tt_row, LV_ALIGN_BOTTOM_MID, 0, -6);
	lv_obj_set_flex_flow(tt_row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(tt_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(tt_row, 6, 0);
	tt_oil = telltale(tt_row, "OIL", COL_ALARM);
	tt_temp = telltale(tt_row, "TEMP", COL_ALARM);
	tt_fuel = telltale(tt_row, "FUEL", COL_WARN);
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

static void dot_clicked(lv_event_t *e)
{
	show_page((enum ui_page)(intptr_t)lv_event_get_user_data(e));
}

/* Touch: a swipe changes page (or moves up/down within one); taps go to the
 * clickable controls on each page. The handlebar keys still work. */
static void screen_gesture_cb(lv_event_t *e)
{
	lv_indev_t *indev = lv_event_get_indev(e);

	if (!indev)
		return;
	switch (lv_indev_get_gesture_dir(indev)) {
	case LV_DIR_LEFT: ui_key(UI_KEY_RIGHT); break;   /* swipe left -> next page */
	case LV_DIR_RIGHT: ui_key(UI_KEY_LEFT); break;
	case LV_DIR_TOP: ui_key(UI_KEY_DOWN); break;      /* swipe up -> down a list */
	case LV_DIR_BOTTOM: ui_key(UI_KEY_UP); break;
	default: break;
	}
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
	pages[UI_PAGE_MAP] = page_create(scr, "MAP");
	pages[UI_PAGE_MEDIA] = page_create(scr, "MEDIA");
	pages[UI_PAGE_AUDIO] = page_create(scr, "AUDIO");
	pages[UI_PAGE_EQ] = page_create(scr, "EQ");
	pages[UI_PAGE_TIRES] = page_create(scr, "TIRES");
	pages[UI_PAGE_GPS] = page_create(scr, "GPS");
	pages[UI_PAGE_SYSTEM] = page_create(scr, "SYSTEM");
	build_dash(pages[UI_PAGE_DASH]);
	ui_map_build(pages[UI_PAGE_MAP]);
	ui_media_build(pages[UI_PAGE_MEDIA]);
	ui_audio_build(pages[UI_PAGE_AUDIO]);
	ui_eq_build(pages[UI_PAGE_EQ]);
	build_tires(pages[UI_PAGE_TIRES]);
	ui_gps_build(pages[UI_PAGE_GPS]);
	build_system(pages[UI_PAGE_SYSTEM]);

	for (int i = 0; i < UI_PAGE_COUNT; i++) {
		dots[i] = lv_obj_create(scr);
		lv_obj_remove_style_all(dots[i]);
		lv_obj_set_size(dots[i], 6, 6);
		lv_obj_set_style_radius(dots[i], LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_bg_opa(dots[i], LV_OPA_COVER, 0);
		lv_obj_align(dots[i], LV_ALIGN_TOP_MID, i * 10 - (UI_PAGE_COUNT - 1) * 5 - 10, 12);
		/* a bigger invisible hit area so a fingertip can land on a 6px dot */
		lv_obj_set_ext_click_area(dots[i], 8);
		lv_obj_add_flag(dots[i], LV_OBJ_FLAG_CLICKABLE);
		lv_obj_add_event_cb(dots[i], dot_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
	}
	lv_obj_add_event_cb(scr, screen_gesture_cb, LV_EVENT_GESTURE, NULL);
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

	/* Speed in the bike's own unit setting (0x5C0), mph until it's known.
	 * The dial relabels itself when the unit setting first arrives. */
	if (v->metric != dash_metric) {
		dash_metric = v->metric;
		lv_scale_set_range(lv_obj_get_parent(ndl_speed), 0, v->metric ? 200 : 160);
		lv_scale_set_total_tick_count(lv_obj_get_parent(ndl_speed), v->metric ? 41 : 33);
		lv_label_set_text(lbl_speed_unit, v->metric ? "km/h" : "mph");
	}
	if (v->metric ? hbas_speed_kph_x10(v, &sp) : hbas_speed_mph_x10(v, &sp)) {
		unsigned mph = (sp + 5) / 10;

		lv_label_set_text_fmt(lbl_speed, "%u", mph);
		lv_scale_set_line_needle_value(lv_obj_get_parent(ndl_speed), ndl_speed, DIAL_R - 14,
					       mph);
	} else {
		lv_label_set_text(lbl_speed, "--");
	}
	ui_gps_set_metric(v->metric);
	ui_map_set_metric(v->metric);

	if (v->seen & HBAS_SEEN_ENG2) {
		int t = v->rpm > RPM_MAX ? 8 : (v->rpm * 8 + RPM_MAX / 2) / RPM_MAX;

		/* Gear value meanings aren't defined in stock code; show the number. */
		lv_label_set_text_fmt(lbl_gear, "%u", v->gear_raw);
		lv_label_set_text_fmt(lbl_rpm, "%u", v->rpm);
		lv_scale_set_line_needle_value(lv_obj_get_parent(ndl_tach), ndl_tach, DIAL_R - 14, t);
	}

	if (v->seen & HBAS_SEEN_INST2) {
		unsigned h = v->clock_h;

		if (!v->clock_24h)
			h = (h % 12) ? h % 12 : 12;
		lv_label_set_text_fmt(lbl_clock, "%u:%02u", h, v->clock_m);
	}
	if (hbas_ambient_c_x10(v, &amb))
		lv_label_set_text_fmt(lbl_ambient, "%d.%d C", amb / 10, (amb < 0 ? -amb : amb) % 10);

	ui_audio_update(v);
	/* telltales appear only when active */
	(v->oil_pressure_warning ? lv_obj_remove_flag : lv_obj_add_flag)(tt_oil, LV_OBJ_FLAG_HIDDEN);
	(v->overtemp ? lv_obj_remove_flag : lv_obj_add_flag)(tt_temp, LV_OBJ_FLAG_HIDDEN);
	(v->low_fuel ? lv_obj_remove_flag : lv_obj_add_flag)(tt_fuel, LV_OBJ_FLAG_HIDDEN);

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
	if (ui_kbd_key(key))
		return;
	if (ui_media_pairing_key(key))
		return;
	if (page == UI_PAGE_MAP && ui_map_key(key))
		return;
	if (page == UI_PAGE_MEDIA && ui_media_key(key))
		return;
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
