/* SPDX-License-Identifier: MIT */
#ifndef HBAS_ROUTE_H
#define HBAS_ROUTE_H

/*
 * Following a route: where the bike is along it, and when to recalculate
 * (automatic rerouting) or say "arrived". Pure geometry and timing, so it
 * can be tested without a map; the route itself comes from hbas-map.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HBAS_ROUTE_OFF_M        50.0   /* further than this from the line is "off" */
#define HBAS_ROUTE_OFF_FIXES    3      /* ... for this many fixes in a row */
#define HBAS_ROUTE_RETRY_MS     10000  /* no more often than this */
#define HBAS_ROUTE_ARRIVED_M    30.0

struct hbas_route_line {
	const double *lat, *lon;
	size_t n;
	double total_m;                 /* filled by hbas_route_line_init */
};

struct hbas_route_pos {
	double off_m;                   /* distance from the line */
	double done_m;                  /* along the line to the nearest point */
	double remaining_m;             /* from there to the end */
	size_t segment;                 /* nearest segment (i .. i+1) */
};

void hbas_route_line_init(struct hbas_route_line *l, const double *lat, const double *lon,
			  size_t n);
/* Nearest point on the line. hint: the last segment, or 0 (unused for
 * now: routes are a few thousand points, a full scan is cheap). */
void hbas_route_locate(const struct hbas_route_line *l, double lat, double lon, size_t hint,
		       struct hbas_route_pos *p);

enum hbas_route_action { HBAS_ROUTE_ON, HBAS_ROUTE_REROUTE, HBAS_ROUTE_ARRIVED };

struct hbas_route_follow {
	int off_count;
	uint32_t last_reroute_ms;
	bool rerouted_once;
	size_t segment;
};

void hbas_route_follow_reset(struct hbas_route_follow *f);
/* Feed one GPS fix (accuracy_m: the fix's horizontal error, 0 if unknown). */
enum hbas_route_action hbas_route_follow(struct hbas_route_follow *f,
					 const struct hbas_route_line *l, double lat, double lon,
					 double accuracy_m, uint32_t now_ms,
					 struct hbas_route_pos *p);

#endif
