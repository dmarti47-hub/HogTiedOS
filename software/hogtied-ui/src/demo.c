// SPDX-License-Identifier: MIT
#include "demo.h"

#include "hbas/vehicle.h"

static int trike;

void demo_set_trike(int t) { trike = t; }

static void put16(uint8_t *p, unsigned v) { p[0] = v >> 8; p[1] = v & 0xFF; }

static void frame(struct can_frame_lite *f, uint16_t id)
{
	f->id = id;
	f->len = 8;
	for (int i = 0; i < 8; i++)
		f->data[i] = 0;
}

/* Speed profile in 0.1 km/h and the gear that goes with it. */
static unsigned speed_at(uint32_t t, unsigned *gear)
{
	static const struct { uint32_t t; unsigned kph_x10; } pts[] = {
		{ 0, 0 }, { 3000, 0 }, { 6000, 250 }, { 9000, 550 }, { 12000, 850 },
		{ 16000, 1050 }, { 26000, 1050 }, { 32000, 400 }, { 36000, 0 },
	};
	unsigned s = 0;

	for (size_t i = 1; i < sizeof(pts) / sizeof(pts[0]); i++) {
		if (t <= pts[i].t) {
			uint32_t t0 = pts[i - 1].t, t1 = pts[i].t;

			s = pts[i - 1].kph_x10 + (int)(pts[i].kph_x10 - pts[i - 1].kph_x10) *
			    (int)(t - t0) / (int)(t1 - t0);
			break;
		}
	}
	*gear = s == 0 ? 0 : s < 200 ? 1 : s < 400 ? 2 : s < 600 ? 3 : s < 800 ? 4 : s < 950 ? 5 : 6;
	return s;
}

/* Engine speed: idle when stopped, sweeping ~1800-4200 rpm within each gear. */
static unsigned rpm_at(uint32_t t, unsigned kph_x10, unsigned gear)
{
	static const unsigned lo[] = { 0, 0, 200, 400, 600, 800, 950 };
	static const unsigned hi[] = { 0, 200, 400, 600, 800, 950, 1200 };

	if (t < 1500)
		return 0;                       /* engine not started yet */
	if (gear == 0)
		return 900;
	return 1800 + (kph_x10 - lo[gear]) * 2400 / (hi[gear] - lo[gear]);
}

size_t demo_frames(uint32_t t, struct can_frame_lite *out, size_t max)
{
	size_t n = 0;
	unsigned gear, kph_x10 = speed_at(t, &gear);
	unsigned rpm = rpm_at(t, kph_x10, gear);
	uint32_t clock_s = 10 * 3600 + 42 * 60 + t / 1000;

	if (max < 6)
		return 0;

	frame(&out[n], HBAS_ID_BODY_CTRL_DATA1);            /* ignition on */
	out[n].data[4] = 40;
	out[n++].data[6] = 0x60;

	frame(&out[n], HBAS_ID_ENGINE_CTRL_DATA1);          /* odometer, 24.0 C */
	out[n].data[0] = 0x00; out[n].data[1] = 0x98; out[n].data[2] = 0x96; out[n].data[3] = 0x80;
	out[n++].data[4] = 128;

	frame(&out[n], HBAS_ID_ENGINE_CTRL_DATA2);          /* rpm, speed, gear, temps */
	put16(&out[n].data[0], rpm);
	put16(&out[n].data[2], kph_x10);
	/* warm-up for the temperature gauges (raw units unverified, demo only) */
	put16(&out[n].data[4], 60 + (t / 90 > 150 ? 150 : t / 90));
	out[n].data[6] = gear;
	out[n++].data[7] = 60 + (t / 110 > 130 ? 130 : t / 110);

	frame(&out[n], HBAS_ID_ENGINE_CTRL_DATA3);          /* engine running after start */
	out[n++].data[1] = t >= 1500 ? 0x40 : 0x00;

	frame(&out[n], HBAS_ID_INSTRUMENT1_DATA1);          /* low fuel from 20 s */
	out[n++].data[0] = t >= 20000 ? 0x40 : 0x00;

	frame(&out[n], HBAS_ID_INSTRUMENT1_DATA2);          /* speedo clock */
	out[n].data[0] = clock_s % 60;
	out[n].data[1] = clock_s / 60 % 60;
	out[n++].data[2] = clock_s / 3600 % 24;

	if (max > n) {
		frame(&out[n], HBAS_ID_BODY_CTRL_DATA2);    /* TPMS */
		out[n].data[0] = 0x80;
		out[n].data[1] = trike ? 0x01 : 0x00;
		out[n].data[2] = 32; out[n].data[3] = 34; out[n].data[4] = trike ? 33 : HBAS_U8_NOVAL;
		out[n].data[5] = 36; out[n].data[6] = 40; out[n].data[7] = trike ? 39 : HBAS_U8_NOVAL;
		n++;
	}
	return n;
}
