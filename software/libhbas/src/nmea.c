// SPDX-License-Identifier: MIT
#include "hbas/nmea.h"

#include <stdlib.h>
#include <string.h>

#define FIELDS_MAX 24

void hbas_gps_init(struct hbas_gps *g)
{
	memset(g, 0, sizeof(*g));
	g->course = -1;
}

static int hexval(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	return -1;
}

int hbas_nmea_checksum_ok(const char *line)
{
	const char *p = line;
	unsigned sum = 0;
	int hi, lo;

	if (*p++ != '$')
		return 0;
	while (*p && *p != '*') {
		if ((unsigned char)*p < 0x20 || (unsigned char)*p > 0x7e)
			return 0;
		sum ^= (unsigned char)*p++;
	}
	if (*p != '*' || (hi = hexval(p[1])) < 0 || (lo = hexval(p[2])) < 0)
		return 0;
	return (unsigned)(hi << 4 | lo) == sum;
}

/* Split the body between '$' and '*' at commas (in place). */
static int split(char *s, char *f[FIELDS_MAX])
{
	int n = 0;

	f[n++] = s;
	for (; *s && *s != '*'; s++) {
		if (*s == ',') {
			*s = '\0';
			if (n == FIELDS_MAX)
				return -1;
			f[n++] = s + 1;
		}
	}
	*s = '\0';
	return n;
}

static bool num(const char *s, double *out)
{
	char *end;

	if (!*s)
		return false;
	*out = strtod(s, &end);
	return *end == '\0';
}

static int inum(const char *s, int dflt)
{
	char *end;
	long v;

	if (!*s)
		return dflt;
	v = strtol(s, &end, 10);
	return *end ? dflt : (int)v;
}

/* "ddmm.mmmm" + hemisphere -> signed degrees */
static bool coord(const char *v, const char *hemi, int deg_digits, double *out)
{
	double x, deg, min;

	if (!num(v, &x) || strlen(v) < (size_t)deg_digits + 2)
		return false;
	deg = (double)(long)(x / 100.0);
	min = x - deg * 100.0;
	if (min < 0 || min >= 60.0)
		return false;
	*out = deg + min / 60.0;
	if (!strcmp(hemi, "S") || !strcmp(hemi, "W"))
		*out = -*out;
	else if (strcmp(hemi, "N") && strcmp(hemi, "E"))
		return false;
	return true;
}

static bool parse_time(struct hbas_gps *g, const char *t)
{
	double sec;

	if (strlen(t) < 6 || !num(t, &sec))
		return false;
	g->hour = (uint8_t)((t[0] - '0') * 10 + (t[1] - '0'));
	g->min = (uint8_t)((t[2] - '0') * 10 + (t[3] - '0'));
	sec = strtod(t + 4, NULL);
	g->sec = (uint8_t)sec;
	g->msec = (uint16_t)((sec - g->sec) * 1000.0 + 0.5);
	if (g->hour > 23 || g->min > 59 || g->sec > 60)
		return false;
	g->have_time = true;
	return true;
}

static void rmc(struct hbas_gps *g, char **f, int n)
{
	double v;

	if (n < 10)
		return;
	parse_time(g, f[1]);
	g->valid = !strcmp(f[2], "A");
	if (g->valid) {
		coord(f[3], f[4], 2, &g->lat);
		coord(f[5], f[6], 3, &g->lon);
	}
	g->speed_kmh = num(f[7], &v) ? v * 1.852 : 0.0;          /* knots */
	g->course = num(f[8], &v) ? v : -1;
	if (strlen(f[9]) == 6) {
		g->day = (uint8_t)inum((char[]){ f[9][0], f[9][1], 0 }, 0);
		g->month = (uint8_t)inum((char[]){ f[9][2], f[9][3], 0 }, 0);
		g->year = (uint16_t)(2000 + inum((char[]){ f[9][4], f[9][5], 0 }, 0));
		g->have_date = g->day >= 1 && g->day <= 31 && g->month >= 1 && g->month <= 12;
	}
}

static void gga(struct hbas_gps *g, char **f, int n)
{
	double v;

	if (n < 10)
		return;
	parse_time(g, f[1]);
	g->quality = (uint8_t)inum(f[6], 0);
	g->sats_used = (uint8_t)inum(f[7], 0);
	g->hdop = num(f[8], &v) ? v : 0;
	g->have_altitude = g->quality > 0 && num(f[9], &g->altitude_m);
	if (g->quality > 0) {
		coord(f[2], f[3], 2, &g->lat);
		coord(f[4], f[5], 3, &g->lon);
	}
}

static void gsa(struct hbas_gps *g, char **f, int n)
{
	double v;

	if (n < 18)
		return;
	switch (inum(f[2], 1)) {
	case 2: g->fix = HBAS_FIX_2D; break;
	case 3: g->fix = HBAS_FIX_3D; break;
	default: g->fix = HBAS_FIX_NONE; break;
	}
	g->pdop = num(f[15], &v) ? v : 0;
	g->hdop = num(f[16], &v) ? v : g->hdop;
	g->vdop = num(f[17], &v) ? v : 0;
}

/* GSV: total messages, message number, sats in view, then 4 x (prn, el, az, snr) */
static void gsv(struct hbas_gps *g, char **f, int n)
{
	int total = inum(f[1], 0), msg = inum(f[2], 0);

	if (n < 4 || total < 1 || msg < 1 || msg > total)
		return;
	if (msg == 1)
		g->gsv_n = 0;
	g->sats_in_view = (uint8_t)inum(f[3], 0);
	for (int i = 4; i < n; i += 4) {
		struct hbas_gps_sat s;

		if (i >= n || !*f[i] || g->gsv_n >= HBAS_GPS_MAX_SATS)
			break;
		s.prn = (uint8_t)inum(f[i], 0);
		s.elevation = (int8_t)(i + 1 < n ? inum(f[i + 1], -1) : -1);
		s.azimuth = (int16_t)(i + 2 < n ? inum(f[i + 2], -1) : -1);
		s.snr = (int8_t)(i + 3 < n ? inum(f[i + 3], -1) : -1);
		g->gsv_sat[g->gsv_n++] = s;
	}
	if (msg == total) {                     /* group complete */
		memcpy(g->sat, g->gsv_sat, sizeof(g->sat));
		g->nsats = g->gsv_n;
	}
}

static void vtg(struct hbas_gps *g, char **f, int n)
{
	double v;

	if (n < 8)
		return;
	if (num(f[1], &v))
		g->course = v;
	if (num(f[7], &v))
		g->speed_kmh = v;
}

int hbas_nmea_feed(struct hbas_gps *g, const char *line)
{
	char buf[HBAS_NMEA_MAX + 8], *f[FIELDS_MAX];
	size_t len = strcspn(line, "\r\n");
	int n;
	const char *type;

	if (len >= sizeof(buf) || len < 7 || line[0] != '$')
		return -1;
	memcpy(buf, line, len);
	buf[len] = '\0';
	if (!hbas_nmea_checksum_ok(buf)) {
		g->bad_checksums++;
		return -1;
	}
	if ((n = split(buf + 1, f)) < 1 || strlen(f[0]) != 5)
		return -1;
	type = f[0] + 2;                        /* skip the talker (GP, GN, GL, GA) */
	if (!strcmp(type, "RMC"))
		rmc(g, f, n);
	else if (!strcmp(type, "GGA"))
		gga(g, f, n);
	else if (!strcmp(type, "GSA"))
		gsa(g, f, n);
	else if (!strcmp(type, "GSV"))
		gsv(g, f, n);
	else if (!strcmp(type, "VTG"))
		vtg(g, f, n);
	else
		return 0;
	g->sentences++;
	g->changes++;
	return 1;
}

const char *hbas_compass_point(double course)
{
	static const char *const pt[16] = {
		"N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE",
		"S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW",
	};

	if (course < 0)
		return "--";
	return pt[(int)((course + 11.25) / 22.5) % 16];
}

/* days from 1970-01-01 to y-m-d (proleptic Gregorian) */
static int64_t days_from_civil(int y, unsigned m, unsigned d)
{
	int64_t era;
	unsigned yoe, doy, doe;

	y -= m <= 2;
	era = (y >= 0 ? y : y - 399) / 400;
	yoe = (unsigned)(y - era * 400);
	doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + (int64_t)doe - 719468;
}

bool hbas_gps_unix_time(const struct hbas_gps *g, int64_t *t)
{
	/* GPS week-number rollover (every 1024 weeks; last on 2019-04-06):
	 * older receivers then report dates 19.6 years early. Nothing we
	 * run on predates this software, so move such dates forward. */
	const int64_t earliest = days_from_civil(HBAS_GPS_EARLIEST_YEAR, 1, 1) * 86400;
	const int64_t rollover = 1024LL * 7 * 86400;

	if (!g->have_time || !g->have_date)
		return false;
	*t = days_from_civil(g->year, g->month, g->day) * 86400 +
	     g->hour * 3600 + g->min * 60 + g->sec;
	while (*t < earliest)
		*t += rollover;
	return true;
}
