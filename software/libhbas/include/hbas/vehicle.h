/* SPDX-License-Identifier: MIT */
/*
 * Decoder for Harley-Davidson bike CAN messages, as consumed by the stock
 * Boom! Box vehicleCAN.lua service (docs/findings/CROSS_CHECKS.md sec. 11).
 *
 * Transport-independent: callers pass a CAN ID and its data bytes, whether
 * they came from IOC IPC channel 4 (the stock path, see hbas_ioc_can_unwrap)
 * or anywhere else. Values are kept raw where the stock code keeps them raw;
 * helpers convert only where the stock code documents the conversion.
 */
#ifndef HBAS_VEHICLE_H
#define HBAS_VEHICLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HBAS_ID_BODY_CTRL_DATA1   0x530
#define HBAS_ID_BODY_CTRL_DATA2   0x531
#define HBAS_ID_ENGINE_CTRL_DATA1 0x540
#define HBAS_ID_ENGINE_CTRL_DATA2 0x541
#define HBAS_ID_ENGINE_CTRL_DATA3 0x542
#define HBAS_ID_ENGINE_CTRL_DATA5 0x544
#define HBAS_ID_INSTRUMENT1_DATA1 0x5C0
#define HBAS_ID_INSTRUMENT1_DATA2 0x5C1

/* Special values used by the bike (per vehicleCAN.lua). */
#define HBAS_U16_ERROR   0xFFFE
#define HBAS_U16_NOVAL   0xFFFF
#define HBAS_U8_ERROR    0xFE
#define HBAS_U8_NOVAL    0xFF

enum hbas_ignition {
	HBAS_IGN_UNKNOWN = 0,   /* no BODY_CTRL_DATA1 yet, or unlisted value */
	HBAS_IGN_OFF,           /* 0x20 */
	HBAS_IGN_ACCESSORY,     /* 0x40 */
	HBAS_IGN_ON,            /* 0x60 */
	HBAS_IGN_CRANK,         /* 0x80 */
};

enum hbas_tire { HBAS_TIRE_FRONT, HBAS_TIRE_REAR, HBAS_TIRE_LEFT_REAR, HBAS_TIRE_COUNT };

/* Bit per message in hbas_vehicle.seen */
enum {
	HBAS_SEEN_BODY1 = 1u << 0, HBAS_SEEN_BODY2 = 1u << 1,
	HBAS_SEEN_ENG1 = 1u << 2, HBAS_SEEN_ENG2 = 1u << 3,
	HBAS_SEEN_ENG3 = 1u << 4, HBAS_SEEN_ENG5 = 1u << 5,
	HBAS_SEEN_INST1 = 1u << 6, HBAS_SEEN_INST2 = 1u << 7,
};

struct hbas_vehicle {
	uint32_t seen;

	/* 0x541 ENGINE_CTRL_DATA2 */
	uint16_t rpm;
	uint16_t speed_raw;        /* 0.1 km/h, or HBAS_U16_ERROR / NOVAL */
	uint16_t engine_temp_raw;  /* units not converted by stock code */
	uint8_t gear_raw;          /* value meanings not defined in stock code */
	uint8_t coolant_temp_raw;  /* units not converted by stock code */

	/* 0x540 ENGINE_CTRL_DATA1 */
	uint32_t total_distance_m; /* >= 0xFFFFFFFE: invalid */
	uint8_t ambient_raw;       /* < 0xFE: raw / 2 - 40 degC */
	uint32_t fuel_used_raw;    /* 0.1 ml; >= 0xFFFFFE: invalid */

	/* 0x530 BODY_CTRL_DATA1 */
	uint8_t oil_pressure_bcm;  /* 2 kPa units */
	enum hbas_ignition ignition;

	/* 0x531 BODY_CTRL_DATA2 (TPMS) */
	struct {
		bool enabled, telltale, trike;
		bool low_battery[HBAS_TIRE_COUNT];
		bool low_pressure[HBAS_TIRE_COUNT];
		uint8_t temp_raw[HBAS_TIRE_COUNT];     /* 254 error, 255 no value */
		uint8_t pressure_raw[HBAS_TIRE_COUNT]; /* 254 error, 255 no value */
	} tpms;

	/* 0x542 ENGINE_CTRL_DATA3 */
	bool overtemp;
	bool oil_pressure_warning; /* telltale: pressure NOT ok */
	bool engine_running;
	bool chassis_fan_enabled;
	uint8_t chassis_fan_mode;  /* 0 not present, 1 on, 2 off, 3 auto */
	bool rcco_enabled;

	/* 0x544 ENGINE_CTRL_DATA5 */
	bool rcco_active;
	uint8_t oil_pressure_mode; /* 0 none, 1 switch, 2 sensor, 3 none */
	uint8_t oil_pressure_ecm;  /* 2 kPa units */

	/* 0x5C0 INSTRUMENT1_DATA1 */
	bool metric;               /* speedometer set to metric */
	bool low_fuel;
	bool daytime_lighting;
	uint8_t photocell;

	/* 0x5C1 INSTRUMENT1_DATA2 (speedometer clock) */
	uint8_t clock_h, clock_m, clock_s;
	bool clock_24h;
	uint32_t battery_connect_time;
};

enum hbas_decode_result {
	HBAS_DECODED = 0,
	HBAS_UNKNOWN_ID = -1,      /* not a message we decode; state unchanged */
	HBAS_TOO_SHORT = -2,       /* fewer data bytes than the fields need */
};

void hbas_vehicle_init(struct hbas_vehicle *v);

/* Update v from one frame. data[0] is the first CAN data byte. */
int hbas_vehicle_decode(struct hbas_vehicle *v, uint16_t can_id,
			const uint8_t *data, size_t len);

/*
 * Split an IOC IPC channel-4 message (stock layout: ID low, ID high, one
 * unused byte, then data) into CAN ID and data. Returns 0, or -1 if the
 * message is shorter than the 3-byte header.
 */
int hbas_ioc_can_unwrap(const uint8_t *msg, size_t len, uint16_t *can_id,
			const uint8_t **data, size_t *data_len);

/* Helpers. Return false when the value is missing or flagged invalid. */
bool hbas_speed_kph_x10(const struct hbas_vehicle *v, unsigned *kph_x10);
bool hbas_speed_mph_x10(const struct hbas_vehicle *v, unsigned *mph_x10);
bool hbas_ambient_c_x10(const struct hbas_vehicle *v, int *c_x10);
const char *hbas_ignition_name(enum hbas_ignition ign);

#endif
