// SPDX-License-Identifier: MIT
#include "hbas/vehicle.h"

#include <stdio.h>
#include <stdlib.h>

static int failures;

#define CHECK(cond) do { \
	if (!(cond)) { fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); failures++; } \
} while (0)

#define DECODE(v, id, ...) ({ \
	const uint8_t _d[] = { __VA_ARGS__ }; \
	hbas_vehicle_decode((v), (id), _d, sizeof(_d)); })

static void test_engine2_speed_rpm_gear(void)
{
	struct hbas_vehicle v;
	unsigned kph, mph;

	hbas_vehicle_init(&v);
	CHECK(!hbas_speed_kph_x10(&v, &kph));            /* nothing seen yet */
	/* rpm 0x0BB8 = 3000, speed 0x03E8 = 100.0 km/h, eng temp 0x0102, gear 5, coolant 0x55 */
	CHECK(DECODE(&v, 0x541, 0x0B, 0xB8, 0x03, 0xE8, 0x01, 0x02, 0x05, 0x55) == HBAS_DECODED);
	CHECK(v.rpm == 3000);
	CHECK(hbas_speed_kph_x10(&v, &kph) && kph == 1000);
	CHECK(hbas_speed_mph_x10(&v, &mph) && mph == 621);   /* 62.1 mph */
	CHECK(v.engine_temp_raw == 0x0102);
	CHECK(v.gear_raw == 5);
	CHECK(v.coolant_temp_raw == 0x55);

	/* error / no-value speed keeps the last valid speed, like stock */
	CHECK(DECODE(&v, 0x541, 0, 0, 0xFF, 0xFE, 0, 0, 0, 0) == HBAS_DECODED);
	CHECK(hbas_speed_kph_x10(&v, &kph) && kph == 1000);
	CHECK(DECODE(&v, 0x541, 0, 0, 0xFF, 0xFF, 0, 0, 0, 0) == HBAS_DECODED);
	CHECK(hbas_speed_kph_x10(&v, &kph) && kph == 1000);
	CHECK(v.rpm == 0);                                 /* other fields still update */
}

static void test_engine1_distance_ambient_fuel(void)
{
	struct hbas_vehicle v;
	int c;

	hbas_vehicle_init(&v);
	CHECK(!hbas_ambient_c_x10(&v, &c));
	/* distance 0x00012345 m, ambient raw 130 -> 25.0 C, fuel 0x000102 */
	CHECK(DECODE(&v, 0x540, 0x00, 0x01, 0x23, 0x45, 130, 0x00, 0x01, 0x02) == HBAS_DECODED);
	CHECK(v.total_distance_m == 0x12345);
	CHECK(hbas_ambient_c_x10(&v, &c) && c == 250);
	CHECK(v.fuel_used_raw == 0x102);
	CHECK(DECODE(&v, 0x540, 0, 0, 0, 0, 0, 0, 0, 0) == HBAS_DECODED);
	CHECK(hbas_ambient_c_x10(&v, &c) && c == -400);  /* raw 0 = -40 C */
	CHECK(DECODE(&v, 0x540, 0, 0, 0, 0, 0xFE, 0, 0, 0) == HBAS_DECODED);
	CHECK(!hbas_ambient_c_x10(&v, &c));               /* error value */
}

static void test_body1_ignition_oil(void)
{
	struct hbas_vehicle v;

	hbas_vehicle_init(&v);
	CHECK(v.ignition == HBAS_IGN_UNKNOWN);
	CHECK(DECODE(&v, 0x530, 0, 0, 0, 0, 0x2A, 0, 0x60) == HBAS_DECODED);
	CHECK(v.ignition == HBAS_IGN_ON);
	CHECK(v.oil_pressure_bcm == 0x2A);
	CHECK(DECODE(&v, 0x530, 0, 0, 0, 0, 0, 0, 0x33) == HBAS_DECODED);
	CHECK(v.ignition == HBAS_IGN_ON);                 /* unlisted value ignored */
	CHECK(DECODE(&v, 0x530, 0, 0, 0, 0, 0, 0, 0x20) == HBAS_DECODED);
	CHECK(v.ignition == HBAS_IGN_OFF);
}

static void test_body2_tpms(void)
{
	struct hbas_vehicle v;

	hbas_vehicle_init(&v);
	CHECK(DECODE(&v, 0x531, 0x80 | 0x10 | 0x04 | 0x01, 0x01, 40, 41, 255, 36, 38, 254) == HBAS_DECODED);
	CHECK(v.tpms.enabled && v.tpms.telltale && v.tpms.trike);
	CHECK(v.tpms.low_battery[HBAS_TIRE_FRONT] && !v.tpms.low_battery[HBAS_TIRE_REAR]);
	CHECK(v.tpms.low_pressure[HBAS_TIRE_REAR] && !v.tpms.low_pressure[HBAS_TIRE_FRONT]);
	CHECK(v.tpms.temp_raw[HBAS_TIRE_FRONT] == 40 && v.tpms.temp_raw[HBAS_TIRE_LEFT_REAR] == 255);
	CHECK(v.tpms.pressure_raw[HBAS_TIRE_REAR] == 38 && v.tpms.pressure_raw[HBAS_TIRE_LEFT_REAR] == 254);
}

static void test_engine3_engine5_flags(void)
{
	struct hbas_vehicle v;

	hbas_vehicle_init(&v);
	CHECK(DECODE(&v, 0x542, 0x01, 0x42, 0x02 | (3 << 3), 0x08) == HBAS_DECODED);
	CHECK(v.overtemp && v.oil_pressure_warning && v.engine_running);
	CHECK(v.chassis_fan_enabled && v.chassis_fan_mode == 3 && v.rcco_enabled);
	CHECK(DECODE(&v, 0x544, 0, 0, 0x80 | 0x08, 150) == HBAS_DECODED);
	CHECK(v.rcco_active && v.oil_pressure_mode == 2 && v.oil_pressure_ecm == 150);
}

static void test_instrument_flags_and_clock(void)
{
	struct hbas_vehicle v;

	hbas_vehicle_init(&v);
	CHECK(DECODE(&v, 0x5C0, 0x20 | 0x40, 0, 0, 0, 0, 0, 77) == HBAS_DECODED);
	CHECK(v.metric && v.low_fuel && !v.daytime_lighting && v.photocell == 77);
	CHECK(DECODE(&v, 0x5C1, 30, 45, 13, 0x00, 0x01, 0x00, 0, 0x80) == HBAS_DECODED);
	CHECK(v.clock_h == 13 && v.clock_m == 45 && v.clock_s == 30);
	CHECK(v.battery_connect_time == 0x100 && v.clock_24h);
}

static void test_short_and_unknown(void)
{
	struct hbas_vehicle v, before;

	hbas_vehicle_init(&v);
	before = v;
	CHECK(DECODE(&v, 0x541, 1, 2, 3) == HBAS_TOO_SHORT);
	CHECK(DECODE(&v, 0x123, 1, 2, 3, 4, 5, 6, 7, 8) == HBAS_UNKNOWN_ID);
	CHECK(v.seen == before.seen && v.rpm == before.rpm);
}

static void test_ioc_unwrap(void)
{
	/* stock channel-4 layout: ID low, ID high, unused byte, data... */
	const uint8_t msg[] = { 0x41, 0x05, 0x08, 0x0B, 0xB8, 0x00, 0x64, 0, 0, 3, 0 };
	uint16_t id;
	const uint8_t *data;
	size_t n;
	struct hbas_vehicle v;
	unsigned kph;

	CHECK(hbas_ioc_can_unwrap(msg, sizeof(msg), &id, &data, &n) == 0);
	CHECK(id == 0x541 && n == 8 && data == msg + 3);
	hbas_vehicle_init(&v);
	CHECK(hbas_vehicle_decode(&v, id, data, n) == HBAS_DECODED);
	CHECK(v.rpm == 3000 && hbas_speed_kph_x10(&v, &kph) && kph == 100 && v.gear_raw == 3);
	CHECK(hbas_ioc_can_unwrap(msg, 2, &id, &data, &n) == -1);
}

int main(void)
{
	test_engine2_speed_rpm_gear();
	test_engine1_distance_ambient_fuel();
	test_body1_ignition_oil();
	test_body2_tpms();
	test_engine3_engine5_flags();
	test_instrument_flags_and_clock();
	test_short_and_unknown();
	test_ioc_unwrap();
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return EXIT_FAILURE;
	}
	puts("test_vehicle: all checks passed");
	return EXIT_SUCCESS;
}
