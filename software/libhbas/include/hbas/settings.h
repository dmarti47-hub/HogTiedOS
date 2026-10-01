/* SPDX-License-Identifier: MIT */
/*
 * Persistent user settings: a small key=value text file.
 *
 *   version=1
 *   volume.speakers=5
 *   volume.headset_driver=5
 *   volume.headset_passenger=5
 *   fade=8
 *   output=stock|custom
 *   headset=off|driver|passenger
 *   eq.preset=flat|bass|vocal|highway|custom
 *   eq.gains=+3,+1,-1,+1,+3,+4,+3
 *
 * Parsing starts from the caller's defaults and only applies keys that are
 * present and valid, so a damaged or partial file can't produce out-of-range
 * settings. Not saved: mute (always off at boot) and the stock speaker count
 * (that comes from the bike).
 *
 * Saving is atomic: write PATH.tmp, fsync, rename over PATH, fsync the
 * directory. A power cut leaves either the old file or the new one.
 */
#ifndef HBAS_SETTINGS_H
#define HBAS_SETTINGS_H

#include <stddef.h>

#include "hbas/audio.h"
#include "hbas/eq.h"

#define HBAS_SETTINGS_VERSION 1
#define HBAS_SETTINGS_MAX     1024     /* bytes; the file is ~250 */

/* Text for the current settings; returns its length, or -1 if len is too small. */
int hbas_settings_format(const struct hbas_audio_settings *a, const struct hbas_eq *eq,
			 char *buf, size_t len);

/*
 * Apply settings from text onto a/eq (which hold defaults). Returns the
 * number of keys applied, or -1 if the file is from an unknown version (then
 * nothing is applied).
 */
int hbas_settings_parse(const char *text, struct hbas_audio_settings *a, struct hbas_eq *eq);

/* Atomic save / plain load. Return 0 on success, -1 with errno set. */
int hbas_settings_save(const char *path, const char *text);
int hbas_settings_load(const char *path, char *buf, size_t cap);

#endif
