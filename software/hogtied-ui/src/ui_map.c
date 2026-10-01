// SPDX-License-Identifier: MIT
/*
 * Map page: the offline map (hbas-map over libosmscout) centred on the GPS
 * position, with an orange arrow for the bike. A worker thread draws each
 * frame into the buffer that isn't on screen and the UI swaps it in, so
 * the screen never waits for the map.
 *
 * Keys: Up / Down zoom in / out, OK switches north-up / heading-up.
 * Left/Right switch pages (ui.c).
 *
 * Built without HOGTIED_MAP (e.g. the snapshot build) the page just says so.
 */
#include "ui.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define MAP_W UI_WIDTH
#define MAP_H UI_HEIGHT
#define LEVEL_MIN 9
#define LEVEL_MAX 17

static lv_obj_t *img, *lbl_info, *lbl_mode, *lbl_note;
static int level = 15;
static bool heading_up = true;
static struct hbas_gps_view gps;

static void show_info(void)
{
	lv_label_set_text_fmt(lbl_mode, "%s  zoom %d", heading_up ? "Heading up" : "North up", level);
}

#ifdef HOGTIED_MAP
#include <pthread.h>

#include "hbas/map.h"

static struct {
	pthread_mutex_t lock;
	pthread_cond_t wake;
	/* request (UI -> worker) */
	bool want, stop;
	double lat, lon, rot, arrow;      /* arrow: bike direction on screen */
	int level;
	unsigned req_seq;
	/* result (worker -> UI) */
	int ready_buf;                     /* -1 none */
	unsigned done_seq;
	double ms;                         /* last render time */
	bool failed;
} job = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, .ready_buf = -1 };

static uint16_t buf[2][MAP_W * MAP_H];
static lv_image_dsc_t dsc[2];
static int shown = -1;                     /* buffer on screen */
static struct hbas_map *map;
static pthread_t worker;
static bool have_worker;

/* Filled triangle on RGB565, for the bike arrow. */
static void fill_tri(uint16_t *px, double x0, double y0, double x1, double y1, double x2, double y2,
		     uint16_t c)
{
	int minx = (int)floor(fmin(x0, fmin(x1, x2))), maxx = (int)ceil(fmax(x0, fmax(x1, x2)));
	int miny = (int)floor(fmin(y0, fmin(y1, y2))), maxy = (int)ceil(fmax(y0, fmax(y1, y2)));

	for (int y = miny < 0 ? 0 : miny; y <= maxy && y < MAP_H; y++)
		for (int x = minx < 0 ? 0 : minx; x <= maxx && x < MAP_W; x++) {
			double px_ = x + 0.5, py = y + 0.5;
			double d0 = (x1 - x0) * (py - y0) - (y1 - y0) * (px_ - x0);
			double d1 = (x2 - x1) * (py - y1) - (y2 - y1) * (px_ - x1);
			double d2 = (x0 - x2) * (py - y2) - (y0 - y2) * (px_ - x2);

			if ((d0 >= 0 && d1 >= 0 && d2 >= 0) || (d0 <= 0 && d1 <= 0 && d2 <= 0))
				px[y * MAP_W + x] = c;
		}
}

/* The bike: an orange arrow at the centre pointing along `dir` degrees on screen. */
static void draw_arrow(uint16_t *px, double dir)
{
	const double cx = MAP_W / 2.0, cy = MAP_H / 2.0, a = dir * M_PI / 180.0;
	const double s = sin(a), c = cos(a);
	/* arrow points (up = -y), 22 px long */
	double p[4][2] = { { 0, -13 }, { 9, 9 }, { 0, 4 }, { -9, 9 } };
	double q[4][2];

	for (int i = 0; i < 4; i++) {
		q[i][0] = cx + p[i][0] * c - p[i][1] * s;
		q[i][1] = cy + p[i][0] * s + p[i][1] * c;
	}
	/* dark edge first, then orange (COL_ACCENT #FF7A1A) */
	for (int k = 1; k >= 0; k--) {
		double g = k ? 1.25 : 1.0;
		uint16_t col = k ? 0x0841 : (uint16_t)(((0xFF & 0xF8) << 8) | ((0x7A & 0xFC) << 3) | (0x1A >> 3));
		double r[4][2];

		for (int i = 0; i < 4; i++) {
			r[i][0] = cx + (q[i][0] - cx) * g;
			r[i][1] = cy + (q[i][1] - cy) * g;
		}
		fill_tri(px, r[0][0], r[0][1], r[1][0], r[1][1], r[2][0], r[2][1], col);
		fill_tri(px, r[0][0], r[0][1], r[2][0], r[2][1], r[3][0], r[3][1], col);
	}
}

static void *worker_main(void *arg)
{
	(void)arg;
	pthread_mutex_lock(&job.lock);
	while (!job.stop) {
		double lat, lon, rot, arrow, ms;
		int lv, target;
		unsigned seq;
		struct timespec t0, t1;
		bool ok;

		while (!job.want && !job.stop)
			pthread_cond_wait(&job.wake, &job.lock);
		if (job.stop)
			break;
		job.want = false;
		lat = job.lat;
		lon = job.lon;
		rot = job.rot;
		arrow = job.arrow;
		lv = job.level;
		seq = job.req_seq;
		target = shown == 0 ? 1 : 0;          /* never the buffer on screen */
		pthread_mutex_unlock(&job.lock);

		clock_gettime(CLOCK_MONOTONIC, &t0);
		ok = hbas_map_render(map, lat, lon, lv, rot, buf[target], MAP_W, MAP_H) == 0;
		if (ok)
			draw_arrow(buf[target], arrow);
		clock_gettime(CLOCK_MONOTONIC, &t1);
		ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;

		pthread_mutex_lock(&job.lock);
		job.failed = !ok;
		job.ms = ms;
		if (ok) {
			job.ready_buf = target;
			job.done_seq = seq;
		}
	}
	pthread_mutex_unlock(&job.lock);
	return NULL;
}

static void request(void)
{
	if (!have_worker || !gps.valid)
		return;
	pthread_mutex_lock(&job.lock);
	job.lat = gps.lat;
	job.lon = gps.lon;
	job.rot = heading_up && gps.course >= 0 && gps.speed_kmh >= 3.0 ? gps.course : 0;
	/* heading-up: the bike points up; north-up: along its course */
	job.arrow = job.rot != 0 ? 0 : gps.course < 0 ? 0 : gps.course;
	job.level = level;
	job.req_seq++;
	job.want = true;
	pthread_cond_signal(&job.wake);
	pthread_mutex_unlock(&job.lock);
}

void ui_map_open(const char *db, const char *style, const char *font)
{
	char err[128];

	if (!db || !style || !font) {
		lv_label_set_text(lbl_note, "No map installed");
		return;
	}
	if (!(map = hbas_map_open(db, style, font, 110, err, sizeof(err)))) {
		lv_label_set_text_fmt(lbl_note, "Map not available:\n%s", err);
		fprintf(stderr, "map: %s: %s\n", db, err);
		return;
	}
	for (int i = 0; i < 2; i++) {
		dsc[i].header.magic = LV_IMAGE_HEADER_MAGIC;
		dsc[i].header.cf = LV_COLOR_FORMAT_RGB565;
		dsc[i].header.w = MAP_W;
		dsc[i].header.h = MAP_H;
		dsc[i].header.stride = MAP_W * 2;
		dsc[i].data_size = sizeof(buf[i]);
		dsc[i].data = (const uint8_t *)buf[i];
	}
	have_worker = pthread_create(&worker, NULL, worker_main, NULL) == 0;
	lv_label_set_text(lbl_note, "Waiting for a GPS fix...");
	fprintf(stderr, "map: %s\n", db);
}

bool ui_map_has_frame(void)
{
	return shown >= 0;
}

void ui_map_close(void)
{
	if (have_worker) {
		pthread_mutex_lock(&job.lock);
		job.stop = true;
		pthread_cond_signal(&job.wake);
		pthread_mutex_unlock(&job.lock);
		pthread_join(worker, NULL);
		have_worker = false;
	}
	hbas_map_close(map);
	map = NULL;
}

void ui_map_tick(void)
{
	int ready = -1;
	double ms = 0;

	if (!have_worker)
		return;
	pthread_mutex_lock(&job.lock);
	if (job.ready_buf >= 0) {
		ready = job.ready_buf;
		ms = job.ms;
		job.ready_buf = -1;
		shown = ready;             /* worker now draws into the other one */
	}
	pthread_mutex_unlock(&job.lock);
	if (ready < 0)
		return;
	lv_image_cache_drop(&dsc[ready]);
	lv_image_set_src(img, &dsc[ready]);
	lv_obj_remove_flag(img, LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_flag(lbl_note, LV_OBJ_FLAG_HIDDEN);
	lv_label_set_text_fmt(lbl_info, "%d ms", (int)ms);
}
#else
void ui_map_open(const char *db, const char *style, const char *font)
{
	(void)db; (void)style; (void)font;
}

void ui_map_close(void) {}
void ui_map_tick(void) {}
bool ui_map_has_frame(void) { return false; }
static void request(void) {}
#endif

void ui_map_build(lv_obj_t *p)
{
	lv_obj_t *attr;

	img = lv_image_create(p);
	lv_obj_set_pos(img, 0, 0);
	lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
	lv_obj_move_background(img);
	lbl_note = ui_label(p, &lv_font_montserrat_14, COL_DIM,
#ifdef HOGTIED_MAP
			    "No map installed"
#else
			    "Maps are not part of this build"
#endif
			    );
	lv_obj_center(lbl_note);
	lv_obj_set_style_text_align(lbl_note, LV_TEXT_ALIGN_CENTER, 0);

	lbl_mode = ui_label(p, &lv_font_montserrat_14, COL_TEXT, "");
	lv_obj_set_style_bg_color(lbl_mode, COL_BG, 0);
	lv_obj_set_style_bg_opa(lbl_mode, LV_OPA_70, 0);
	lv_obj_set_style_pad_all(lbl_mode, 3, 0);
	lv_obj_align(lbl_mode, LV_ALIGN_BOTTOM_RIGHT, -4, -4);
	lbl_info = ui_label(p, &lv_font_montserrat_14, COL_DIM, "");
	lv_obj_align(lbl_info, LV_ALIGN_TOP_RIGHT, -8, 28);
	/* ODbL: the map data needs this credit */
	attr = ui_label(p, &lv_font_montserrat_14, COL_DIM, "(c) OpenStreetMap");   /* no (c) glyph in the font */
	lv_obj_set_style_bg_color(attr, COL_BG, 0);
	lv_obj_set_style_bg_opa(attr, LV_OPA_70, 0);
	lv_obj_set_style_pad_all(attr, 3, 0);
	lv_obj_align(attr, LV_ALIGN_BOTTOM_LEFT, 4, -4);
	show_info();
}

void ui_map_gps(const struct hbas_gps_view *v)
{
	bool was_valid = gps.valid;

	gps = *v;
	if (!gps.valid && was_valid)
		lv_label_set_text(lbl_note, "Waiting for a GPS fix...");
	request();
}

bool ui_map_key(enum ui_key key)
{
	switch (key) {
	case UI_KEY_UP: if (level < LEVEL_MAX) level++; break;
	case UI_KEY_DOWN: if (level > LEVEL_MIN) level--; break;
	case UI_KEY_ENTER: heading_up = !heading_up; break;
	default: return false;
	}
	show_info();
	request();
	return true;
}
