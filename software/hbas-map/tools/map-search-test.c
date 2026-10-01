// SPDX-License-Identifier: MIT
/* map-search-test DB STYLE FONT NEAR_LAT NEAR_LON QUERY...
 * Runs the destination search for each query and prints the results and
 * how long it took. */
#include "hbas/map.h"

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
	struct hbas_search_result r[8];
	struct hbas_map *m;
	char err[128];

	if (argc < 7) {
		fprintf(stderr, "usage: %s DB STYLE FONT NEAR_LAT NEAR_LON QUERY...\n", argv[0]);
		return 2;
	}
	if (!(m = hbas_map_open(argv[1], argv[2], argv[3], 96, err, sizeof(err)))) {
		fprintf(stderr, "open: %s\n", err);
		return 1;
	}
	for (int q = 6; q < argc; q++) {
		double t0 = now();
		int n = hbas_map_search(m, argv[q], atof(argv[4]), atof(argv[5]), r, 8);

		printf("\"%s\": %d (%.0f ms)\n", argv[q], n, (now() - t0) * 1e3);
		for (int i = 0; i < n; i++)
			printf("   %-50s %.5f %.5f\n", r[i].label, r[i].lat, r[i].lon);
	}
	hbas_map_close(m);
	return 0;
}
