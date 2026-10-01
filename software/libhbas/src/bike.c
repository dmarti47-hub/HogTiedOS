// SPDX-License-Identifier: MIT
#include "hbas/bike.h"

/* onOff.lua parse_0xF1E8 Bike_Config_Enum */
static const struct { uint8_t cfg; bool spk2; const char *name; } bikes[] = {
	{ 0, false, "OE FLHT" }, { 1, true, "OE FLTX" }, { 2, false, "OE FLTR" },
	{ 3, true, "OE FLTR" }, { 8, false, "OE FLHT-TriGlide" }, { 9, true, "OE FLHT-TriGlide" },
	{ 14, false, "CVO FLHTKSE" }, { 15, false, "CVO FLTR" }, { 16, false, "CVO FLHXSE4" },
	{ 128, true, "P&A FLHX" },
	{ 130, false, "P&A FLHT" }, { 131, false, "P&A FLHT" }, { 132, false, "P&A FLHT" },
	{ 133, false, "P&A FLHT" }, { 134, false, "P&A FLHT" }, { 135, false, "P&A FLHT" },
	{ 136, false, "P&A FLHT" }, { 137, false, "P&A FLHT" }, { 138, false, "P&A FLHT" },
	{ 139, false, "P&A FLHT" }, { 144, true, "P&A FLHT" }, { 145, false, "P&A FLHT" },
	{ 146, false, "P&A FLHT" }, { 147, false, "P&A FLHT" }, { 148, false, "P&A FLHT" },
	{ 149, false, "P&A FLHT" }, { 150, false, "P&A FLHT" },
	{ 192, true, "P&A FLHR" }, { 193, false, "P&A FLHR" }, { 194, false, "P&A FLHR" },
	{ 195, false, "P&A FLHR" }, { 196, false, "P&A FLHR" }, { 197, false, "P&A FLHR" },
	{ 198, false, "P&A FLHR" }, { 199, false, "P&A FLHR" },
	{ 208, true, "P&A FLTR" }, { 209, false, "P&A FLTR" }, { 210, false, "P&A FLTR" },
	{ 211, false, "P&A FLTR" }, { 212, false, "P&A FLTR" }, { 213, false, "P&A FLTR" },
	{ 214, false, "P&A FLTR" }, { 215, false, "P&A FLTR" },
	{ 224, false, "P&A Trike" }, { 225, false, "P&A Trike" },
	{ 254, false, "4 Spk Flat EQ" }, { 255, true, "2 Spk Flat EQ" },
};

void hbas_bike_info(uint8_t cfg, struct hbas_bike_info *out)
{
	*out = (struct hbas_bike_info){ "Reserved", 4, false, false };
	for (unsigned i = 0; i < sizeof(bikes) / sizeof(bikes[0]); i++) {
		if (bikes[i].cfg != cfg)
			continue;
		out->name = bikes[i].name;
		out->speakers = bikes[i].spk2 ? 2 : 4;
		/* the stock table marks trikes only by name */
		out->trike = cfg == 8 || cfg == 9 || cfg == 224 || cfg == 225;
		out->known = true;
		return;
	}
}
