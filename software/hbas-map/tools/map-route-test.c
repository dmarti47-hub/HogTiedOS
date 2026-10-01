// SPDX-License-Identifier: MIT
/* map-route-test DB STYLE FONT FROM_LAT FROM_LON TO_LAT TO_LON [OUT.ppm]
 * Calculates the route in every mode and reports distance, time and how
 * long it took; with OUT.ppm also draws the fastest route (whole route in
 * view). */
#include "hbas/map.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static double now(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
	double flat, flon, tlat, tlon;
	struct hbas_route_info info;
	struct hbas_map *m;
	char err[128];
	int rc = 0;

	if (argc < 8) {
		fprintf(stderr, "usage: %s DB STYLE FONT FROM_LAT FROM_LON TO_LAT TO_LON [OUT.ppm]\n",
			argv[0]);
		return 2;
	}
	flat = atof(argv[4]);
	flon = atof(argv[5]);
	tlat = atof(argv[6]);
	tlon = atof(argv[7]);
	if (!(m = hbas_map_open(argv[1], argv[2], argv[3], 96, err, sizeof(err)))) {
		fprintf(stderr, "open: %s\n", err);
		return 1;
	}
	for (int mode = HBAS_ROUTE_MODES - 1; mode >= 0; mode--) {   /* fastest last: it's drawn */
		double t0 = now();

		if (hbas_map_route(m, flat, flon, -1, tlat, tlon, mode, &info, err, sizeof(err))) {
			printf("%-12s failed: %s\n", hbas_route_mode_name(mode), err);
			rc = 1;
			continue;
		}
		printf("%-12s %6.1f km  %3.0f min  %5zu points  (%.0f ms)\n",
		       hbas_route_mode_name(mode), info.distance_m / 1000, info.duration_s / 60,
		       info.points, (now() - t0) * 1e3);
	}
	if (argc > 8 && !rc) {
		int w = 400, h = 240;
		uint16_t *px = malloc((size_t)w * h * 2);
		/* zoom so both ends fit: ~ level where the span covers the screen */
		double span = fmax(fabs(tlat - flat), fabs(tlon - flon) * cos(flat * M_PI / 180));
		double level = floor(log2(180.0 * 240 / 256 / (span * 1.3 + 1e-6)));
		FILE *f;

		hbas_map_render(m, (flat + tlat) / 2, (flon + tlon) / 2, level, 0, px, w, h);
		if ((f = fopen(argv[8], "wb"))) {
			fprintf(f, "P6 %d %d 255\n", w, h);
			for (int i = 0; i < w * h; i++) {
				unsigned char rgb[3] = { (unsigned char)((px[i] >> 8) & 0xF8),
							 (unsigned char)((px[i] >> 3) & 0xFC),
							 (unsigned char)((px[i] << 3) & 0xF8) };
				fwrite(rgb, 1, 3, f);
			}
			fclose(f);
		}
		free(px);
	}
	hbas_map_close(m);
	return rc;
}
