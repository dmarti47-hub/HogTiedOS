/* SPDX-License-Identifier: MIT */
#ifndef HBAS_PLACES_H
#define HBAS_PLACES_H

/*
 * Navigation's saved places and route option, kept in a small text file
 * next to the settings (written with hbas_settings_save, read with
 * hbas_settings_load):
 *
 *   # HogTiedOS places 1
 *   route=fastest
 *   place=43.031700,-87.916500,H-D Museum
 *
 * Unknown lines are skipped, so later versions can add to it.
 */
#include <stdbool.h>
#include <stddef.h>

#define HBAS_PLACES_MAX 32
#define HBAS_PLACE_NAME 32
#define HBAS_PLACES_TEXT_MAX (64 + HBAS_PLACES_MAX * (HBAS_PLACE_NAME + 32))

/* route options, in the order of hbas-map's enum hbas_route_mode */
enum { HBAS_ROUTE_OPTION_COUNT = 4 };
extern const char *const hbas_route_option_keys[HBAS_ROUTE_OPTION_COUNT];

struct hbas_place {
	char name[HBAS_PLACE_NAME];
	double lat, lon;
};

struct hbas_places {
	int route;                       /* 0..HBAS_ROUTE_OPTION_COUNT-1 */
	int count;
	struct hbas_place place[HBAS_PLACES_MAX];
};

void hbas_places_init(struct hbas_places *p);
/* Returns the number of places read, or -1 if text isn't a places file. */
int hbas_places_parse(const char *text, struct hbas_places *p);
/* Returns the length written, or -1 if it doesn't fit. */
int hbas_places_format(const struct hbas_places *p, char *buf, size_t len);
/* Adds a place (name trimmed to fit, commas and newlines replaced);
 * returns its index, or -1 if the list is full or lat/lon are invalid. */
int hbas_places_add(struct hbas_places *p, const char *name, double lat, double lon);
void hbas_places_remove(struct hbas_places *p, int i);

#endif
