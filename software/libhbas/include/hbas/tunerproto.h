/* SPDX-License-Identifier: MIT */
#ifndef HBAS_TUNERPROTO_H
#define HBAS_TUNERPROTO_H

/*
 * AM/FM/WB radio: the model shared by the UI and hbas-tunerd, and the line
 * protocol between them (one text line per message over a Unix socket, like
 * btproto/gpsproto).
 *
 * Hardware (docs/findings/RADIO.md): a Silabs Si4763 on I2C bus 1, driven
 * with the public Si476x command set. Frequencies are in kHz (FM and AM);
 * WB is a channel number 1..7 (NOAA 162.400..162.550 MHz).
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum hbas_band { HBAS_BAND_FM, HBAS_BAND_AM, HBAS_BAND_WB, HBAS_BAND_COUNT };

/* NA (region 1) band plan; min/max/step in kHz (WB: channel 1..7). */
struct hbas_band_plan {
	int min, max, step;
};
extern const struct hbas_band_plan hbas_band_plan_na[HBAS_BAND_COUNT];

const char *hbas_band_name(enum hbas_band b);              /* "FM" / "AM" / "WB" */
/* The seven NOAA weather-band channel frequencies, in kHz. */
int hbas_wb_channel_khz(int channel);                      /* channel 1..7 */

struct hbas_tuner_state {
	bool daemon;                 /* connected to hbas-tunerd */
	bool powered;
	enum hbas_band band;
	int freq;                    /* kHz, or WB channel 1..7 */
	bool seeking;
	bool stereo;
	int rssi;                    /* dBuV, 0..75-ish */
	int snr;                     /* dB */
	char ps[9];                  /* RDS station name (8 chars) */
	char rt[65];                 /* RDS radio text */
	unsigned changes;
};

void hbas_tuner_state_init(struct hbas_tuner_state *t);

/*
 * Format the current frequency for display: "105.7" (FM, MHz), "1010"
 * (AM, kHz), "WX1" (WB). unit (may be NULL) gets "MHz"/"kHz"/"".
 */
void hbas_tuner_freq_str(const struct hbas_tuner_state *t, char *buf, size_t len,
			 const char **unit);

/* Step the frequency by n steps within the band (wraps). Returns the new freq. */
int hbas_tuner_step(enum hbas_band band, int freq, int n);

/* ---- line protocol ---- */

/* Daemon -> UI: the current state as one line (tuner ...). */
int hbas_tuner_format(char *buf, size_t len, const struct hbas_tuner_state *t);
/* Daemon -> UI: RDS text (rds ps="..." rt="..."). */
int hbas_tuner_format_rds(char *buf, size_t len, const char *ps, const char *rt);
/* UI side: apply one line; true if it was a tuner/rds line. */
bool hbas_tuner_apply(struct hbas_tuner_state *t, const char *line);

/* UI -> daemon command builders (write into buf). */
int hbas_tuner_cmd_band(char *buf, size_t len, enum hbas_band b);
int hbas_tuner_cmd_tune(char *buf, size_t len, int freq);
int hbas_tuner_cmd_seek(char *buf, size_t len, bool up);
int hbas_tuner_cmd_power(char *buf, size_t len, bool on);

#endif
