/* SPDX-License-Identifier: MIT */
#ifndef HOGTIED_PERSIST_H
#define HOGTIED_PERSIST_H

#include <stdint.h>

/*
 * Remembering the rider's settings (libhbas/settings file format).
 *
 * persist_init() loads PATH into the UI (call after ui_create). MOUNT, if
 * set, is a filesystem that stays read-only except while a save runs: it is
 * remounted read-write, the file written atomically, and remounted
 * read-only again (the eMMC on the radio). persist_poll() saves once the
 * rider has stopped changing things for a couple of seconds;
 * persist_flush() saves anything pending (on exit).
 */
void persist_init(const char *path, const char *mount);
void persist_poll(uint32_t now_ms);
void persist_flush(void);

#endif
