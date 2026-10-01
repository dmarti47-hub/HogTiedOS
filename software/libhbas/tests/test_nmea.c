// SPDX-License-Identifier: MIT
#include "hbas/nmea.h"
#include "hbas/gpsproto.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK(%s) failed\n", \
	__FILE__, __LINE__, #c); failures++; } } while (0)
#define NEAR(a, b, eps) (fabs((a) - (b)) < (eps))

static void test_checksums(void)
{
	/* stock boot.sh's baud switch is a valid sentence */
	CHECK(hbas_nmea_checksum_ok("$PUBX,41,1,0007,0003,57600,0*2B"));
	CHECK(!strncmp(HBAS_UBX_PUBX41_57600, "$PUBX,41,1,0007,0003,57600,0*2B\r\n", 34));
	CHECK(!hbas_nmea_checksum_ok("$PUBX,41,1,0007,0003,57600,0*2C"));
	CHECK(!hbas_nmea_checksum_ok("PUBX,41*2B"));
	CHECK(!hbas_nmea_checksum_ok("$GPRMC,no star"));
}

static void test_rmc_and_classic_example(void)
{
	struct hbas_gps g;

	hbas_gps_init(&g);
	/* the textbook RMC example: 48 07.038' N, 11 31.000' E, 22.4 kn, 84.4 deg */
	CHECK(hbas_nmea_feed(&g, "$GPRMC,123519.00,A,4807.038,N,01131.000,E,022.4,084.4,"
			     "230394,003.1,W*44\r\n") == 1);
	CHECK(g.valid && g.have_time && g.hour == 12 && g.min == 35 && g.sec == 19);
	CHECK(NEAR(g.lat, 48.1173, 1e-4) && NEAR(g.lon, 11.516667, 1e-5));
	CHECK(NEAR(g.speed_kmh, 22.4 * 1.852, 1e-6) && NEAR(g.course, 84.4, 1e-9));
	CHECK(g.have_date && g.year == 2094 && /* two-digit years are 20yy */ g.month == 3 && g.day == 23);
	/* western/northern, multi-GNSS talker */
	CHECK(hbas_nmea_feed(&g, "$GNRMC,201530.50,A,4228.8640,N,08337.4710,W,30.0,270.0,"
			     "011026,,,A*6B") == 1);
	CHECK(NEAR(g.lat, 42.481067, 1e-6) && NEAR(g.lon, -83.624517, 1e-6));
	CHECK(g.msec == 500 && !strcmp(hbas_compass_point(g.course), "W"));
	/* no fix: position kept but marked invalid */
	CHECK(hbas_nmea_feed(&g, "$GPRMC,201530.50,V,,,,,,,011026,,,N*79") == 1);
	CHECK(!g.valid && g.course < 0);
}

static void test_gga_gsa_gsv_vtg(void)
{
	struct hbas_gps g;

	hbas_gps_init(&g);
	CHECK(hbas_nmea_feed(&g, "$GPGGA,201530.50,4228.8640,N,08337.4710,W,1,08,0.9,250.5,M,"
			     "-34.0,M,,*5C") == 1);
	CHECK(g.quality == 1 && g.sats_used == 8 && g.have_altitude &&
	      NEAR(g.altitude_m, 250.5, 1e-9) && NEAR(g.hdop, 0.9, 1e-9));
	CHECK(hbas_nmea_feed(&g, "$GPGSA,A,3,04,05,,09,12,,,24,,,,,2.5,1.3,2.1*39") == 1);
	CHECK(g.fix == HBAS_FIX_3D && NEAR(g.pdop, 2.5, 1e-9) && NEAR(g.vdop, 2.1, 1e-9));
	/* GSV: the list only updates when the group is complete */
	CHECK(hbas_nmea_feed(&g, "$GPGSV,2,1,08,01,40,083,46,02,17,308,41,12,07,344,39,"
			     "14,22,228,45*75") == 1);
	CHECK(g.nsats == 0 && g.sats_in_view == 8);
	CHECK(hbas_nmea_feed(&g, "$GPGSV,2,2,08,15,60,120,,24,45,045,38,25,10,180,20,"
			     "29,05,300,*7D") == 1);
	CHECK(g.nsats == 8 && g.sat[0].prn == 1 && g.sat[0].snr == 46);
	CHECK(g.sat[4].prn == 15 && g.sat[4].snr == -1 && g.sat[4].elevation == 60);
	CHECK(g.sat[7].prn == 29 && g.sat[7].snr == -1);
	CHECK(hbas_nmea_feed(&g, "$GPVTG,054.7,T,034.4,M,005.5,N,010.2,K*48") == 1);
	CHECK(NEAR(g.course, 54.7, 1e-9) && NEAR(g.speed_kmh, 10.2, 1e-9));
	CHECK(!strcmp(hbas_compass_point(g.course), "NE"));
}

static void test_bad_input(void)
{
	struct hbas_gps g;

	hbas_gps_init(&g);
	CHECK(hbas_nmea_feed(&g, "$GPGGA,201530.50,4228.8640,N,08337.4710,W,1,08,0.9,250.5,M,"
			     "-34.0,M,,*5D") == -1);              /* one bit off */
	CHECK(g.bad_checksums == 1 && g.sentences == 0 && g.quality == 0);
	CHECK(hbas_nmea_feed(&g, "$GPTXT,01,01,02,u-blox ag*2A") == 0);   /* ignored */
	CHECK(hbas_nmea_feed(&g, "") == -1 && hbas_nmea_feed(&g, "garbage\xff") == -1);
	CHECK(hbas_nmea_feed(&g, "$GPGSV,0,1,08,*7F") == -1 || g.nsats == 0);
}

static void test_time_and_rollover(void)
{
	struct hbas_gps g;
	int64_t t;

	hbas_gps_init(&g);
	CHECK(!hbas_gps_unix_time(&g, &t));
	hbas_nmea_feed(&g, "$GNRMC,201530.50,A,4228.8640,N,08337.4710,W,30.0,270.0,011026,,,A*6B");
	CHECK(hbas_gps_unix_time(&g, &t) && t == 1790885730);   /* 2026-10-01 20:15:30 UTC */
	/* a u-blox 5 hit by the 2019 week rollover reports 2007-02-15 for 2026-10-01 */
	hbas_nmea_feed(&g, "$GPRMC,000000.00,A,4228.8640,N,08337.4710,W,0.0,,150207,,,A*68");
	CHECK(hbas_gps_unix_time(&g, &t) && t == 1790812800);   /* 2026-10-01 00:00:00 */
	CHECK(!strcmp(hbas_compass_point(-1), "--") && !strcmp(hbas_compass_point(359), "N"));
}

static void test_gps_line_roundtrip(void)
{
	struct hbas_gps g;
	struct hbas_gps_view v;
	char line[HBAS_GPS_LINE_MAX];

	hbas_gps_init(&g);
	hbas_nmea_feed(&g, "$GNRMC,201530.50,A,4228.8640,N,08337.4710,W,30.0,270.0,011026,,,A*6B");
	hbas_nmea_feed(&g, "$GPGGA,201530.50,4228.8640,N,08337.4710,W,1,08,0.9,250.5,M,-34.0,M,,*5C");
	hbas_nmea_feed(&g, "$GPGSA,A,3,04,05,,09,12,,,24,,,,,2.5,1.3,2.1*39");
	hbas_nmea_feed(&g, "$GPGSV,2,1,08,01,40,083,46,02,17,308,41,12,07,344,39,14,22,228,45*75");
	hbas_nmea_feed(&g, "$GPGSV,2,2,08,15,60,120,,24,45,045,38,25,10,180,20,29,05,300,*7D");
	CHECK(hbas_gps_format(line, sizeof(line), &g, true) > 0);
	hbas_gps_view_init(&v);
	CHECK(hbas_gps_view_apply(&v, line));
	CHECK(v.link && v.valid && v.fix == HBAS_FIX_3D && v.used == 8 && v.view == 8);
	CHECK(NEAR(v.lat, 42.481067, 1e-6) && NEAR(v.lon, -83.624517, 1e-6));
	CHECK(NEAR(v.speed_kmh, 55.6, 0.05) && NEAR(v.course, 270, 1e-9));
	CHECK(v.have_alt && NEAR(v.alt_m, 250.5, 1e-9) && v.time == 1790885730);
	/* strongest satellites first */
	CHECK(v.nsats >= 6 && v.sat[0].prn == 1 && v.sat[0].snr == 46 && v.sat[1].snr == 45);
	CHECK(v.sat[v.nsats - 1].snr <= v.sat[0].snr);
	/* no fix: time withheld (could be the receiver's guess) */
	hbas_nmea_feed(&g, "$GPRMC,201530.50,V,,,,,,,011026,,,N*79");
	hbas_gps_format(line, sizeof(line), &g, true);
	hbas_gps_view_apply(&v, line);
	CHECK(!v.valid && v.time == 0 && v.course < 0);
	CHECK(!hbas_gps_view_apply(&v, "bt powered=1\n"));
}

int main(void)
{
	test_checksums();
	test_rmc_and_classic_example();
	test_gga_gsa_gsv_vtg();
	test_bad_input();
	test_time_and_rollover();
	test_gps_line_roundtrip();
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return EXIT_FAILURE;
	}
	puts("test_nmea: all checks passed");
	return EXIT_SUCCESS;
}
