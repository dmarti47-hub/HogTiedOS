/* SPDX-License-Identifier: MIT */
#ifndef HOGTIED_DEMO_H
#define HOGTIED_DEMO_H

#include <stddef.h>
#include <stdint.h>

struct can_frame_lite {
	uint16_t id;
	uint8_t len;
	uint8_t data[8];
};

/*
 * Scripted ride for UI development: key on, engine start, pull away through
 * the gears, cruise, low-fuel warning, slow down. Frames are encoded exactly
 * as the bike sends them (CROSS_CHECKS.md sec. 11), so they exercise the real
 * decoder. Fills out[] with the frames for time t_ms; returns the count.
 */
size_t demo_frames(uint32_t t_ms, struct can_frame_lite *out, size_t max);

/* Make the demo bike report itself as a trike (third tire, BODY_CTRL_DATA2). */
void demo_set_trike(int trike);

#endif
