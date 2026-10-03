// SPDX-License-Identifier: MIT
#include "hbas/tunerproto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const struct hbas_band_plan hbas_band_plan_na[HBAS_BAND_COUNT] = {
	[HBAS_BAND_FM] = { 87700, 107900, 200 },   /* kHz */
	[HBAS_BAND_AM] = { 530, 1700, 10 },
	[HBAS_BAND_WB] = { 1, 7, 1 },              /* NOAA channels */
};

const char *hbas_band_name(enum hbas_band b)
{
	switch (b) {
	case HBAS_BAND_FM: return "FM";
	case HBAS_BAND_AM: return "AM";
	case HBAS_BAND_WB: return "WB";
	default: return "?";
	}
}

int hbas_wb_channel_khz(int channel)
{
	static const int khz[7] = { 162400, 162425, 162450, 162475, 162500, 162525, 162550 };

	if (channel < 1 || channel > 7)
		return 0;
	return khz[channel - 1];
}

void hbas_tuner_state_init(struct hbas_tuner_state *t)
{
	memset(t, 0, sizeof(*t));
	t->band = HBAS_BAND_FM;
	t->freq = 105700;
}

void hbas_tuner_freq_str(const struct hbas_tuner_state *t, char *buf, size_t len,
			 const char **unit)
{
	switch (t->band) {
	case HBAS_BAND_FM:
		snprintf(buf, len, "%d.%d", t->freq / 1000, (t->freq % 1000) / 100);
		if (unit)
			*unit = "MHz";
		break;
	case HBAS_BAND_AM:
		snprintf(buf, len, "%d", t->freq);
		if (unit)
			*unit = "kHz";
		break;
	case HBAS_BAND_WB:
		snprintf(buf, len, "WX%d", t->freq);
		if (unit)
			*unit = "";
		break;
	default:
		snprintf(buf, len, "--");
		if (unit)
			*unit = "";
	}
}

int hbas_tuner_step(enum hbas_band band, int freq, int n)
{
	const struct hbas_band_plan *p;
	int span, idx, count;

	if (band < 0 || band >= HBAS_BAND_COUNT)
		return freq;
	p = &hbas_band_plan_na[band];
	span = p->max - p->min;
	count = span / p->step + 1;
	idx = (freq - p->min) / p->step;
	idx = ((idx + n) % count + count) % count;       /* wrap */
	return p->min + idx * p->step;
}

/* ---- line protocol ------------------------------------------------------ */

int hbas_tuner_format(char *buf, size_t len, const struct hbas_tuner_state *t)
{
	return snprintf(buf, len,
			"tuner powered=%d band=%s freq=%d seeking=%d stereo=%d rssi=%d snr=%d\n",
			t->powered, hbas_band_name(t->band), t->freq, t->seeking, t->stereo,
			t->rssi, t->snr);
}

int hbas_tuner_format_rds(char *buf, size_t len, const char *ps, const char *rt)
{
	return snprintf(buf, len, "rds ps=\"%s\" rt=\"%s\"\n", ps ? ps : "", rt ? rt : "");
}

/* value of key=... into out; handles key="quoted value". Returns true if found. */
static bool field(const char *line, const char *key, char *out, size_t len)
{
	char pat[24];
	const char *p;
	size_t n;

	snprintf(pat, sizeof(pat), "%s=", key);
	p = strstr(line, pat);
	if (!p)
		return false;
	p += strlen(pat);
	if (*p == '"') {
		const char *end = strchr(++p, '"');

		n = end ? (size_t)(end - p) : strlen(p);
	} else {
		n = strcspn(p, " \t\r\n");
	}
	if (n >= len)
		n = len - 1;
	memcpy(out, p, n);
	out[n] = '\0';
	return true;
}

static int field_int(const char *line, const char *key, int def)
{
	char v[24];

	return field(line, key, v, sizeof(v)) ? atoi(v) : def;
}

bool hbas_tuner_apply(struct hbas_tuner_state *t, const char *line)
{
	char v[24];

	if (!strncmp(line, "tuner ", 6)) {
		t->powered = field_int(line, "powered", 0);
		if (field(line, "band", v, sizeof(v))) {
			for (int b = 0; b < HBAS_BAND_COUNT; b++)
				if (!strcmp(v, hbas_band_name(b)))
					t->band = b;
		}
		t->freq = field_int(line, "freq", t->freq);
		t->seeking = field_int(line, "seeking", 0);
		t->stereo = field_int(line, "stereo", 0);
		t->rssi = field_int(line, "rssi", 0);
		t->snr = field_int(line, "snr", 0);
		t->daemon = true;
		t->changes++;
		return true;
	}
	if (!strncmp(line, "rds ", 4)) {
		field(line, "ps", t->ps, sizeof(t->ps));
		field(line, "rt", t->rt, sizeof(t->rt));
		t->changes++;
		return true;
	}
	return false;
}

int hbas_tuner_cmd_band(char *buf, size_t len, enum hbas_band b)
{
	return snprintf(buf, len, "band %s\n", hbas_band_name(b));
}

int hbas_tuner_cmd_tune(char *buf, size_t len, int freq)
{
	return snprintf(buf, len, "tune %d\n", freq);
}

int hbas_tuner_cmd_seek(char *buf, size_t len, bool up)
{
	return snprintf(buf, len, "seek %s\n", up ? "up" : "down");
}

int hbas_tuner_cmd_power(char *buf, size_t len, bool on)
{
	return snprintf(buf, len, "power %s\n", on ? "on" : "off");
}
