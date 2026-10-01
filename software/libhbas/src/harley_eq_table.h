/* SPDX-License-Identifier: MIT */
/* Internal: the generated Harley EQ preset table (harley_eq_table.c). */
#ifndef HBAS_HARLEY_EQ_TABLE_H
#define HBAS_HARLEY_EQ_TABLE_H

#include <stdint.h>

#include "hbas/audio.h"
#include "hbas/eq.h"

#define HBAS_HARLEY_HEADSET 256         /* cfg value for the HS_* profiles */

struct hbas_harley_voicing {
	uint16_t cfg;                   /* HD_Configuration_Options byte 0, or HEADSET */
	uint8_t engine_on;
	int8_t gain_db[HBAS_VOL_STEPS][HBAS_EQ_BANDS];
};

extern const struct hbas_harley_voicing hbas_harley_voicing[];
extern const unsigned hbas_harley_voicing_count;

#endif
