// SPDX-License-Identifier: MIT
#include "hbas/route.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define EARTH_M 6371000.0
#define RAD (M_PI / 180.0)

/* Metres east / north of (lat0, lon0): fine over a route segment's length. */
static void local_xy(double lat0, double lon0, double lat, double lon, double *x, double *y)
{
	*x = (lon - lon0) * RAD * EARTH_M * cos(lat0 * RAD);
	*y = (lat - lat0) * RAD * EARTH_M;
}

static double seg_len(const struct hbas_route_line *l, size_t i)
{
	double x, y;

	local_xy(l->lat[i], l->lon[i], l->lat[i + 1], l->lon[i + 1], &x, &y);
	return hypot(x, y);
}

void hbas_route_line_init(struct hbas_route_line *l, const double *lat, const double *lon,
			  size_t n)
{
	l->lat = lat;
	l->lon = lon;
	l->n = n;
	l->total_m = 0;
	for (size_t i = 0; i + 1 < n; i++)
		l->total_m += seg_len(l, i);
}

void hbas_route_locate(const struct hbas_route_line *l, double lat, double lon, size_t hint,
		       struct hbas_route_pos *p)
{
	double best = INFINITY, along = 0, best_along = 0;
	size_t best_i = 0;

	memset(p, 0, sizeof(*p));
	if (l->n == 0)
		return;
	if (l->n == 1) {
		double x, y;

		local_xy(lat, lon, l->lat[0], l->lon[0], &x, &y);
		p->off_m = hypot(x, y);
		return;
	}
	(void)hint;   /* routes are a few thousand points: a full scan is cheap */
	for (size_t i = 0; i + 1 < l->n; i++) {
		double bx, by, px, py, t, len2, dx, dy, d, len;

		/* segment a->b and the bike, in metres from a */
		local_xy(l->lat[i], l->lon[i], l->lat[i + 1], l->lon[i + 1], &bx, &by);
		local_xy(l->lat[i], l->lon[i], lat, lon, &px, &py);
		len2 = bx * bx + by * by;
		len = sqrt(len2);
		t = len2 > 0 ? (px * bx + py * by) / len2 : 0;
		t = t < 0 ? 0 : t > 1 ? 1 : t;
		dx = px - t * bx;
		dy = py - t * by;
		d = hypot(dx, dy);
		/* ties go to the later segment: further along the route */
		if (d <= best) {
			best = d;
			best_i = i;
			best_along = along + t * len;
		}
		along += len;
	}
	p->off_m = best;
	p->segment = best_i;
	p->done_m = best_along;
	p->remaining_m = l->total_m - best_along;
	if (p->remaining_m < 0)
		p->remaining_m = 0;
}

void hbas_route_follow_reset(struct hbas_route_follow *f)
{
	memset(f, 0, sizeof(*f));
}

enum hbas_route_action hbas_route_follow(struct hbas_route_follow *f,
					 const struct hbas_route_line *l, double lat, double lon,
					 double accuracy_m, uint32_t now_ms,
					 struct hbas_route_pos *p)
{
	double end_x, end_y, limit;

	hbas_route_locate(l, lat, lon, f->segment, p);
	f->segment = p->segment;
	if (l->n) {
		local_xy(lat, lon, l->lat[l->n - 1], l->lon[l->n - 1], &end_x, &end_y);
		if (hypot(end_x, end_y) <= HBAS_ROUTE_ARRIVED_M)
			return HBAS_ROUTE_ARRIVED;
	}
	/* a poor fix shouldn't look like a wrong turn */
	limit = HBAS_ROUTE_OFF_M + (accuracy_m > 0 ? accuracy_m : 0);
	if (p->off_m <= limit) {
		f->off_count = 0;
		return HBAS_ROUTE_ON;
	}
	if (++f->off_count < HBAS_ROUTE_OFF_FIXES)
		return HBAS_ROUTE_ON;
	if (f->rerouted_once && now_ms - f->last_reroute_ms < HBAS_ROUTE_RETRY_MS)
		return HBAS_ROUTE_ON;
	f->off_count = 0;
	f->rerouted_once = true;
	f->last_reroute_ms = now_ms;
	return HBAS_ROUTE_REROUTE;
}

int hbas_nav_distance(char *buf, size_t len, double m, bool metric)
{
	if (m < 0)
		m = 0;
	if (metric) {
		if (m < 300)
			return snprintf(buf, len, "%d m", (int)lround(m / 10) * 10);
		if (m < 1000)
			return snprintf(buf, len, "%d m", (int)lround(m / 50) * 50);
		if (m < 10000)
			return snprintf(buf, len, "%.1f km", m / 1000);
		return snprintf(buf, len, "%d km", (int)lround(m / 1000));
	}
	{
		double ft = m / 0.3048, mi = m / 1609.344;

		if (mi < 0.1)
			return snprintf(buf, len, "%d ft", (int)lround(ft / 50) * 50);
		if (mi < 10)
			return snprintf(buf, len, "%.1f mi", mi);
		return snprintf(buf, len, "%d mi", (int)lround(mi));
	}
}
