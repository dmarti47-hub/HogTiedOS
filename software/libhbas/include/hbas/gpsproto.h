/* SPDX-License-Identifier: MIT */
/*
 * hbas-gpsd -> hogtied-ui: one line per second on a Unix socket
 * (/run/hbas/gps.sock), same text framing as btproto.h:
 *
 *   gps link=0|1 valid=0|1 fix=none|2d|3d quality=N lat=DEG lon=DEG
 *       speed=KMH course=DEG|-1 alt=M|"" used=N view=N hdop=X time=UNIX|0
 *       sats="PRN:SNR ..."     (strongest first, as many as fit)
 *
 * link=0 means the daemon hears nothing valid from the receiver. SNR -1 =
 * in view but not tracked.
 */
#ifndef HBAS_GPSPROTO_H
#define HBAS_GPSPROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hbas/nmea.h"

#define HBAS_GPS_LINE_MAX 640

/* What the UI shows. */
struct hbas_gps_view {
	bool daemon;                 /* connected to hbas-gpsd */
	bool link;                   /* receiver talking */
	bool valid;
	enum hbas_gps_fix fix;
	uint8_t quality, used, view;
	double lat, lon, speed_kmh, course, alt_m, hdop;
	bool have_alt;
	int64_t time;                /* UTC seconds, 0 = unknown */
	unsigned nsats;
	struct hbas_gps_sat sat[HBAS_GPS_MAX_SATS];
	unsigned changes;
};

/* Daemon side: the line for the current state (link = receiver alive). */
int hbas_gps_format(char *buf, size_t len, const struct hbas_gps *g, bool link);

void hbas_gps_view_init(struct hbas_gps_view *v);
/* UI side: apply one line; true if it was a gps line. */
bool hbas_gps_view_apply(struct hbas_gps_view *v, const char *line);

#endif
