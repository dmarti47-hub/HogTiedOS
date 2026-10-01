// SPDX-License-Identifier: MIT
#include "hbas/route.h"

#include <math.h>
#include <stdio.h>

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK(%s) failed\n", \
	__FILE__, __LINE__, #c); failures++; } } while (0)

/* an L: 1 km north, then 1 km east (at 43 N) */
#define DLAT (1000.0 / 111194.9)
#define DLON (1000.0 / (111194.9 * 0.7313537))
static const double lat[] = { 43.0, 43.0 + DLAT, 43.0 + DLAT };
static const double lon[] = { -88.0, -88.0, -88.0 + DLON };

static void test_locate(void)
{
	struct hbas_route_line l;
	struct hbas_route_pos p;

	hbas_route_line_init(&l, lat, lon, 3);
	CHECK(fabs(l.total_m - 2000) < 2);
	/* halfway up the first leg, 20 m east of it */
	hbas_route_locate(&l, 43.0 + DLAT / 2, -88.0 + DLON / 50, 0, &p);
	CHECK(fabs(p.off_m - 20) < 0.5);
	CHECK(fabs(p.done_m - 500) < 1 && fabs(p.remaining_m - 1500) < 2);
	CHECK(p.segment == 0);
	/* on the second leg */
	hbas_route_locate(&l, 43.0 + DLAT, -88.0 + DLON * 0.75, 0, &p);
	CHECK(p.segment == 1 && p.off_m < 0.5 && fabs(p.remaining_m - 250) < 1);
	/* before the start: distance to the first point */
	hbas_route_locate(&l, 43.0 - DLAT / 10, -88.0, 0, &p);
	CHECK(fabs(p.off_m - 100) < 0.5 && p.done_m == 0);
}

static void test_follow(void)
{
	struct hbas_route_line l;
	struct hbas_route_follow f;
	struct hbas_route_pos p;
	double off_lon = -88.0 + DLON / 10;          /* 100 m east of the first leg */

	hbas_route_line_init(&l, lat, lon, 3);
	hbas_route_follow_reset(&f);
	CHECK(hbas_route_follow(&f, &l, 43.0 + DLAT / 4, -88.0, 5, 1000, &p) == HBAS_ROUTE_ON);
	/* off by 100 m: reroute only on the third fix in a row */
	CHECK(hbas_route_follow(&f, &l, 43.0 + DLAT / 4, off_lon, 5, 2000, &p) == HBAS_ROUTE_ON);
	CHECK(hbas_route_follow(&f, &l, 43.0 + DLAT / 4, off_lon, 5, 3000, &p) == HBAS_ROUTE_ON);
	CHECK(hbas_route_follow(&f, &l, 43.0 + DLAT / 4, off_lon, 5, 4000, &p) == HBAS_ROUTE_REROUTE);
	/* still off (new route not in yet): not again within 10 s */
	for (uint32_t t = 5000; t < 14000; t += 1000)
		CHECK(hbas_route_follow(&f, &l, 43.0 + DLAT / 4, off_lon, 5, t, &p) == HBAS_ROUTE_ON);
	CHECK(hbas_route_follow(&f, &l, 43.0 + DLAT / 4, off_lon, 5, 14000, &p) == HBAS_ROUTE_REROUTE);
	/* one bad fix in between resets the count */
	hbas_route_follow_reset(&f);
	hbas_route_follow(&f, &l, 43.0 + DLAT / 4, off_lon, 5, 1000, &p);
	hbas_route_follow(&f, &l, 43.0 + DLAT / 4, off_lon, 5, 2000, &p);
	hbas_route_follow(&f, &l, 43.0 + DLAT / 4, -88.0, 5, 3000, &p);
	CHECK(hbas_route_follow(&f, &l, 43.0 + DLAT / 4, off_lon, 5, 4000, &p) == HBAS_ROUTE_ON);
	/* a poor fix (60 m error) widens the corridor */
	hbas_route_follow_reset(&f);
	for (uint32_t t = 0; t < 5000; t += 1000)
		CHECK(hbas_route_follow(&f, &l, 43.0 + DLAT / 4, off_lon, 60, t, &p) == HBAS_ROUTE_ON);
	/* arrived within 30 m of the end */
	CHECK(hbas_route_follow(&f, &l, 43.0 + DLAT, -88.0 + DLON * 0.98, 5, 6000, &p) ==
	      HBAS_ROUTE_ARRIVED);
}

int main(void)
{
	test_locate();
	test_follow();
	if (failures)
		fprintf(stderr, "%d failure(s)\n", failures);
	return failures != 0;
}
