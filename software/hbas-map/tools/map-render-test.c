// SPDX-License-Identifier: MIT
/* map-render-test DB STYLE FONT LAT LON LEVEL HEADING OUT.ppm [W H [DPI]]
 * Draws one frame through hbas/map.h and reports how long it took. */
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
	int w = argc > 10 ? atoi(argv[9]) : 400, h = argc > 10 ? atoi(argv[10]) : 240;
	double dpi = argc > 11 ? atof(argv[11]) : 96;
	char err[128];
	struct hbas_map *m;
	uint16_t *px;
	double t0, t1, t2;
	FILE *f;

	if (argc < 9) {
		fprintf(stderr, "usage: %s DB STYLE FONT LAT LON LEVEL HEADING OUT.ppm [W H [DPI]]\n",
			argv[0]);
		return 2;
	}
	t0 = now();
	if (!(m = hbas_map_open(argv[1], argv[2], argv[3], dpi, err, sizeof(err)))) {
		fprintf(stderr, "open: %s\n", err);
		return 1;
	}
	px = malloc((size_t)w * h * 2);
	t1 = now();
	if (hbas_map_render(m, atof(argv[4]), atof(argv[5]), atof(argv[6]), atof(argv[7]), px, w, h)) {
		fprintf(stderr, "render failed\n");
		return 1;
	}
	t2 = now();
	/* second render of the same spot: tiles cached */
	hbas_map_render(m, atof(argv[4]), atof(argv[5]), atof(argv[6]), atof(argv[7]), px, w, h);
	printf("open %.0f ms, first render %.0f ms, cached render %.0f ms\n",
	       (t1 - t0) * 1e3, (t2 - t1) * 1e3, (now() - t2) * 1e3);
	if (!(f = fopen(argv[8], "wb")))
		return 1;
	fprintf(f, "P6 %d %d 255\n", w, h);
	for (int i = 0; i < w * h; i++) {
		unsigned char rgb[3] = { (unsigned char)((px[i] >> 8) & 0xF8),
					 (unsigned char)((px[i] >> 3) & 0xFC),
					 (unsigned char)((px[i] << 3) & 0xF8) };
		fwrite(rgb, 1, 3, f);
	}
	fclose(f);
	hbas_map_close(m);
	free(px);
	return 0;
}
