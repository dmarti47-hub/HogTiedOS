// SPDX-License-Identifier: MIT
#define _GNU_SOURCE
#include "persist.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>

#include "hbas/settings.h"
#include "ui.h"

#define SAVE_DELAY_MS 2000          /* save once adjusting has stopped this long */

static const char *path, *mnt;
static unsigned seen_changes, saved_changes;
static uint32_t last_change_ms;
static char saved_text[HBAS_SETTINGS_MAX];   /* what the file holds now */

static int remount(bool rw)
{
	if (!mnt)
		return 0;
	return mount(NULL, mnt, NULL, MS_REMOUNT | MS_NOATIME | (rw ? 0 : MS_RDONLY), NULL);
}

/* mkdir -p for the directory part of file */
static int make_parent_dirs(const char *file)
{
	char dir[512];

	if (snprintf(dir, sizeof(dir), "%s", file) >= (int)sizeof(dir))
		return -1;
	for (char *p = dir + 1; *p; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (mkdir(dir, 0755) && errno != EEXIST)
			return -1;
		*p = '/';
	}
	return 0;
}

int persist_write_file(const char *file, const char *text)
{
	int ok, err;

	if (remount(true)) {
		err = errno;
		fprintf(stderr, "persist: %s not saved, %s not writable: %s\n", file, mnt,
			strerror(err));
		errno = err;
		return -1;
	}
	ok = !make_parent_dirs(file) && !hbas_settings_save(file, text);
	err = errno;
	if (remount(false))
		fprintf(stderr, "persist: WARNING %s left read-write: %s\n", mnt, strerror(errno));
	if (!ok) {
		fprintf(stderr, "persist: saving %s failed: %s\n", file, strerror(err));
		errno = err;
		return -1;
	}
	return 0;
}

static void current_text(char *buf, size_t len)
{
	struct hbas_audio_settings a;
	struct hbas_eq eq;

	ui_settings_get(&a, &eq);
	if (hbas_settings_format(&a, &eq, buf, len) < 0)
		buf[0] = '\0';
}

static void save(void)
{
	char text[HBAS_SETTINGS_MAX];

	current_text(text, sizeof(text));
	if (!text[0] || !strcmp(text, saved_text))
		return;                             /* e.g. changed and changed back */
	if (persist_write_file(path, text))
		return;
	snprintf(saved_text, sizeof(saved_text), "%s", text);
	fprintf(stderr, "settings: saved %s\n", path);
}

void persist_init(const char *p, const char *m)
{
	struct hbas_audio_settings a;
	struct hbas_eq eq;
	char text[HBAS_SETTINGS_MAX];

	path = p;
	mnt = m;
	seen_changes = saved_changes = ui_settings_changes();
	ui_settings_get(&a, &eq);                   /* defaults */
	if (hbas_settings_load(path, text, sizeof(text))) {
		if (errno != ENOENT)
			fprintf(stderr, "settings: cannot read %s: %s\n", path, strerror(errno));
	} else if (hbas_settings_parse(text, &a, &eq) < 0) {
		fprintf(stderr, "settings: %s is not a version %d file, using defaults\n",
			path, HBAS_SETTINGS_VERSION);
	} else {
		ui_settings_set(&a, &eq);
		fprintf(stderr, "settings: loaded %s\n", path);
	}
	/* only write when something actually differs from what's on disk */
	current_text(saved_text, sizeof(saved_text));
}

void persist_poll(uint32_t now_ms)
{
	unsigned c;

	if (!path)
		return;
	c = ui_settings_changes();
	if (c != seen_changes) {
		seen_changes = c;
		last_change_ms = now_ms;
	}
	if (seen_changes != saved_changes && now_ms - last_change_ms >= SAVE_DELAY_MS) {
		saved_changes = seen_changes;
		save();
	}
}

void persist_flush(void)
{
	if (path && ui_settings_changes() != saved_changes) {
		saved_changes = ui_settings_changes();
		save();
	}
}
