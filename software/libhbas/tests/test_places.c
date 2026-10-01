// SPDX-License-Identifier: MIT
#include "hbas/places.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK(%s) failed\n", \
	__FILE__, __LINE__, #c); failures++; } } while (0)

static void test_roundtrip(void)
{
	struct hbas_places a, b;
	char buf[HBAS_PLACES_TEXT_MAX];

	hbas_places_init(&a);
	a.route = 3;
	CHECK(hbas_places_add(&a, "Home", 43.0389, -87.9065) == 0);
	CHECK(hbas_places_add(&a, "Devil's Lake, WI", 43.4147, -89.73) == 1);
	CHECK(!strcmp(a.place[1].name, "Devil's Lake  WI"));        /* no commas */
	CHECK(hbas_places_format(&a, buf, sizeof(buf)) > 0);
	CHECK(hbas_places_parse(buf, &b) == 2);
	CHECK(b.route == 3);
	CHECK(!strcmp(b.place[0].name, "Home") && fabs(b.place[0].lat - 43.0389) < 1e-6);
	CHECK(fabs(b.place[1].lon + 89.73) < 1e-6);
}

static void test_bad_input(void)
{
	struct hbas_places p;

	CHECK(hbas_places_parse("route=fastest\n", &p) < 0);           /* no header */
	CHECK(hbas_places_parse("# HogTiedOS places 1\nroute=sideways\nplace=x,y,Bad\n"
				"place=91,0,Pole\nplace=1,2,Ok\nfuture=1\n", &p) == 1);
	CHECK(p.route == 0 && !strcmp(p.place[0].name, "Ok"));
	hbas_places_init(&p);
	for (int i = 0; i < HBAS_PLACES_MAX; i++)
		CHECK(hbas_places_add(&p, "x", 1, 1) == i);
	CHECK(hbas_places_add(&p, "full", 1, 1) < 0);
	hbas_places_remove(&p, 0);
	CHECK(p.count == HBAS_PLACES_MAX - 1);
	CHECK(hbas_places_add(&p, "", 1, 1) == HBAS_PLACES_MAX - 1);
	CHECK(!strcmp(p.place[HBAS_PLACES_MAX - 1].name, "Place"));
	{
		char buf[HBAS_PLACES_TEXT_MAX];

		for (int i = 0; i < p.count; i++)
			memset(p.place[i].name, 'n', HBAS_PLACE_NAME - 1);
		CHECK(hbas_places_format(&p, buf, sizeof(buf)) > 0);    /* full list fits */
	}
}

int main(void)
{
	test_roundtrip();
	test_bad_input();
	if (failures)
		fprintf(stderr, "%d failure(s)\n", failures);
	return failures != 0;
}
