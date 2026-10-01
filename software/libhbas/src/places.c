// SPDX-License-Identifier: MIT
#include "hbas/places.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HEADER "# HogTiedOS places 1\n"

const char *const hbas_route_option_keys[HBAS_ROUTE_OPTION_COUNT] = {
	"fastest", "shortest", "no-highways", "backroads",
};

void hbas_places_init(struct hbas_places *p)
{
	memset(p, 0, sizeof(*p));
}

static bool valid(double lat, double lon)
{
	return isfinite(lat) && isfinite(lon) && fabs(lat) <= 90 && fabs(lon) <= 180;
}

int hbas_places_add(struct hbas_places *p, const char *name, double lat, double lon)
{
	struct hbas_place *pl;

	if (p->count >= HBAS_PLACES_MAX || !valid(lat, lon))
		return -1;
	pl = &p->place[p->count];
	snprintf(pl->name, sizeof(pl->name), "%s", name && *name ? name : "Place");
	for (char *c = pl->name; *c; c++)
		if (*c == '\n' || *c == '\r' || *c == ',')
			*c = ' ';
	pl->lat = lat;
	pl->lon = lon;
	return p->count++;
}

void hbas_places_remove(struct hbas_places *p, int i)
{
	if (i < 0 || i >= p->count)
		return;
	memmove(&p->place[i], &p->place[i + 1], (size_t)(p->count - i - 1) * sizeof(p->place[0]));
	p->count--;
}

int hbas_places_parse(const char *text, struct hbas_places *p)
{
	const char *line = text;

	if (strncmp(text, HEADER, strlen(HEADER)))
		return -1;
	hbas_places_init(p);
	while (*line) {
		const char *end = strchr(line, '\n');
		size_t n = end ? (size_t)(end - line) : strlen(line);
		char buf[128];

		if (n < sizeof(buf)) {
			memcpy(buf, line, n);
			buf[n] = '\0';
			if (!strncmp(buf, "route=", 6)) {
				for (int i = 0; i < HBAS_ROUTE_OPTION_COUNT; i++)
					if (!strcmp(buf + 6, hbas_route_option_keys[i]))
						p->route = i;
			} else if (!strncmp(buf, "place=", 6)) {
				char *s = buf + 6, *e1, *e2;
				double lat = strtod(s, &e1), lon;

				if (*e1 == ',') {
					lon = strtod(e1 + 1, &e2);
					if (*e2 == ',' && e2 > e1 + 1 && e1 > s)
						hbas_places_add(p, e2 + 1, lat, lon);
				}
			}
		}
		line += n + (end ? 1 : 0);
	}
	return p->count;
}

int hbas_places_format(const struct hbas_places *p, char *buf, size_t len)
{
	int r = (p->route >= 0 && p->route < HBAS_ROUTE_OPTION_COUNT) ? p->route : 0;
	size_t off;
	int n = snprintf(buf, len, HEADER "route=%s\n", hbas_route_option_keys[r]);

	if (n < 0 || (size_t)n >= len)
		return -1;
	off = (size_t)n;
	for (int i = 0; i < p->count; i++) {
		n = snprintf(buf + off, len - off, "place=%.6f,%.6f,%s\n", p->place[i].lat,
			     p->place[i].lon, p->place[i].name);
		if (n < 0 || (size_t)n >= len - off)
			return -1;
		off += (size_t)n;
	}
	return (int)off;
}
