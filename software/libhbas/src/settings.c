// SPDX-License-Identifier: MIT
#define _POSIX_C_SOURCE 200809L
#include "hbas/settings.h"

#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *const system_keys[HBAS_SYS_COUNT] = { "stock", "custom" };
static const char *const headset_keys[HBAS_HS_COUNT] = { "off", "driver", "passenger" };
static const char *const preset_keys[HBAS_EQ_PRESET_COUNT] = {
	"flat", "harley", "bass", "vocal", "highway", "custom",
};
static const char *const volume_keys[HBAS_OUT_COUNT] = {
	"volume.speakers", "volume.headset_driver", "volume.headset_passenger",
};

int hbas_settings_format(const struct hbas_audio_settings *a, const struct hbas_eq *eq,
			 char *buf, size_t len)
{
	size_t n = 0;
	int w;

#define PUT(...) do { \
	w = snprintf(buf + n, len > n ? len - n : 0, __VA_ARGS__); \
	if (w < 0 || (size_t)w >= len - n) return -1; \
	n += (size_t)w; } while (0)

	if (len == 0)
		return -1;
	PUT("# HogTiedOS settings (written automatically)\n");
	PUT("version=%d\n", HBAS_SETTINGS_VERSION);
	for (int i = 0; i < HBAS_OUT_COUNT; i++)
		PUT("%s=%u\n", volume_keys[i], a->volume[i]);
	PUT("fade=%u\n", a->fade);
	PUT("output=%s\n", system_keys[a->system < HBAS_SYS_COUNT ? a->system : 0]);
	PUT("headset=%s\n", headset_keys[a->headset < HBAS_HS_COUNT ? a->headset : 0]);
	PUT("eq.preset=%s\n", preset_keys[eq->preset < HBAS_EQ_PRESET_COUNT ? eq->preset : 0]);
	PUT("eq.gains=");
	for (int i = 0; i < HBAS_EQ_BANDS; i++)
		PUT("%s%+d", i ? "," : "", eq->gain_db[i]);
	PUT("\n");
#undef PUT
	return (int)n;
}

static int lookup(const char *v, const char *const *keys, int n)
{
	for (int i = 0; i < n; i++)
		if (!strcmp(v, keys[i]))
			return i;
	return -1;
}

/* strict integer in [lo, hi]; returns 0 on success */
static int parse_int(const char *s, long lo, long hi, long *out)
{
	char *end;
	long v;

	errno = 0;
	v = strtol(s, &end, 10);
	if (errno || end == s || *end || v < lo || v > hi)
		return -1;
	*out = v;
	return 0;
}

static int parse_gains(const char *s, int8_t g[HBAS_EQ_BANDS])
{
	int8_t tmp[HBAS_EQ_BANDS];
	char buf[128];
	char *tok, *save = NULL;
	int n = 0;

	if (strlen(s) >= sizeof(buf))
		return -1;
	strcpy(buf, s);
	for (tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
		long v;

		if (n >= HBAS_EQ_BANDS || parse_int(tok, -HBAS_EQ_MAX_DB, HBAS_EQ_MAX_DB, &v))
			return -1;
		tmp[n++] = (int8_t)v;
	}
	if (n != HBAS_EQ_BANDS)
		return -1;                /* all bands or nothing */
	memcpy(g, tmp, sizeof(tmp));
	return 0;
}

int hbas_settings_parse(const char *text, struct hbas_audio_settings *a, struct hbas_eq *eq)
{
	struct hbas_audio_settings na = *a;
	struct hbas_eq neq = *eq;
	const char *p = text;
	int applied = 0, version = -1, preset = -1;
	bool gains = false;

	while (*p) {
		char line[160], *eqs, *key, *val;
		size_t ll = strcspn(p, "\n");
		long v;
		int i;

		if (ll < sizeof(line)) {
			memcpy(line, p, ll);
			line[ll] = '\0';
		} else {
			line[0] = '\0';           /* overlong line: ignore */
		}
		p += ll + (p[ll] == '\n');
		if (line[0] == '#' || !(eqs = strchr(line, '=')))
			continue;
		*eqs = '\0';
		key = line;
		val = eqs + 1;
		val[strcspn(val, "\r")] = '\0';

		if (!strcmp(key, "version")) {
			if (!parse_int(val, 0, 1000, &v))
				version = (int)v;
		} else if ((i = lookup(key, volume_keys, HBAS_OUT_COUNT)) >= 0) {
			if (!parse_int(val, 0, HBAS_VOL_STEPS - 1, &v)) {
				na.volume[i] = (uint8_t)v;
				applied++;
			}
		} else if (!strcmp(key, "fade")) {
			if (!parse_int(val, 0, HBAS_FADE_STEPS - 1, &v)) {
				na.fade = (uint8_t)v;
				applied++;
			}
		} else if (!strcmp(key, "output")) {
			if ((i = lookup(val, system_keys, HBAS_SYS_COUNT)) >= 0) {
				na.system = (enum hbas_speaker_system)i;
				applied++;
			}
		} else if (!strcmp(key, "headset")) {
			if ((i = lookup(val, headset_keys, HBAS_HS_COUNT)) >= 0) {
				na.headset = (enum hbas_headset)i;
				applied++;
			}
		} else if (!strcmp(key, "eq.preset")) {
			preset = lookup(val, preset_keys, HBAS_EQ_PRESET_COUNT);
		} else if (!strcmp(key, "eq.gains")) {
			if (!parse_gains(val, neq.gain_db)) {
				gains = true;
				applied++;
			}
		}
		/* unknown keys are ignored (forward compatibility) */
	}
	if (version != HBAS_SETTINGS_VERSION)
		return -1;
	if (preset >= 0) {
		if (preset == HBAS_EQ_CUSTOM) {
			if (gains)
				neq.preset = HBAS_EQ_CUSTOM;  /* custom needs its gains */
		} else {
			hbas_eq_set_preset(&neq, (enum hbas_eq_preset)preset);
		}
		applied++;
	}
	*a = na;
	*eq = neq;
	return applied;
}

int hbas_settings_save(const char *path, const char *text)
{
	char tmp[4096], dir[4096];
	size_t len = strlen(text);
	int fd, dfd, err;

	if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp) ||
	    snprintf(dir, sizeof(dir), "%s", path) >= (int)sizeof(dir)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0)
		return -1;
	if (write(fd, text, len) != (ssize_t)len || fsync(fd)) {
		err = errno ? errno : EIO;
		close(fd);
		unlink(tmp);
		errno = err;
		return -1;
	}
	if (close(fd) || rename(tmp, path)) {
		err = errno;
		unlink(tmp);
		errno = err;
		return -1;
	}
	/* make the rename itself durable */
	dfd = open(dirname(dir), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (dfd >= 0) {
		fsync(dfd);
		close(dfd);
	}
	return 0;
}

int hbas_settings_load(const char *path, char *buf, size_t cap)
{
	FILE *f = fopen(path, "r");
	size_t n;

	if (!f)
		return -1;
	n = fread(buf, 1, cap - 1, f);
	buf[n] = '\0';
	fclose(f);
	return 0;
}
