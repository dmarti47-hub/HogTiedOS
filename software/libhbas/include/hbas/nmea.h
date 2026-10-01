/* SPDX-License-Identifier: MIT */
/*
 * NMEA 0183 from the u-blox G5/G6 GPS on UART2 (stock boot.sh,
 * docs/findings/GPS.md). Sentences the
 * receiver sends by default: RMC (time, position, speed, course), GGA (fix
 * quality, satellites used, altitude, HDOP), GSA (2D/3D, DOPs), GSV
 * (satellites in view with signal strength), VTG. Talker IDs GP/GN/GL/GA
 * are all accepted. A line counts only if its checksum is right.
 */
#ifndef HBAS_NMEA_H
#define HBAS_NMEA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HBAS_NMEA_MAX      83     /* longest legal sentence incl. CRLF */
#define HBAS_GPS_MAX_SATS  32

/* Stock boot.sh switches the receiver's UART from 9600 to 57600 with this. */
#define HBAS_UBX_PUBX41_57600 "$PUBX,41,1,0007,0003,57600,0*2B\r\n"

enum hbas_gps_fix { HBAS_FIX_NONE, HBAS_FIX_2D, HBAS_FIX_3D };

struct hbas_gps_sat {
	uint8_t prn;
	int8_t elevation;        /* degrees, -1 unknown */
	int16_t azimuth;         /* degrees, -1 unknown */
	int8_t snr;              /* dB-Hz, -1 = not tracked */
};

struct hbas_gps {
	/* RMC */
	bool valid;              /* RMC status A: position is good */
	bool have_time, have_date;
	uint8_t hour, min, sec;  /* UTC */
	uint16_t msec;
	uint16_t year;           /* 4 digits */
	uint8_t month, day;
	double lat, lon;         /* degrees, + = N / E */
	double speed_kmh;        /* over ground */
	double course;           /* degrees true, -1 unknown */
	/* GGA / GSA */
	uint8_t quality;         /* GGA: 0 none, 1 GPS, 2 DGPS, 6 estimated (dead reckoning) */
	uint8_t sats_used;
	double altitude_m;       /* above mean sea level */
	bool have_altitude;
	double hdop, pdop, vdop; /* 0 = unknown */
	enum hbas_gps_fix fix;
	/* GSV: satellites in view (collected over a GSV group) */
	uint8_t sats_in_view;
	unsigned nsats;
	struct hbas_gps_sat sat[HBAS_GPS_MAX_SATS];
	/* bookkeeping */
	unsigned sentences, bad_checksums;
	unsigned changes;        /* bumps on every accepted sentence */
	/* GSV group being collected */
	unsigned gsv_n;
	struct hbas_gps_sat gsv_sat[HBAS_GPS_MAX_SATS];
};

void hbas_gps_init(struct hbas_gps *g);

/* Verify "$...*HH": 1 if good, 0 if no/bad checksum. */
int hbas_nmea_checksum_ok(const char *line);

/*
 * Feed one line ("$GPRMC,...*HH", CR/LF optional). Returns 1 if it was a
 * sentence we use and it updated g, 0 if ignored (other sentence types),
 * -1 if malformed or the checksum is wrong (counted in bad_checksums).
 */
int hbas_nmea_feed(struct hbas_gps *g, const char *line);

/* 16-point compass name for a course ("N", "NNE", ... ; "--" if unknown). */
const char *hbas_compass_point(double course);

/*
 * Seconds since 1970 (UTC) from RMC date+time; false until both are known.
 * Dates before HBAS_GPS_EARLIEST_YEAR are taken as GPS week-number rollover
 * (u-blox 5 era firmware) and moved forward by 1024 weeks.
 */
#define HBAS_GPS_EARLIEST_YEAR 2025
bool hbas_gps_unix_time(const struct hbas_gps *g, int64_t *t);

#endif
