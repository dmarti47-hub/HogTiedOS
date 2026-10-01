// SPDX-License-Identifier: MIT
/*
 * Map page: the offline map (hbas-map over libosmscout) centred on the GPS
 * position, with an orange arrow for the bike. A worker thread draws each
 * frame into the buffer that isn't on screen and the UI swaps it in, so
 * the screen never waits for the map.
 *
 * Navigation: OK opens the menu - find an address (on-screen keyboard with
 * autofill from the map's address index), saved places (go / rename /
 * delete), "Save this spot", the route option (fastest, shortest, no
 * highways, back roads), north-up / heading-up, and stopping the route.
 * Routes and searches run on the map thread too; the route is drawn in
 * orange. While following it, the remaining distance and time count down,
 * leaving the route recalculates it from where the bike is (libhbas
 * route.h decides when), and reaching the end says so. Places and the route
 * option are kept in the places file (libhbas places.h) next to the
 * settings.
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
#include "hbas/route.h"
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
static lv_obj_t *turn_panel, *turn_line[3], *lbl_turn_dist, *lbl_turn_road, *lbl_turn_trip;
static bool metric;                        /* the bike's unit setting */
static lv_obj_t *menu, *menu_rows[MENU_ROWS];
static int level = 15;
static bool heading_up = true;
static struct hbas_gps_view gps;

/* saved places and route option */
static struct hbas_places places;
static const char *places_path;

/* the route being followed */
#define ROUTE_POINTS_MAX 65536
static struct {
	bool active, busy, rerouting;
	char name[HBAS_PLACE_NAME];
	double lat, lon;
	double trip_m, trip_s;         /* as calculated */
	int mode;
	double lat_pts[ROUTE_POINTS_MAX], lon_pts[ROUTE_POINTS_MAX];
	struct hbas_route_line line;
	struct hbas_route_follow follow;
} dest;

/* destination search (autofill) */
#define SEARCH_MAX UI_KBD_SUGGESTIONS
static struct {
	char label[SEARCH_MAX][64];
	double lat[SEARCH_MAX], lon[SEARCH_MAX];
	int n, picked;
} found;

/* the menu: a list of items built when it opens; a place or a search
 * result has its own small menu */
enum menu_mode { MENU_MAIN, MENU_PLACE, MENU_RESULT };
enum item_kind {
	IT_STOP, IT_FIND, IT_PLACE, IT_SAVE_HERE, IT_MODE, IT_ORIENT,
	IT_P_GO, IT_P_RENAME, IT_P_DELETE,
	IT_R_GO, IT_R_SAVE_GO, IT_R_SAVE,
	IT_BACK,
};
static struct { enum item_kind kind; int place; } items[HBAS_PLACES_MAX + 8];
static int n_items, sel, menu_top, menu_place;
static enum menu_mode menu_mode;
static bool menu_open;
static int renaming = -1;                  /* place being named on the keyboard */

static void route_to(const char *name, double lat, double lon);
static void clear_route(void);
static void search(const char *query);
static void open_menu_mode(enum menu_mode m);
static void close_menu(void);
static void show_next_turn(const struct hbas_route_pos *pos);

static void show_info(void)
{
	lv_label_set_text_fmt(lbl_mode, "%s  zoom %d", heading_up ? "Heading up" : "North up", level);
}

static void show_route_banner(const char *text)
{
	if (!text) {
		lv_obj_add_flag(lbl_route, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(turn_panel, LV_OBJ_FLAG_HIDDEN);
		return;
	}
	lv_label_set_text(lbl_route, text);
	lv_obj_remove_flag(lbl_route, LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_flag(turn_panel, LV_OBJ_FLAG_HIDDEN);
}

/* "189 km  2 h 07" / "800 m  3 min" */
static __attribute__((unused)) void format_trip(char *buf, size_t len, double dist_m,
						 double secs)
{
	int min = (int)lround(secs / 60);
	char d[16], t[16];

	hbas_nav_distance(d, sizeof(d), dist_m, metric);
	if (min < 60)
		snprintf(t, sizeof(t), "%d min", min < 1 ? 1 : min);
	else
		snprintf(t, sizeof(t), "%d h %02d", min / 60, min % 60);
	snprintf(buf, len, "%s  %s", d, t);
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
	bool want_route, want_clear, want_search;
	double from_lat, from_lon, heading, to_lat, to_lon;
	int mode;
	char query[64];
	double near_lat, near_lon;
	/* result (worker -> UI) */
	bool route_done, route_ok, search_done;
	char route_err[96];
	struct hbas_route_info route;
	size_t route_n;                    /* points in route_lat/lon */
	struct hbas_search_result results[SEARCH_MAX];
	int n_results;
	int ready_buf;                     /* -1 none */
	unsigned done_seq;
	double ms;                         /* last render time */
	bool failed;
} job = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, .ready_buf = -1 };
static double job_lat[ROUTE_POINTS_MAX], job_lon[ROUTE_POINTS_MAX];   /* under job.lock */
#define STEPS_MAX 1024
static struct hbas_route_step job_steps[STEPS_MAX], steps[STEPS_MAX];
static size_t job_n_steps, n_steps;

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

		while (!job.want && !job.want_route && !job.want_clear && !job.want_search &&
		       !job.stop)
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
			if (rc == 0) {
				size_t n = hbas_map_route_points(map, job_lat, job_lon,
								 ROUTE_POINTS_MAX);

				job.route_n = n < ROUTE_POINTS_MAX ? n : ROUTE_POINTS_MAX;
				n = hbas_map_route_steps(map, job_steps, STEPS_MAX);
				job_n_steps = n < STEPS_MAX ? n : STEPS_MAX;
			}
			job.route_done = true;
			job.route_ok = rc == 0;
			job.route = info;
			snprintf(job.route_err, sizeof(job.route_err), "%s", err);
			job.want = true;               /* draw it */
			continue;                      /* a newer request may be waiting */
		}
		if (job.want_search) {
			char q[sizeof(job.query)];
			double nl = job.near_lat, nn = job.near_lon;
			struct hbas_search_result r[SEARCH_MAX];
			int n;

			job.want_search = false;
			snprintf(q, sizeof(q), "%s", job.query);
			pthread_mutex_unlock(&job.lock);
			n = hbas_map_search(map, q, nl, nn, r, SEARCH_MAX);
			pthread_mutex_lock(&job.lock);
			if (!job.want_search) {        /* else: typed on, that one's newer */
				job.n_results = n < 0 ? 0 : n;
				memcpy(job.results, r, sizeof(r));
				job.search_done = true;
			}
			continue;
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
	if (name != dest.name)
		snprintf(dest.name, sizeof(dest.name), "%s", name);
	dest.lat = lat;
	dest.lon = lon;
	dest.busy = true;
	dest.mode = places.route;
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
	snprintf(text, sizeof(text), "%s %s...", dest.rerouting ? "Off route, rerouting to" :
		 "Routing to", dest.name);
	show_route_banner(text);
}

static void search(const char *query)
{
	if (!have_worker)
		return;
	pthread_mutex_lock(&job.lock);
	snprintf(job.query, sizeof(job.query), "%s", query);
	/* no fix yet: search around the middle of nowhere in particular */
	job.near_lat = gps.valid ? gps.lat : 0;
	job.near_lon = gps.valid ? gps.lon : 0;
	job.want_search = query[0] != '\0';
	if (!job.want_search) {
		job.n_results = 0;
		job.search_done = true;
	}
	pthread_cond_signal(&job.wake);
	pthread_mutex_unlock(&job.lock);
}

static void clear_route(void)
{
	dest.active = dest.busy = dest.rerouting = false;
	show_route_banner(NULL);
	if (!have_worker)
		return;
	pthread_mutex_lock(&job.lock);
	job.want_clear = true;
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

bool ui_map_idle(void)
{
	bool idle;

	if (!have_worker)
		return true;
	pthread_mutex_lock(&job.lock);
	idle = !job.want && !job.drawing && !job.want_route && !job.want_clear && !job.route_done &&
	       !job.want_search && !job.search_done && job.ready_buf < 0;
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
		bool want_turn_panel = false;

		job.route_done = false;
		if (dest.busy) {
			dest.busy = false;
			if (job.route_ok) {
				dest.active = true;
				dest.trip_m = job.route.distance_m;
				dest.trip_s = job.route.duration_s;
				memcpy(dest.lat_pts, job_lat, job.route_n * sizeof(double));
				memcpy(dest.lon_pts, job_lon, job.route_n * sizeof(double));
				hbas_route_line_init(&dest.line, dest.lat_pts, dest.lon_pts,
						     job.route_n);
				memcpy(steps, job_steps, job_n_steps * sizeof(steps[0]));
				n_steps = job_n_steps;
				if (!dest.rerouting)
					hbas_route_follow_reset(&dest.follow);
				dest.follow.off_count = 0;
				format_trip(trip, sizeof(trip), dest.trip_m, dest.trip_s);
				snprintf(text, sizeof(text), "To %s\n%s  %s", dest.name, trip,
					 mode_names[dest.mode]);
				want_turn_panel = true;
			} else if (dest.rerouting && dest.active) {
				/* keep following the old one; try again later */
				snprintf(text, sizeof(text), "Off route to %s:\n%s", dest.name,
					 job.route_err);
			} else {
				dest.active = false;
				snprintf(text, sizeof(text), "No route to %s:\n%s", dest.name,
					 job.route_err);
			}
			dest.rerouting = false;
			show_route_banner(text);
			if (want_turn_panel) {
				struct hbas_route_pos pos;

				hbas_route_locate(&dest.line, gps.lat, gps.lon, 0, &pos);
				show_next_turn(&pos);
			}
		}
	}
	if (job.search_done) {
		const char *labels[SEARCH_MAX];

		job.search_done = false;
		found.n = job.n_results;
		for (int i = 0; i < found.n; i++) {
			snprintf(found.label[i], sizeof(found.label[i]), "%s", job.results[i].label);
			found.lat[i] = job.results[i].lat;
			found.lon[i] = job.results[i].lon;
			labels[i] = found.label[i];
		}
		if (ui_kbd_active() && renaming < 0)
			ui_kbd_suggest(labels, found.n);
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
/* ---- next-turn panel ------------------------------------------------------ */

/* The arrow: a bent shaft (from the bottom middle of a 44 x 44 box) and a
 * head at its tip, in the direction of its last part. */
static lv_point_precise_t arrow_pts[3][3];

static void set_arrow(enum hbas_turn t)
{
	/* the shaft's bend and tip, by turn */
	static const struct { int bx, by, tx, ty; } shape[] = {
		[HBAS_TURN_STRAIGHT] = { 22, 22, 22, 5 },
		[HBAS_TURN_SLIGHT_LEFT] = { 22, 26, 10, 10 },
		[HBAS_TURN_LEFT] = { 22, 20, 5, 20 },
		[HBAS_TURN_SHARP_LEFT] = { 22, 14, 8, 30 },
		[HBAS_TURN_SLIGHT_RIGHT] = { 22, 26, 34, 10 },
		[HBAS_TURN_RIGHT] = { 22, 20, 39, 20 },
		[HBAS_TURN_SHARP_RIGHT] = { 22, 14, 36, 30 },
		[HBAS_TURN_ROUNDABOUT] = { 22, 22, 22, 5 },
		[HBAS_TURN_MOTORWAY_ENTER] = { 22, 26, 34, 10 },
		[HBAS_TURN_MOTORWAY_EXIT_LEFT] = { 22, 26, 10, 10 },
		[HBAS_TURN_MOTORWAY_EXIT_RIGHT] = { 22, 26, 34, 10 },
		[HBAS_TURN_KEEP_LEFT] = { 22, 26, 12, 8 },
		[HBAS_TURN_KEEP_RIGHT] = { 22, 26, 32, 8 },
		[HBAS_TURN_ARRIVE] = { 22, 22, 22, 5 },
	};
	int k = (unsigned)t < sizeof(shape) / sizeof(shape[0]) ? (int)t : 0;
	double dx = shape[k].tx - shape[k].bx, dy = shape[k].ty - shape[k].by;
	double len = sqrt(dx * dx + dy * dy), ux = dx / len, uy = dy / len;

	arrow_pts[0][0] = (lv_point_precise_t){ 22, 42 };
	arrow_pts[0][1] = (lv_point_precise_t){ shape[k].bx, shape[k].by };
	arrow_pts[0][2] = (lv_point_precise_t){ shape[k].tx, shape[k].ty };
	/* head: two strokes back from the tip, 45 degrees either side */
	for (int side = 0; side < 2; side++) {
		double a = side ? 2.356 : -2.356, c = cos(a), sn = sin(a);
		double hx = (ux * c - uy * sn) * 11, hy = (ux * sn + uy * c) * 11;

		arrow_pts[1 + side][0] = (lv_point_precise_t){ shape[k].tx, shape[k].ty };
		arrow_pts[1 + side][1] = (lv_point_precise_t){ shape[k].tx + hx, shape[k].ty + hy };
	}
	lv_line_set_points(turn_line[0], arrow_pts[0], 3);
	lv_line_set_points(turn_line[1], arrow_pts[1], 2);
	lv_line_set_points(turn_line[2], arrow_pts[2], 2);
}

static const char *turn_words(const struct hbas_route_step *st, char *buf, size_t len)
{
	static const char *const verbs[] = {
		[HBAS_TURN_STRAIGHT] = "Continue",
		[HBAS_TURN_SLIGHT_LEFT] = "Bear left", [HBAS_TURN_LEFT] = "Left",
		[HBAS_TURN_SHARP_LEFT] = "Sharp left",
		[HBAS_TURN_SLIGHT_RIGHT] = "Bear right", [HBAS_TURN_RIGHT] = "Right",
		[HBAS_TURN_SHARP_RIGHT] = "Sharp right",
		[HBAS_TURN_MOTORWAY_ENTER] = "Take the ramp",
		[HBAS_TURN_MOTORWAY_EXIT_LEFT] = "Exit left",
		[HBAS_TURN_MOTORWAY_EXIT_RIGHT] = "Exit right",
		[HBAS_TURN_KEEP_LEFT] = "Keep left", [HBAS_TURN_KEEP_RIGHT] = "Keep right",
	};
	const char *onto = st->turn == HBAS_TURN_MOTORWAY_EXIT_LEFT ||
			   st->turn == HBAS_TURN_MOTORWAY_EXIT_RIGHT ? "to" : "onto";

	if (st->turn == HBAS_TURN_ARRIVE)
		snprintf(buf, len, "Arrive at %s", dest.name);
	else if (st->turn == HBAS_TURN_ROUNDABOUT && st->exit > 0)
		snprintf(buf, len, "Roundabout, exit %d%s%s", st->exit, st->name[0] ? " - " : "",
			 st->name);
	else if (st->turn == HBAS_TURN_ROUNDABOUT)
		snprintf(buf, len, "Roundabout%s%s", st->name[0] ? " - " : "", st->name);
	else if (st->name[0])
		snprintf(buf, len, "%s %s %s", verbs[st->turn], onto, st->name);
	else
		snprintf(buf, len, "%s", verbs[st->turn]);
	return buf;
}

/* The next manoeuvre after `along` metres of the route (in the
 * description's distances), or NULL. */
static const struct hbas_route_step *next_step(double along)
{
	for (size_t i = 0; i < n_steps; i++)
		if (steps[i].dist_m > along + 5)
			return &steps[i];
	return NULL;
}

static void show_next_turn(const struct hbas_route_pos *pos)
{
	/* the description's distances and our route line's differ a little:
	 * scale ours to theirs */
	double scale = dest.line.total_m > 0 ? dest.trip_m / dest.line.total_m : 1;
	double along = pos->done_m * scale;
	const struct hbas_route_step *st = next_step(along);
	char d[16], words[96], trip[40];

	if (!st)
		return;
	hbas_nav_distance(d, sizeof(d), st->dist_m - along, metric);
	set_arrow(st->turn);
	lv_label_set_text(lbl_turn_dist, d);
	lv_label_set_text(lbl_turn_road, turn_words(st, words, sizeof(words)));
	format_trip(trip, sizeof(trip), pos->remaining_m * scale,
		    dest.trip_m > 0 ? dest.trip_s * pos->remaining_m * scale / dest.trip_m : 0);
	lv_label_set_text_fmt(lbl_turn_trip, "%s  %s", dest.name, trip);
	lv_obj_add_flag(lbl_route, LV_OBJ_FLAG_HIDDEN);
	lv_obj_remove_flag(turn_panel, LV_OBJ_FLAG_HIDDEN);
}

bool ui_map_next_turn(char *buf, size_t len)
{
	if (lv_obj_has_flag(turn_panel, LV_OBJ_FLAG_HIDDEN))
		return false;
	snprintf(buf, len, "%s: %s", lv_label_get_text(lbl_turn_dist),
		 lv_label_get_text(lbl_turn_road));
	return true;
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
static void show_next_turn(const struct hbas_route_pos *pos)
{
	(void)pos;
}
bool ui_map_next_turn(char *buf, size_t len)
{
	(void)buf; (void)len;
	return false;
}
static void search(const char *query)
{
	(void)query;
}

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
	const struct hbas_place *pl = &places.place[menu_place];

	switch (items[i].kind) {
	case IT_STOP: snprintf(buf, len, "Stop route to %s", dest.name); break;
	case IT_FIND: snprintf(buf, len, "Find address"); break;
	case IT_PLACE: snprintf(buf, len, "%s", places.place[items[i].place].name); break;
	case IT_SAVE_HERE: snprintf(buf, len, "Save this spot"); break;
	case IT_MODE: snprintf(buf, len, "Route: < %s >", mode_names[places.route]); break;
	case IT_ORIENT: snprintf(buf, len, "Map: %s", heading_up ? "Heading up" : "North up"); break;
	case IT_P_GO: snprintf(buf, len, "Go to %s", pl->name); break;
	case IT_P_RENAME: snprintf(buf, len, "Rename"); break;
	case IT_P_DELETE: snprintf(buf, len, "Delete"); break;
	case IT_R_GO: snprintf(buf, len, "Go to %s", found.label[found.picked]); break;
	case IT_R_SAVE_GO: snprintf(buf, len, "Save as a place and go"); break;
	case IT_R_SAVE: snprintf(buf, len, "Save as a place"); break;
	case IT_BACK: snprintf(buf, len, "Back"); break;
	}
}

#define ADD(k, p) (items[n_items++] = (typeof(items[0])){ (k), (p) })

static void build_items(void)
{
	n_items = 0;
	switch (menu_mode) {
	case MENU_MAIN:
		if (dest.active || dest.busy)
			ADD(IT_STOP, -1);
		ADD(IT_FIND, -1);
		for (int i = 0; i < places.count; i++)
			ADD(IT_PLACE, i);
		ADD(IT_SAVE_HERE, -1);
		ADD(IT_MODE, -1);
		ADD(IT_ORIENT, -1);
		break;
	case MENU_PLACE:
		ADD(IT_P_GO, -1);
		ADD(IT_P_RENAME, -1);
		ADD(IT_P_DELETE, -1);
		ADD(IT_BACK, -1);
		break;
	case MENU_RESULT:
		ADD(IT_R_GO, -1);
		ADD(IT_R_SAVE_GO, -1);
		ADD(IT_R_SAVE, -1);
		ADD(IT_BACK, -1);
		break;
	}
	if (sel >= n_items)
		sel = n_items - 1;
}

static void show_menu(void)
{
	char text[96];

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

static void open_menu_mode(enum menu_mode m)
{
	menu_mode = m;
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

/* keyboard finished naming a place (new spot or rename) */
static void name_done(enum ui_kbd_result r, const char *text, int pick)
{
	(void)pick;
	if (r == UI_KBD_OK && text[0] && renaming >= 0 && renaming < places.count) {
		char name[HBAS_PLACE_NAME];
		size_t n;

		snprintf(name, sizeof(name), "%s", text);
		n = strlen(name);
		while (n && name[n - 1] == ' ')
			name[--n] = '\0';
		if (n) {
			/* same cleaning as a new place (no commas or newlines) */
			struct hbas_places tmp;

			hbas_places_init(&tmp);
			hbas_places_add(&tmp, name, 0, 0);
			snprintf(places.place[renaming].name, HBAS_PLACE_NAME, "%s",
				 tmp.place[0].name);
			save_places();
		}
	}
	renaming = -1;
	open_menu_mode(MENU_MAIN);
}

static void rename_place(int i)
{
	renaming = i;
	close_menu();
	ui_kbd_open("Name this place", places.place[i].name, NULL, name_done);
}

/* keyboard finished the address search */
static void find_done(enum ui_kbd_result r, const char *text, int pick)
{
	(void)text;
	if (r == UI_KBD_OK && found.n > 0)
		pick = 0;                          /* OK takes the best match */
	if ((r == UI_KBD_PICK || r == UI_KBD_OK) && pick >= 0 && pick < found.n) {
		found.picked = pick;
		open_menu_mode(MENU_RESULT);
		return;
	}
	open_menu_mode(MENU_MAIN);
}

/* a search result's short name for the places list: up to the comma */
static int save_result(void)
{
	char name[HBAS_PLACE_NAME];
	const char *label = found.label[found.picked];
	const char *comma = strchr(label, ',');

	snprintf(name, sizeof(name), "%.*s", comma ? (int)(comma - label) : (int)strlen(label),
		 label);
	if (hbas_places_add(&places, name, found.lat[found.picked], found.lon[found.picked]) < 0) {
		show_route_banner("The saved places list is full");
		return -1;
	}
	save_places();
	return places.count - 1;
}

static void select_item(void)
{
	char name[HBAS_PLACE_NAME];
	int i;

	switch (items[sel].kind) {
	case IT_STOP:
		clear_route();
		close_menu();
		break;
	case IT_FIND:
		close_menu();
		found.n = 0;
		ui_kbd_open("Find address: town, street or both", "", search, find_done);
		break;
	case IT_PLACE:
		menu_place = items[sel].place;
		open_menu_mode(MENU_PLACE);
		break;
	case IT_SAVE_HERE:
		if (!gps.valid) {
			show_route_banner("Can't save a spot without a GPS fix");
			close_menu();
			break;
		}
		snprintf(name, sizeof(name), "Spot %d", places.count + 1);
		if (hbas_places_add(&places, name, gps.lat, gps.lon) < 0) {
			show_route_banner("The saved places list is full");
			close_menu();
			break;
		}
		save_places();
		rename_place(places.count - 1);    /* Cancel keeps "Spot N" */
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
	case IT_P_GO:
		route_to(places.place[menu_place].name, places.place[menu_place].lat,
			 places.place[menu_place].lon);
		close_menu();
		break;
	case IT_P_RENAME:
		rename_place(menu_place);
		break;
	case IT_P_DELETE:
		hbas_places_remove(&places, menu_place);
		save_places();
		open_menu_mode(MENU_MAIN);
		break;
	case IT_R_GO:
		snprintf(name, sizeof(name), "%s", found.label[found.picked]);
		route_to(name, found.lat[found.picked], found.lon[found.picked]);
		close_menu();
		break;
	case IT_R_SAVE_GO:
		if ((i = save_result()) >= 0)
			route_to(places.place[i].name, places.place[i].lat, places.place[i].lon);
		close_menu();
		break;
	case IT_R_SAVE:
		if (save_result() >= 0)
			open_menu_mode(MENU_MAIN);
		else
			close_menu();
		break;
	case IT_BACK:
		open_menu_mode(MENU_MAIN);
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
		else if (menu_mode != MENU_MAIN)
			open_menu_mode(MENU_MAIN);
		else
			close_menu();
		break;
	case UI_KEY_ENTER:
		select_item();
		break;
	case UI_KEY_BACK:
		if (menu_mode != MENU_MAIN)
			open_menu_mode(MENU_MAIN);
		else
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

	/* next turn: arrow, distance, what to do; destination and time left */
	turn_panel = lv_obj_create(p);
	lv_obj_remove_style_all(turn_panel);
	lv_obj_set_size(turn_panel, 310, 70);
	lv_obj_set_pos(turn_panel, 4, 24);
	lv_obj_set_style_bg_color(turn_panel, COL_BG, 0);
	lv_obj_set_style_bg_opa(turn_panel, LV_OPA_80, 0);
	lv_obj_set_style_radius(turn_panel, 6, 0);
	lv_obj_set_style_border_side(turn_panel, LV_BORDER_SIDE_LEFT, 0);
	lv_obj_set_style_border_width(turn_panel, 3, 0);
	lv_obj_set_style_border_color(turn_panel, COL_ACCENT, 0);
	lv_obj_remove_flag(turn_panel, LV_OBJ_FLAG_SCROLLABLE);
	for (int i = 0; i < 3; i++) {
		turn_line[i] = lv_line_create(turn_panel);
		lv_obj_set_pos(turn_line[i], 8, 4);
		lv_obj_set_style_line_width(turn_line[i], 6, 0);
		lv_obj_set_style_line_color(turn_line[i], COL_ACCENT, 0);
		lv_obj_set_style_line_rounded(turn_line[i], true, 0);
	}
	lbl_turn_dist = ui_label(turn_panel, &lv_font_montserrat_28, COL_TEXT, "");
	lv_obj_set_pos(lbl_turn_dist, 60, 2);
	lbl_turn_road = ui_label(turn_panel, &lv_font_montserrat_14, COL_TEXT, "");
	lv_obj_set_pos(lbl_turn_road, 60, 34);
	lv_obj_set_width(lbl_turn_road, 244);
	lv_label_set_long_mode(lbl_turn_road, LV_LABEL_LONG_DOT);
	lbl_turn_trip = ui_label(turn_panel, &lv_font_montserrat_14, COL_DIM, "");
	lv_obj_set_pos(lbl_turn_trip, 8, 51);
	lv_obj_set_width(lbl_turn_trip, 296);
	lv_label_set_long_mode(lbl_turn_trip, LV_LABEL_LONG_DOT);
	lv_obj_add_flag(turn_panel, LV_OBJ_FLAG_HIDDEN);

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

/* Each fix while following a route: count down, reroute, or arrive. */
static void follow_route(void)
{
	struct hbas_route_pos pos;
	char text[160];
	/* HDOP x ~5 m is a rough horizontal error for a u-blox 5/6 */
	double accuracy = gps.hdop > 0 ? gps.hdop * 5.0 : 0;

	switch (hbas_route_follow(&dest.follow, &dest.line, gps.lat, gps.lon, accuracy,
				  lv_tick_get(), &pos)) {
	case HBAS_ROUTE_ARRIVED:
		snprintf(text, sizeof(text), "Arrived at %s", dest.name);
		clear_route();
		show_route_banner(text);
		break;
	case HBAS_ROUTE_REROUTE:
		dest.rerouting = true;
		route_to(dest.name, dest.lat, dest.lon);
		break;
	case HBAS_ROUTE_ON:
		show_next_turn(&pos);
		break;
	}
}

void ui_map_set_metric(bool m)
{
	metric = m;
}

void ui_map_gps(const struct hbas_gps_view *v)
{
	bool was_valid = gps.valid;

	gps = *v;
	if (!gps.valid && was_valid)
		lv_label_set_text(lbl_note, "Waiting for a GPS fix...");
	request();
	if (gps.valid && dest.active && !dest.busy && dest.line.n >= 2)
		follow_route();
}

bool ui_map_key(enum ui_key key)
{
	if (menu_open)
		return menu_key(key);
	switch (key) {
	case UI_KEY_UP: if (level < LEVEL_MAX) level++; break;
	case UI_KEY_DOWN: if (level > LEVEL_MIN) level--; break;
	case UI_KEY_ENTER: open_menu_mode(MENU_MAIN); return true;
	default: return false;
	}
	show_info();
	request();
	return true;
}
