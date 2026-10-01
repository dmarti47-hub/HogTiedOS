// SPDX-License-Identifier: MIT
#include "hbas/gpsproto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hbas/btproto.h"            /* shared one-line framing */

static const char *const fix_names[] = { "none", "2d", "3d" };

int hbas_gps_format(char *buf, size_t len, const struct hbas_gps *g, bool link)
{
	char lat[24], lon[24], spd[16], crs[16], alt[16], used[8], view[8], hdop[12], tm[24],
	     q[8], sats[HBAS_BT_TEXT_MAX];
	int64_t t = 0;
	size_t n = 0;

	snprintf(lat, sizeof(lat), "%.6f", g->lat);
	snprintf(lon, sizeof(lon), "%.6f", g->lon);
	snprintf(spd, sizeof(spd), "%.1f", g->speed_kmh);
	snprintf(crs, sizeof(crs), "%.1f", g->course);
	if (g->have_altitude)
		snprintf(alt, sizeof(alt), "%.1f", g->altitude_m);
	else
		alt[0] = '\0';
	snprintf(used, sizeof(used), "%u", g->sats_used);
	snprintf(view, sizeof(view), "%u", g->sats_in_view);
	snprintf(hdop, sizeof(hdop), "%.1f", g->hdop);
	snprintf(q, sizeof(q), "%u", g->quality);
	if (!g->valid || !hbas_gps_unix_time(g, &t))
		t = 0;
	snprintf(tm, sizeof(tm), "%lld", (long long)t);
	/* satellites, strongest signal first, as many as fit in one value */
	struct hbas_gps_sat by_snr[HBAS_GPS_MAX_SATS];
	unsigned ns = g->nsats < HBAS_GPS_MAX_SATS ? g->nsats : HBAS_GPS_MAX_SATS;

	memcpy(by_snr, g->sat, ns * sizeof(by_snr[0]));
	for (unsigned i = 1; i < ns; i++)               /* insertion sort, stable */
		for (unsigned j = i; j > 0 && by_snr[j].snr > by_snr[j - 1].snr; j--) {
			struct hbas_gps_sat t = by_snr[j];

			by_snr[j] = by_snr[j - 1];
			by_snr[j - 1] = t;
		}
	sats[0] = '\0';
	for (unsigned i = 0; i < ns; i++) {
		char one[24];
		int w = snprintf(one, sizeof(one), "%s%u:%d", n ? " " : "", by_snr[i].prn,
				 by_snr[i].snr);

		if (n + (size_t)w >= sizeof(sats))
			break;
		memcpy(sats + n, one, (size_t)w + 1);
		n += (size_t)w;
	}
	return hbas_bt_format(buf, len, "gps", "link", link ? "1" : "0", "valid", g->valid ? "1" : "0",
			      "fix", fix_names[g->fix <= HBAS_FIX_3D ? g->fix : 0], "quality", q,
			      "lat", lat, "lon", lon, "speed", spd, "course", crs, "alt", alt,
			      "used", used, "view", view, "hdop", hdop, "time", tm, "sats", sats, NULL);
}

void hbas_gps_view_init(struct hbas_gps_view *v)
{
	memset(v, 0, sizeof(*v));
	v->course = -1;
}

/* The shared parser keeps HBAS_BT_ARGS_MAX pairs; gps lines have more, so
 * parse key=value pairs here directly. */
static const char *field(const char *line, const char *key, char *out, size_t cap)
{
	size_t kl = strlen(key);
	const char *p = line;

	while ((p = strstr(p, key))) {
		if ((p == line || p[-1] == ' ') && p[kl] == '=') {
			const char *v = p + kl + 1;
			size_t n = 0;

			if (*v == '"') {
				for (v++; *v && *v != '"' && n + 1 < cap; v++) {
					if (*v == '\\' && v[1])
						v++;
					out[n++] = *v;
				}
			} else {
				while (*v && *v != ' ' && *v != '\n' && *v != '\r' && n + 1 < cap)
					out[n++] = *v++;
			}
			out[n] = '\0';
			return out;
		}
		p += kl;
	}
	return NULL;
}

static double dnum(const char *line, const char *key, double dflt, bool *ok)
{
	char b[32], *end;
	double x;

	if (ok)
		*ok = false;
	if (!field(line, key, b, sizeof(b)) || !b[0])
		return dflt;
	x = strtod(b, &end);
	if (*end)
		return dflt;
	if (ok)
		*ok = true;
	return x;
}

bool hbas_gps_view_apply(struct hbas_gps_view *v, const char *line)
{
	char b[HBAS_BT_TEXT_MAX];
	const char *p;

	if (strncmp(line, "gps ", 4))
		return false;
	v->link = dnum(line, "link", 0, NULL) != 0;
	v->valid = dnum(line, "valid", 0, NULL) != 0;
	v->fix = HBAS_FIX_NONE;
	if (field(line, "fix", b, sizeof(b)))
		for (int i = 0; i < 3; i++)
			if (!strcmp(b, fix_names[i]))
				v->fix = (enum hbas_gps_fix)i;
	v->quality = (uint8_t)dnum(line, "quality", 0, NULL);
	v->lat = dnum(line, "lat", 0, NULL);
	v->lon = dnum(line, "lon", 0, NULL);
	v->speed_kmh = dnum(line, "speed", 0, NULL);
	v->course = dnum(line, "course", -1, NULL);
	v->alt_m = dnum(line, "alt", 0, &v->have_alt);
	v->used = (uint8_t)dnum(line, "used", 0, NULL);
	v->view = (uint8_t)dnum(line, "view", 0, NULL);
	v->hdop = dnum(line, "hdop", 0, NULL);
	v->time = (int64_t)dnum(line, "time", 0, NULL);
	v->nsats = 0;
	if (field(line, "sats", b, sizeof(b))) {
		for (p = b; *p && v->nsats < HBAS_GPS_MAX_SATS;) {
			int prn, snr, used;

			if (sscanf(p, "%d:%d%n", &prn, &snr, &used) != 2)
				break;
			v->sat[v->nsats++] = (struct hbas_gps_sat){ (uint8_t)prn, -1, -1, (int8_t)snr };
			p += used;
			while (*p == ' ')
				p++;
		}
	}
	v->changes++;
	return true;
}
