/* SPDX-License-Identifier: MIT */
/*
 * Bike configuration: HD_Configuration_Options (DID 0xF1E8) byte 0, sent by
 * the IOC at startup. Names, speaker counts and trikes are stock onOff.lua's
 * Bike_Config_Enum; the value also names the factory EQ profile
 * (<cfg>_<ON|OFF>.bin).
 */
#ifndef HBAS_BIKE_H
#define HBAS_BIKE_H

#include <stdbool.h>
#include <stdint.h>

struct hbas_bike_info {
	const char *name;       /* e.g. "OE FLTR"; "Reserved" if not in the table */
	uint8_t speakers;       /* 2 or 4 (stock default 4) */
	bool trike;
	bool known;
};

void hbas_bike_info(uint8_t cfg, struct hbas_bike_info *out);

#endif
