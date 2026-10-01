// SPDX-License-Identifier: MIT
/*
 * Map page: the offline map (hbas-map over libosmscout) centred on the GPS
 * position, with an orange arrow for the bike. A worker thread draws each
 * frame into the buffer that isn't on screen and the UI swaps it in, so
 * the screen never waits for the map.
 *
 * Navigation: OK opens the menu - saved places to route to, "Save this
 * spot", the route option (fastest, shortest, no highways, back roads),
 * north-up / heading-up, and stopping the route. Routes are calculated on
 * the map thread too and drawn in orange. Places and the route option are
 * kept in the places file (libhbas places.h) next to the settings.
 *
 * Keys: Up / Down zoom in / out, OK opens the menu (in it: Up / Down
 * choose, OK selects, Back or Left closes). Left/Right switch pages (ui.c).
 *
 * Built without HOGTIED_MAP (e.g. the snapshot build) the page just says so.
 */
#include "ui.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "hbas/places.h"
#include "hbas/settings.h"
#include "persist.h"

#define MAP_W UI_WIDTH
#define MAP_H UI_HEIGHT
#define LEVEL_MIN 9
#define LEVEL_MAX 17

#define MENU_ROWS 7
#define ROUTE_MODES HBAS_ROUTE_OPTION_COUNT

static const char *const mode_names[ROUTE_MODES] = {
	"Fastest", "Shortest", "No highways", "Back roads",   /* hbas-map's order */
};

static lv_obj_t *img, *lbl_info, *lbl_mode, *lbl_note, *lbl_route;
static lv_obj_t *menu, *menu_rows[MENU_ROWS];
static int level = 15;
static bool heading_up = true;
static struct hbas_gps_view gps;

/* saved places and route option */
static struct hbas_places places;
static const char *places_path;

/* the route being followed */
static struct {
	bool active, busy;
	char name[HBAS_PLACE_NAME];
	double lat, lon;
} dest;

/* the menu: a list of items built when it opens */
enum item_kind { IT_STOP, IT_PLACE, IT_SAVE_HERE, IT_MODE, IT_ORIENT };
static struct { enum item_kind kind; int place; } items[HBAS_PLACES_MAX + 4];
static int n_items, sel, menu_top;
static bool menu_open;

static void route_to(const char *name, double lat, double lon);
static void clear_route(void);

static void show_info(void)
{
	lv_label_set_text_fmt(lbl_mode, "%s  zoom %d", heading_up ? "Heading up" : "North up", level);
}

static void show_route_banner(const char *text)
{
	if (!text) {
		lv_obj_add_flag(lbl_route, LV_OBJ_FLAG_HIDDEN);
		return;
	}
	lv_label_set_text(lbl_route, text);
	lv_obj_remove_flag(lbl_route, LV_OBJ_FLAG_HIDDEN);
}

#ifdef HOGTIED_MAP
#include <pthread.h>

#include "hbas/map.h"

static struct {
	pthread_mutex_t lock;
	pthread_cond_t wake;
	/* request (UI -> worker) */
	bool want, stop, drawing;
	double lat, lon, rot, arrow;      /* arrow: bike direction on screen */
	int level;
	unsigned req_seq;
	bool want_route, want_clear;
	double from_lat, from_lon, heading, to_lat, to_lon;
	int mode;
	/* result (worker -> UI) */
	bool route_done, route_ok;
	char route_err[96];
	struct hbas_route_info route;
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

		while (!job.want && !job.want_route && !job.want_clear && !job.stop)
			pthread_cond_wait(&job.wake, &job.lock);
		if (job.stop)
			break;
		if (job.want_clear) {
			job.want_clear = false;
			hbas_map_clear_route(map);
			job.want = true;               /* redraw without it */
		}
		if (job.want_route) {
			double f_lat = job.from_lat, f_lon = job.from_lon, hd = job.heading;
			double t_lat = job.to_lat, t_lon = job.to_lon;
			int mode = job.mode, rc;
			struct hbas_route_info info = { 0 };
			char err[96] = "";

			job.want_route = false;
			pthread_mutex_unlock(&job.lock);
			rc = hbas_map_route(map, f_lat, f_lon, hd, t_lat, t_lon,
					    (enum hbas_route_mode)mode, &info, err, sizeof(err));
			pthread_mutex_lock(&job.lock);
			job.route_done = true;
			job.route_ok = rc == 0;
			job.route = info;
			snprintf(job.route_err, sizeof(job.route_err), "%s", err);
			job.want = true;               /* draw it */
			continue;                      /* a newer request may be waiting */
		}
		job.want = false;
		job.drawing = true;
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
		job.drawing = false;
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

static void route_to(const char *name, double lat, double lon)
{
	char text[96];

	if (!have_worker)
		return;
	if (!gps.valid) {
		show_route_banner("Can't route without a GPS fix");
		return;
	}
	snprintf(dest.name, sizeof(dest.name), "%s", name);
	dest.lat = lat;
	dest.lon = lon;
	dest.busy = true;
	pthread_mutex_lock(&job.lock);
	job.from_lat = gps.lat;
	job.from_lon = gps.lon;
	job.heading = gps.course >= 0 && gps.speed_kmh >= 3.0 ? gps.course : -1;
	job.to_lat = lat;
	job.to_lon = lon;
	job.mode = places.route;
	job.want_route = true;
	pthread_cond_signal(&job.wake);
	pthread_mutex_unlock(&job.lock);
	snprintf(text, sizeof(text), "Routing to %s...", dest.name);
	show_route_banner(text);
}

static void clear_route(void)
{
	dest.active = dest.busy = false;
	show_route_banner(NULL);
	if (!have_worker)
		return;
	pthread_mutex_lock(&job.lock);
	job.want_clear = true;
	pthread_cond_signal(&job.wake);
	pthread_mutex_unlock(&job.lock);
}

/* "189 km  2 h 07" / "800 m  3 min" */
static void format_trip(char *buf, size_t len, const struct hbas_route_info *r)
{
	int min = (int)lround(r->duration_s / 60);
	char d[16], t[16];

	if (r->distance_m < 1000)
		snprintf(d, sizeof(d), "%d m", (int)lround(r->distance_m / 10) * 10);
	else if (r->distance_m < 10000)
		snprintf(d, sizeof(d), "%.1f km", r->distance_m / 1000);
	else
		snprintf(d, sizeof(d), "%d km", (int)lround(r->distance_m / 1000));
	if (min < 60)
		snprintf(t, sizeof(t), "%d min", min < 1 ? 1 : min);
	else
		snprintf(t, sizeof(t), "%d h %02d", min / 60, min % 60);
	snprintf(buf, len, "%s  %s", d, t);
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

bool ui_map_idle(void)
{
	bool idle;

	if (!have_worker)
		return true;
	pthread_mutex_lock(&job.lock);
	idle = !job.want && !job.drawing && !job.want_route && !job.want_clear && !job.route_done &&
	       job.ready_buf < 0;
	pthread_mutex_unlock(&job.lock);
	return idle && !dest.busy;
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
	if (job.route_done) {
		char text[192], trip[40];

		job.route_done = false;
		if (dest.busy) {
			dest.busy = false;
			dest.active = job.route_ok;
			if (job.route_ok) {
				format_trip(trip, sizeof(trip), &job.route);
				snprintf(text, sizeof(text), "To %s\n%s  %s", dest.name, trip,
					 mode_names[job.mode]);
			} else {
				snprintf(text, sizeof(text), "No route to %s:\n%s", dest.name,
					 job.route_err);
			}
			show_route_banner(text);
		}
	}
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
bool ui_map_idle(void) { return true; }
static void request(void) {}
static void route_to(const char *name, double lat, double lon)
{
	(void)name; (void)lat; (void)lon;
}
static void clear_route(void) {}
#endif

/* ---- saved places ------------------------------------------------------- */

static void save_places(void)
{
	char text[HBAS_PLACES_TEXT_MAX];

	if (!places_path || hbas_places_format(&places, text, sizeof(text)) < 0)
		return;
	if (!persist_write_file(places_path, text))
		fprintf(stderr, "places: saved %s\n", places_path);
}

void ui_map_places(const char *path)
{
	char text[HBAS_PLACES_TEXT_MAX];

	places_path = path;
	hbas_places_init(&places);
	if (!path || hbas_settings_load(path, text, sizeof(text)))
		return;
	if (hbas_places_parse(text, &places) < 0)
		fprintf(stderr, "places: %s is not a places file\n", path);
	else
		fprintf(stderr, "places: %d from %s\n", places.count, path);
}

void ui_map_add_place(const char *name, double lat, double lon)
{
	hbas_places_add(&places, name, lat, lon);
}

/* ---- menu --------------------------------------------------------------- */

static void item_text(int i, char *buf, size_t len)
{
	switch (items[i].kind) {
	case IT_STOP: snprintf(buf, len, "Stop route to %s", dest.name); break;
	case IT_PLACE: snprintf(buf, len, "Go to %s", places.place[items[i].place].name); break;
	case IT_SAVE_HERE: snprintf(buf, len, "Save this spot"); break;
	case IT_MODE: snprintf(buf, len, "Route: < %s >", mode_names[places.route]); break;
	case IT_ORIENT: snprintf(buf, len, "Map: %s", heading_up ? "Heading up" : "North up"); break;
	}
}

static void build_items(void)
{
	n_items = 0;
	if (dest.active || dest.busy)
		items[n_items++] = (typeof(items[0])){ IT_STOP, -1 };
	for (int i = 0; i < places.count; i++)
		items[n_items++] = (typeof(items[0])){ IT_PLACE, i };
	items[n_items++] = (typeof(items[0])){ IT_SAVE_HERE, -1 };
	items[n_items++] = (typeof(items[0])){ IT_MODE, -1 };
	items[n_items++] = (typeof(items[0])){ IT_ORIENT, -1 };
	if (sel >= n_items)
		sel = n_items - 1;
}

static void show_menu(void)
{
	char text[64];

	if (sel < menu_top)
		menu_top = sel;
	if (sel >= menu_top + MENU_ROWS)
		menu_top = sel - MENU_ROWS + 1;
	for (int r = 0; r < MENU_ROWS; r++) {
		int i = menu_top + r;

		if (i >= n_items) {
			lv_obj_add_flag(menu_rows[r], LV_OBJ_FLAG_HIDDEN);
			continue;
		}
		item_text(i, text, sizeof(text));
		lv_label_set_text(menu_rows[r], text);
		lv_obj_set_style_bg_opa(menu_rows[r], i == sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
		lv_obj_set_style_text_color(menu_rows[r], i == sel ? COL_BG : COL_TEXT, 0);
		lv_obj_remove_flag(menu_rows[r], LV_OBJ_FLAG_HIDDEN);
	}
	lv_obj_set_height(menu, (n_items < MENU_ROWS ? n_items : MENU_ROWS) * 24 + 10);
}

static void open_menu(void)
{
	sel = menu_top = 0;
	build_items();
	menu_open = true;
	lv_obj_remove_flag(menu, LV_OBJ_FLAG_HIDDEN);
	show_menu();
}

static void close_menu(void)
{
	menu_open = false;
	lv_obj_add_flag(menu, LV_OBJ_FLAG_HIDDEN);
}

static void change_mode(int d)
{
	places.route = (places.route + ROUTE_MODES + d) % ROUTE_MODES;
	save_places();
	if (dest.active || dest.busy)
		route_to(dest.name, dest.lat, dest.lon);     /* again, the new way */
	show_menu();
}

static void select_item(void)
{
	char name[HBAS_PLACE_NAME];

	switch (items[sel].kind) {
	case IT_STOP:
		clear_route();
		close_menu();
		break;
	case IT_PLACE: {
		const struct hbas_place *pl = &places.place[items[sel].place];

		route_to(pl->name, pl->lat, pl->lon);
		close_menu();
		break;
	}
	case IT_SAVE_HERE:
		if (!gps.valid) {
			show_route_banner("Can't save a spot without a GPS fix");
			close_menu();
			break;
		}
		/* named by number for now; the on-screen keyboard will rename */
		snprintf(name, sizeof(name), "Spot %d", places.count + 1);
		if (hbas_places_add(&places, name, gps.lat, gps.lon) < 0) {
			show_route_banner("The saved places list is full");
			close_menu();
			break;
		}
		save_places();
		build_items();
		show_menu();
		break;
	case IT_MODE:
		change_mode(1);
		break;
	case IT_ORIENT:
		heading_up = !heading_up;
		show_info();
		request();
		show_menu();
		break;
	}
}

static bool menu_key(enum ui_key key)
{
	switch (key) {
	case UI_KEY_UP:
		if (sel > 0)
			sel--;
		show_menu();
		break;
	case UI_KEY_DOWN:
		if (sel < n_items - 1)
			sel++;
		show_menu();
		break;
	case UI_KEY_RIGHT:
		if (items[sel].kind == IT_MODE)
			change_mode(1);
		break;
	case UI_KEY_LEFT:
		if (items[sel].kind == IT_MODE)
			change_mode(-1);
		else
			close_menu();
		break;
	case UI_KEY_ENTER:
		select_item();
		break;
	case UI_KEY_BACK:
		close_menu();
		break;
	}
	return true;                            /* the menu takes every key */
}

/* ---- page --------------------------------------------------------------- */

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

	/* where we're going, and how far */
	lbl_route = ui_label(p, &lv_font_montserrat_14, COL_TEXT, "");
	lv_obj_set_style_bg_color(lbl_route, COL_BG, 0);
	lv_obj_set_style_bg_opa(lbl_route, LV_OPA_80, 0);
	lv_obj_set_style_border_side(lbl_route, LV_BORDER_SIDE_LEFT, 0);
	lv_obj_set_style_border_width(lbl_route, 3, 0);
	lv_obj_set_style_border_color(lbl_route, COL_ACCENT, 0);
	lv_obj_set_style_pad_all(lbl_route, 4, 0);
	lv_obj_set_style_pad_left(lbl_route, 7, 0);
	lv_obj_align(lbl_route, LV_ALIGN_TOP_LEFT, 6, 28);
	lv_obj_add_flag(lbl_route, LV_OBJ_FLAG_HIDDEN);

	/* the navigation menu */
	menu = lv_obj_create(p);
	lv_obj_remove_style_all(menu);
	lv_obj_set_width(menu, 260);
	lv_obj_align(menu, LV_ALIGN_CENTER, 0, 10);
	lv_obj_set_style_bg_color(menu, COL_PANEL, 0);
	lv_obj_set_style_bg_opa(menu, LV_OPA_90, 0);
	lv_obj_set_style_border_color(menu, COL_ACCENT, 0);
	lv_obj_set_style_border_width(menu, 1, 0);
	lv_obj_set_style_radius(menu, 6, 0);
	lv_obj_set_style_pad_all(menu, 4, 0);
	for (int r = 0; r < MENU_ROWS; r++) {
		menu_rows[r] = ui_label(menu, &lv_font_montserrat_14, COL_TEXT, "");
		lv_obj_set_size(menu_rows[r], 250, 24);
		lv_obj_set_pos(menu_rows[r], 1, r * 24);
		lv_obj_set_style_pad_left(menu_rows[r], 6, 0);
		lv_obj_set_style_pad_top(menu_rows[r], 3, 0);
		lv_obj_set_style_radius(menu_rows[r], 4, 0);
		lv_obj_set_style_bg_color(menu_rows[r], COL_ACCENT, 0);
		lv_label_set_long_mode(menu_rows[r], LV_LABEL_LONG_CLIP);
	}
	lv_obj_add_flag(menu, LV_OBJ_FLAG_HIDDEN);
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
	if (menu_open)
		return menu_key(key);
	switch (key) {
	case UI_KEY_UP: if (level < LEVEL_MAX) level++; break;
	case UI_KEY_DOWN: if (level > LEVEL_MIN) level--; break;
	case UI_KEY_ENTER: open_menu(); return true;
	default: return false;
	}
	show_info();
	request();
	return true;
}
