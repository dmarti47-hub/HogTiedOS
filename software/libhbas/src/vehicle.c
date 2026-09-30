// SPDX-License-Identifier: MIT
/*
 * Field layouts follow the stock vehicleCAN.lua decoders one for one. The Lua
 * indexes IPC messages from 1 with the first CAN data byte at msg[4], so Lua
 * msg[N] is data[N - 4] here.
 */
#include "hbas/vehicle.h"

#include <string.h>

static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t be24(const uint8_t *p) { return (uint32_t)p[0] << 16 | p[1] << 8 | p[2]; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | be24(p + 1); }

void hbas_vehicle_init(struct hbas_vehicle *v)
{
	memset(v, 0, sizeof(*v));
	v->speed_raw = HBAS_U16_NOVAL;
	v->ambient_raw = HBAS_U8_NOVAL;
	v->total_distance_m = 0xFFFFFFFF;
	v->fuel_used_raw = 0xFFFFFF;
	for (int i = 0; i < HBAS_TIRE_COUNT; i++)
		v->tpms.temp_raw[i] = v->tpms.pressure_raw[i] = HBAS_U8_NOVAL;
}

static enum hbas_ignition ignition_from(uint8_t raw)
{
	switch (raw) {
	case 0x20: return HBAS_IGN_OFF;
	case 0x40: return HBAS_IGN_ACCESSORY;
	case 0x60: return HBAS_IGN_ON;
	case 0x80: return HBAS_IGN_CRANK;
	default:   return HBAS_IGN_UNKNOWN;
	}
}

int hbas_vehicle_decode(struct hbas_vehicle *v, uint16_t can_id,
			const uint8_t *d, size_t len)
{
	switch (can_id) {
	case HBAS_ID_BODY_CTRL_DATA1:           /* Lua msg[8], msg[10] */
		if (len < 7)
			return HBAS_TOO_SHORT;
		v->oil_pressure_bcm = d[4];
		/* stock keeps the previous state for unlisted values */
		if (ignition_from(d[6]) != HBAS_IGN_UNKNOWN)
			v->ignition = ignition_from(d[6]);
		v->seen |= HBAS_SEEN_BODY1;
		return HBAS_DECODED;

	case HBAS_ID_BODY_CTRL_DATA2:           /* Lua msg[4..11] */
		if (len < 8)
			return HBAS_TOO_SHORT;
		v->tpms.enabled = d[0] & 0x80;
		v->tpms.low_battery[HBAS_TIRE_LEFT_REAR] = d[0] & 0x40;
		v->tpms.low_battery[HBAS_TIRE_REAR] = d[0] & 0x20;
		v->tpms.low_battery[HBAS_TIRE_FRONT] = d[0] & 0x10;
		v->tpms.low_pressure[HBAS_TIRE_LEFT_REAR] = d[0] & 0x08;
		v->tpms.low_pressure[HBAS_TIRE_REAR] = d[0] & 0x04;
		v->tpms.low_pressure[HBAS_TIRE_FRONT] = d[0] & 0x02;
		v->tpms.telltale = d[0] & 0x01;
		v->tpms.trike = d[1] & 0x01;
		v->tpms.temp_raw[HBAS_TIRE_FRONT] = d[2];
		v->tpms.temp_raw[HBAS_TIRE_REAR] = d[3];
		v->tpms.temp_raw[HBAS_TIRE_LEFT_REAR] = d[4];
		v->tpms.pressure_raw[HBAS_TIRE_FRONT] = d[5];
		v->tpms.pressure_raw[HBAS_TIRE_REAR] = d[6];
		v->tpms.pressure_raw[HBAS_TIRE_LEFT_REAR] = d[7];
		v->seen |= HBAS_SEEN_BODY2;
		return HBAS_DECODED;

	case HBAS_ID_ENGINE_CTRL_DATA1:         /* Lua msg[4..11] */
		if (len < 8)
			return HBAS_TOO_SHORT;
		v->total_distance_m = be32(d);
		v->ambient_raw = d[4];
		v->fuel_used_raw = be24(d + 5);
		v->seen |= HBAS_SEEN_ENG1;
		return HBAS_DECODED;

	case HBAS_ID_ENGINE_CTRL_DATA2:         /* Lua msg[4..11] */
		if (len < 8)
			return HBAS_TOO_SHORT;
		v->rpm = be16(d);
		/* stock ignores error/no-value speed and keeps the last valid one */
		if (be16(d + 2) != HBAS_U16_ERROR && be16(d + 2) != HBAS_U16_NOVAL)
			v->speed_raw = be16(d + 2);
		v->engine_temp_raw = be16(d + 4);
		v->gear_raw = d[6];
		v->coolant_temp_raw = d[7];
		v->seen |= HBAS_SEEN_ENG2;
		return HBAS_DECODED;

	case HBAS_ID_ENGINE_CTRL_DATA3:         /* Lua msg[4..7] */
		if (len < 4)
			return HBAS_TOO_SHORT;
		v->overtemp = d[0] & 0x01;
		v->oil_pressure_warning = d[1] & 0x02;
		v->engine_running = d[1] & 0x40;
		v->chassis_fan_enabled = d[2] & 0x02;
		v->chassis_fan_mode = (d[2] & 0x18) >> 3;
		v->rcco_enabled = d[3] & 0x08;
		v->seen |= HBAS_SEEN_ENG3;
		return HBAS_DECODED;

	case HBAS_ID_ENGINE_CTRL_DATA5:         /* Lua msg[6], msg[7] */
		if (len < 4)
			return HBAS_TOO_SHORT;
		v->rcco_active = d[2] & 0x08;
		v->oil_pressure_mode = (d[2] & 0xC0) >> 6;
		v->oil_pressure_ecm = d[3];
		v->seen |= HBAS_SEEN_ENG5;
		return HBAS_DECODED;

	case HBAS_ID_INSTRUMENT1_DATA1:         /* Lua msg[4], msg[10] */
		if (len < 7)
			return HBAS_TOO_SHORT;
		v->metric = d[0] & 0x20;
		v->low_fuel = d[0] & 0x40;
		v->daytime_lighting = d[0] & 0x80;
		v->photocell = d[6];
		v->seen |= HBAS_SEEN_INST1;
		return HBAS_DECODED;

	case HBAS_ID_INSTRUMENT1_DATA2:         /* Lua msg[4..11] */
		if (len < 8)
			return HBAS_TOO_SHORT;
		v->clock_s = d[0];
		v->clock_m = d[1];
		v->clock_h = d[2];
		v->battery_connect_time = be24(d + 3);
		v->clock_24h = d[7] & 0x80;
		v->seen |= HBAS_SEEN_INST2;
		return HBAS_DECODED;

	default:
		return HBAS_UNKNOWN_ID;
	}
}

int hbas_ioc_can_unwrap(const uint8_t *msg, size_t len, uint16_t *can_id,
			const uint8_t **data, size_t *data_len)
{
	if (len < 3)
		return -1;
	*can_id = (uint16_t)(msg[1] << 8 | msg[0]);   /* Lua: msg[2] * 256 + msg[1] */
	*data = msg + 3;
	*data_len = len - 3;
	return 0;
}

bool hbas_speed_kph_x10(const struct hbas_vehicle *v, unsigned *kph_x10)
{
	if (!(v->seen & HBAS_SEEN_ENG2) || v->speed_raw == HBAS_U16_NOVAL ||
	    v->speed_raw == HBAS_U16_ERROR)
		return false;
	*kph_x10 = v->speed_raw;
	return true;
}

bool hbas_speed_mph_x10(const struct hbas_vehicle *v, unsigned *mph_x10)
{
	unsigned k;

	if (!hbas_speed_kph_x10(v, &k))
		return false;
	/* 1 mph = 1.609344 km/h, rounded to nearest */
	*mph_x10 = (unsigned)(((unsigned long long)k * 1000000 + 804672) / 1609344);
	return true;
}

bool hbas_ambient_c_x10(const struct hbas_vehicle *v, int *c_x10)
{
	if (!(v->seen & HBAS_SEEN_ENG1) || v->ambient_raw >= HBAS_U8_ERROR)
		return false;
	*c_x10 = v->ambient_raw * 5 - 400;      /* raw / 2 - 40, in tenths */
	return true;
}

const char *hbas_ignition_name(enum hbas_ignition ign)
{
	static const char *const names[] = { "--", "OFF", "ACC", "ON", "CRANK" };

	return (unsigned)ign < sizeof(names) / sizeof(names[0]) ? names[ign] : "UNKNOWN";
}
