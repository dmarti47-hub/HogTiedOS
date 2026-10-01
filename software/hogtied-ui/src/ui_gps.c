// SPDX-License-Identifier: MIT
/*
 * GPS page: what the u-blox receiver reports through hbas-gpsd. Ground
 * speed and heading, position, altitude, fix quality, UTC time, and signal
 * strength per satellite. Speed and altitude follow the bike's own unit
 * setting, like the dash. No keys of its own.
 */
#include "ui.h"

#include <math.h>
#include <stdio.h>
#include <time.h>

#define BARS 9                         /* satellites shown (strongest first) */

static struct hbas_gps_view gps;
static bool metric;

static lv_obj_t *lbl_status, *lbl_speed, *lbl_speed_unit, *lbl_heading;
static lv_obj_t *lbl_pos, *lbl_detail, *bar[BARS], *bar_lbl[BARS];

static void fmt_coord(char *buf, size_t n, double v, char pos, char neg)
{
	snprintf(buf, n, "%.5f\xc2\xb0 %c", fabs(v), v >= 0 ? pos : neg);  /* degree sign */
}

static void refresh(void)
{
	char lat[24], lon[24], alt[24], tm[16];
	bool have = gps.daemon && gps.link;

	if (!gps.daemon)
		lv_label_set_text(lbl_status, "GPS service not running");
	else if (!gps.link)
		lv_label_set_text(lbl_status, "No data from the GPS receiver");
	else if (!gps.valid)
		lv_label_set_text_fmt(lbl_status, "Searching... %u satellites in view", gps.view);
	else
		lv_label_set_text_fmt(lbl_status, "%s fix, %u satellites%s",
				      gps.fix == HBAS_FIX_3D ? "3D" : "2D", gps.used,
				      gps.quality == 2 ? " (DGPS)" : "");
	lv_obj_set_style_text_color(lbl_status, have && gps.valid ? COL_OK : COL_WARN, 0);

	if (have && gps.valid) {
		double sp = metric ? gps.speed_kmh : gps.speed_kmh / 1.609344;

		lv_label_set_text_fmt(lbl_speed, "%d", (int)lround(sp));
		if (gps.course >= 0 && gps.speed_kmh >= 3.0)    /* heading is noise when stopped */
			lv_label_set_text_fmt(lbl_heading, "%s  %d\xc2\xb0",
					      hbas_compass_point(gps.course), (int)lround(gps.course) % 360);
		else
			lv_label_set_text(lbl_heading, "--");
		fmt_coord(lat, sizeof(lat), gps.lat, 'N', 'S');
		fmt_coord(lon, sizeof(lon), gps.lon, 'E', 'W');
		lv_label_set_text_fmt(lbl_pos, "%s\n%s", lat, lon);
	} else {
		lv_label_set_text(lbl_speed, "--");
		lv_label_set_text(lbl_heading, "--");
		lv_label_set_text(lbl_pos, "");
	}
	lv_label_set_text(lbl_speed_unit, metric ? "km/h" : "mph");

	if (have && gps.valid && gps.have_alt)
		snprintf(alt, sizeof(alt), metric ? "%.0f m" : "%.0f ft",
			 metric ? gps.alt_m : gps.alt_m / 0.3048);
	else
		snprintf(alt, sizeof(alt), "--");
	if (gps.time) {
		time_t t = (time_t)gps.time;
		struct tm u;

		gmtime_r(&t, &u);
		snprintf(tm, sizeof(tm), "%02d:%02d:%02d", u.tm_hour, u.tm_min, u.tm_sec);
	} else {
		snprintf(tm, sizeof(tm), "--");
	}
	{
		char d[80];

		/* LVGL's own printf has no floats: format here */
		snprintf(d, sizeof(d), "Altitude %s\nHDOP %.1f\nUTC %s", alt, have ? gps.hdop : 0.0, tm);
		lv_label_set_text(lbl_detail, d);
	}

	for (int i = 0; i < BARS; i++) {
		bool show = have && (unsigned)i < gps.nsats;
		int snr = show ? gps.sat[i].snr : -1;

		if (!show) {
			lv_obj_add_flag(bar[i], LV_OBJ_FLAG_HIDDEN);
			lv_obj_add_flag(bar_lbl[i], LV_OBJ_FLAG_HIDDEN);
			continue;
		}
		lv_obj_remove_flag(bar[i], LV_OBJ_FLAG_HIDDEN);
		lv_obj_remove_flag(bar_lbl[i], LV_OBJ_FLAG_HIDDEN);
		lv_bar_set_value(bar[i], snr < 0 ? 0 : snr, LV_ANIM_OFF);
		/* ~35 dB-Hz and up is a solid signal for a fix */
		lv_obj_set_style_bg_color(bar[i], snr >= 35 ? COL_OK : snr >= 25 ? COL_WARN : COL_DIM,
					  LV_PART_INDICATOR);
		lv_label_set_text_fmt(bar_lbl[i], "%u", gps.sat[i].prn);
	}
}

void ui_gps_build(lv_obj_t *p)
{
	lv_obj_t *t;

	lbl_status = ui_label(p, &lv_font_montserrat_14, COL_DIM, "");
	lv_obj_set_pos(lbl_status, 12, 34);

	lbl_speed = ui_label(p, &lv_font_montserrat_48, COL_TEXT, "--");
	lv_obj_set_pos(lbl_speed, 12, 56);
	lbl_speed_unit = ui_label(p, &lv_font_montserrat_14, COL_DIM, "mph");
	lv_obj_set_pos(lbl_speed_unit, 14, 110);
	lbl_heading = ui_label(p, &lv_font_montserrat_20, COL_ACCENT, "--");
	lv_obj_set_pos(lbl_heading, 100, 70);

	lbl_pos = ui_label(p, &lv_font_montserrat_14, COL_TEXT, "");
	lv_obj_set_pos(lbl_pos, 12, 136);
	lbl_detail = ui_label(p, &lv_font_montserrat_14, COL_DIM, "");
	lv_obj_set_pos(lbl_detail, 12, 176);

	/* satellite signal bars, right half */
	t = ui_label(p, &lv_font_montserrat_14, COL_DIM, "Signal (dB-Hz)");
	lv_obj_set_pos(t, 230, 56);
	for (int i = 0; i < BARS; i++) {
		int x = 226 + i * 18;

		bar[i] = lv_bar_create(p);
		lv_obj_set_size(bar[i], 12, 120);
		lv_obj_set_pos(bar[i], x, 80);
		lv_bar_set_range(bar[i], 0, 50);
		lv_obj_set_style_bg_color(bar[i], COL_PANEL_HI, LV_PART_MAIN);
		lv_obj_set_style_radius(bar[i], 2, LV_PART_MAIN);
		lv_obj_set_style_radius(bar[i], 2, LV_PART_INDICATOR);
		bar_lbl[i] = ui_label(p, &lv_font_montserrat_14, COL_DIM, "");
		lv_obj_set_width(bar_lbl[i], 18);
		lv_obj_set_style_text_align(bar_lbl[i], LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_set_pos(bar_lbl[i], x - 3, 204);
	}
	hbas_gps_view_init(&gps);
	refresh();
}

void ui_gps_update(const struct hbas_gps_view *v)
{
	gps = *v;
	refresh();
	ui_map_gps(v);
}

void ui_gps_set_metric(bool m)
{
	if (m != metric) {
		metric = m;
		refresh();
	}
}
