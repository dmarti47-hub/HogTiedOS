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

/* Where lat/lon falls in the last render, in pixels; false if not rendered yet. */
bool hbas_map_to_pixel(const struct hbas_map *m, double lat, double lon, double *x, double *y);

#ifdef __cplusplus
}
#endif

#endif
