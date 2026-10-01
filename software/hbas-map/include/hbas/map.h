/* SPDX-License-Identifier: MIT */
/*
 * Offline map drawing for hogtied-ui: a small C interface over libosmscout
 * (database + AGG painter), so the C UI can show a map without C++.
 * Maps are built with tools/maps/build_map.sh, styled by
 * software/maps/hogtied.oss (docs/NAVIGATION.md).
 *
 * One hbas_map is used from one thread at a time (the UI's map worker).
 */
#ifndef HBAS_MAP_H
#define HBAS_MAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct hbas_map;

/*
 * Open a map database directory with a stylesheet (.oss) and a TrueType
 * font for labels. dpi scales line widths and text given in mm by the
 * style. Returns NULL (with a reason in err) on failure.
 */
struct hbas_map *hbas_map_open(const char *db_dir, const char *style, const char *font,
			       double dpi, char *err, size_t errlen);
void hbas_map_close(struct hbas_map *m);

/*
 * Draw w x h pixels of map into rgb565 (row-major, no padding), centred on
 * lat/lon. level is a web-map zoom level (0 = world, 16 = streets); may be
 * fractional. rotation_deg turns the map so that bearing is up (0 =
 * north-up; pass the heading for heading-up). Returns 0, or -1 if nothing
 * could be drawn.
 */
int hbas_map_render(struct hbas_map *m, double lat, double lon, double level,
		    double rotation_deg, uint16_t *rgb565, int w, int h);

/*
 * Routing (libosmscout's router over the same map). The modes are the
 * rider's route options; all are for a motor vehicle on public roads.
 */
enum hbas_route_mode {
	HBAS_ROUTE_FASTEST,
	HBAS_ROUTE_SHORTEST,
	HBAS_ROUTE_NO_HIGHWAYS,         /* no motorways (interstates, freeways) */
	HBAS_ROUTE_BACKROADS,           /* prefers secondary and country roads */
	HBAS_ROUTE_MODES
};

const char *hbas_route_mode_name(enum hbas_route_mode mode);

struct hbas_route_info {
	double distance_m;
	double duration_s;              /* at typical road speeds */
	size_t points;
};

/*
 * Calculate a route from one position to another; heading_deg (or < 0 for
 * unknown) is the direction the bike is going, so the route starts that
 * way. On success the route becomes the one drawn (orange) by
 * hbas_map_render, and 0 is returned; otherwise -1 with a reason in err,
 * and any earlier route stays. Takes from milliseconds to seconds, so call
 * it from the map's thread, not the UI's.
 */
int hbas_map_route(struct hbas_map *m, double from_lat, double from_lon, double heading_deg,
		   double to_lat, double to_lon, enum hbas_route_mode mode,
		   struct hbas_route_info *info, char *err, size_t errlen);
void hbas_map_clear_route(struct hbas_map *m);
/*
 * Turn-by-turn: the manoeuvres along the current route, in order, the last
 * one HBAS_TURN_ARRIVE. dist_m is from the start of the route.
 */
enum hbas_turn {
	HBAS_TURN_STRAIGHT,
	HBAS_TURN_SLIGHT_LEFT, HBAS_TURN_LEFT, HBAS_TURN_SHARP_LEFT,
	HBAS_TURN_SLIGHT_RIGHT, HBAS_TURN_RIGHT, HBAS_TURN_SHARP_RIGHT,
	HBAS_TURN_ROUNDABOUT,           /* exit: which exit */
	HBAS_TURN_MOTORWAY_ENTER,       /* onto name */
	HBAS_TURN_MOTORWAY_EXIT_LEFT,   /* exit ramp */
	HBAS_TURN_MOTORWAY_EXIT_RIGHT,
	HBAS_TURN_KEEP_LEFT,            /* motorway to motorway */
	HBAS_TURN_KEEP_RIGHT,
	HBAS_TURN_ARRIVE,
};
#define HBAS_STEP_NAME 48
struct hbas_route_step {
	enum hbas_turn turn;
	int exit;
	double dist_m;
	double lat, lon;
	char name[HBAS_STEP_NAME];      /* road taken ("Main Street", "I 94"); may be "" */
};
/* Copy the current route's steps (up to max); returns how many it has. */
size_t hbas_map_route_steps(const struct hbas_map *m, struct hbas_route_step *out, size_t max);

/* Copy the current route's points (up to max); returns how many it has. */
size_t hbas_map_route_points(const struct hbas_map *m, double *lat, double *lon, size_t max);

/*
 * Address / place search in the map's location index, for the destination
 * keyboard's autofill. query is free text ("canal st milwaukee",
 * "400 w canal", "devils lake"); partial words match. Results are the best
 * matches, nearest to near_lat/near_lon first among equally good ones.
 * Returns how many were written to out (up to max), or -1 on error.
 */
#define HBAS_SEARCH_LABEL 64
struct hbas_search_result {
	char label[HBAS_SEARCH_LABEL];  /* "400 W Canal Street, Milwaukee" */
	double lat, lon;
};
int hbas_map_search(struct hbas_map *m, const char *query, double near_lat, double near_lon,
		    struct hbas_search_result *out, int max);

/* Where lat/lon falls in the last render, in pixels; false if not rendered yet. */
bool hbas_map_to_pixel(const struct hbas_map *m, double lat, double lon, double *x, double *y);

#ifdef __cplusplus
}
#endif

#endif
