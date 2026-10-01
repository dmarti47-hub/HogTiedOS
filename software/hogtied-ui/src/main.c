// SPDX-License-Identifier: MIT
/*
 * hogtied-ui: HogTiedOS main screen.
 *
 * Data sources (bike CAN frames, decoded by libhbas):
 *   --demo            built-in scripted ride
 *   --replay FILE     lines containing "ID#HEXDATA" (candump -L style),
 *                     played at 20 ms per line
 *   --can IFACE       Linux SocketCAN, e.g. vcan0 on a PC
 * (The stock path, IOC IPC channel 4, arrives with the IOC driver.)
 *
 * Output:
 *   default           Linux framebuffer (/dev/fb0, or --fb DEV), when built
 *                     with HOGTIED_FBDEV
 *   --snapshot PREFIX offline: run the demo on a virtual clock and write
 *                     PREFIX-<name>.bmp screenshots, no display needed
 *
 * Keys: handlebar and front-panel buttons come from the "hbas-buttons" input
 * device (hbas-iocd). --stdin-keys also reads a/d (left/right), w/s
 * (up/down), Enter, q (back) from stdin, e.g. over the UART console.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <dirent.h>
#include <linux/can.h>
#include <linux/input.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

#include "lvgl.h"
#include "hbas/vehicle.h"
#include "demo.h"
#include "ui.h"

static struct hbas_vehicle vehicle;
static uint32_t virtual_ms;
static bool use_virtual_clock;

static uint32_t tick_ms(void)
{
	struct timespec ts;

	if (use_virtual_clock)
		return virtual_ms;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void feed(const struct can_frame_lite *f)
{
	hbas_vehicle_decode(&vehicle, f->id, f->data, f->len);
}

/* ---- offline snapshot backend ------------------------------------------ */

static uint16_t fb565[UI_WIDTH * UI_HEIGHT];

static void snap_flush(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
	const uint16_t *src = (const uint16_t *)px;

	for (int y = a->y1; y <= a->y2; y++)
		for (int x = a->x1; x <= a->x2; x++)
			fb565[y * UI_WIDTH + x] = *src++;
	lv_display_flush_ready(d);
}

static int write_bmp(const char *path)
{
	FILE *f = fopen(path, "wb");
	const uint32_t size = 54 + UI_WIDTH * 3 * UI_HEIGHT;
	const uint32_t height = (uint32_t)-UI_HEIGHT;     /* negative = top-down rows */
	uint8_t h[54] = { 'B', 'M' };

	if (!f)
		return -1;
	for (int i = 0; i < 4; i++) {
		h[2 + i] = (size >> (8 * i)) & 0xFF;
		h[18 + i] = ((uint32_t)UI_WIDTH >> (8 * i)) & 0xFF;
		h[22 + i] = (height >> (8 * i)) & 0xFF;
	}
	h[10] = 54; h[14] = 40;
	h[26] = 1; h[28] = 24;
	fwrite(h, 1, sizeof(h), f);
	for (int i = 0; i < UI_WIDTH * UI_HEIGHT; i++) {
		uint16_t p = fb565[i];
		uint8_t bgr[3] = { (p & 0x1F) << 3, ((p >> 5) & 0x3F) << 2, (p >> 11) << 3 };

		fwrite(bgr, 1, 3, f);
	}
	return fclose(f);
}

static void run_to(uint32_t until_ms)
{
	struct can_frame_lite frames[16];

	while (virtual_ms < until_ms) {
		virtual_ms += 20;
		size_t n = demo_frames(virtual_ms, frames, 16);

		for (size_t i = 0; i < n; i++)
			feed(&frames[i]);
		ui_update(&vehicle);
		lv_timer_handler();
	}
}

static int snapshot(const char *prefix)
{
	static uint16_t buf[UI_WIDTH * UI_HEIGHT];
	static const struct { uint32_t t; enum ui_key key; bool press; const char *name; } script[] = {
		{ 500, 0, false, "01-key-on" },
		{ 11000, 0, false, "02-accelerating" },
		{ 22000, 0, false, "03-cruise-low-fuel" },
		{ 22100, UI_KEY_RIGHT, true, "04-tires" },
		{ 22200, UI_KEY_RIGHT, true, "05-system" },
		{ 22300, UI_KEY_BACK, true, "06-back-to-dash" },
	};
	lv_display_t *d = lv_display_create(UI_WIDTH, UI_HEIGHT);
	char path[512];

	lv_display_set_color_format(d, LV_COLOR_FORMAT_RGB565);
	lv_display_set_buffers(d, buf, NULL, sizeof(buf), LV_DISPLAY_RENDER_MODE_FULL);
	lv_display_set_flush_cb(d, snap_flush);
	ui_create();
	for (size_t i = 0; i < sizeof(script) / sizeof(script[0]); i++) {
		run_to(script[i].t);
		if (script[i].press)
			ui_key(script[i].key);
		ui_update(&vehicle);
		lv_obj_invalidate(lv_screen_active());
		lv_refr_now(d);
		snprintf(path, sizeof(path), "%s-%s.bmp", prefix, script[i].name);
		if (write_bmp(path)) {
			perror(path);
			return 1;
		}
		printf("wrote %s\n", path);
	}
	return 0;
}

/* ---- live sources ------------------------------------------------------- */

static int open_can(const char *ifname)
{
	struct sockaddr_can addr = { .can_family = AF_CAN };
	struct ifreq ifr;
	int s = socket(PF_CAN, SOCK_RAW | SOCK_NONBLOCK, CAN_RAW);

	if (s < 0)
		return -1;
	snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname);
	if (ioctl(s, SIOCGIFINDEX, &ifr) < 0) {
		close(s);
		return -1;
	}
	addr.can_ifindex = ifr.ifr_ifindex;
	if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(s);
		return -1;
	}
	return s;
}

static void poll_can(int s)
{
	struct can_frame cf;

	while (read(s, &cf, sizeof(cf)) == (ssize_t)sizeof(cf)) {
		if (cf.can_id & (CAN_EFF_FLAG | CAN_RTR_FLAG | CAN_ERR_FLAG))
			continue;
		/* can_dlc: `len` only exists in kernel headers >= 5.11 */
		hbas_vehicle_decode(&vehicle, cf.can_id & CAN_SFF_MASK, cf.data, cf.can_dlc);
	}
}

/* Parse "...ID#HEX..." from a candump -L style line. */
static bool parse_line(const char *line, struct can_frame_lite *f)
{
	const char *hash = strchr(line, '#');
	const char *p = hash;
	unsigned id;

	if (!hash)
		return false;
	while (p > line && p[-1] != ' ')
		p--;
	if (sscanf(p, "%x#", &id) != 1 || id > 0x7FF)
		return false;
	f->id = id;
	f->len = 0;
	for (p = hash + 1; f->len < 8 && sscanf(p, "%2hhx", &f->data[f->len]) == 1; p += 2)
		f->len++;
	return true;
}

/*
 * Handlebar/front-panel buttons arrive as the "hbas-buttons" keyboard that
 * hbas-iocd creates, using the stock key letters (CROSS_CHECKS sec. 13).
 */
static int open_buttons(void)
{
	DIR *d = opendir("/dev/input");
	struct dirent *e;
	int found = -1;

	while (d && (e = readdir(d)) && found < 0) {
		char path[300], name[64] = "";
		int fd;

		if (strncmp(e->d_name, "event", 5))
			continue;
		snprintf(path, sizeof(path), "/dev/input/%s", e->d_name);
		fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0)
			continue;
		if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) >= 0 && !strcmp(name, "hbas-buttons"))
			found = fd;
		else
			close(fd);
	}
	if (d)
		closedir(d);
	return found;
}

static bool button_to_ui_key(int code, enum ui_key *k)
{
	switch (code) {
	case KEY_U: case KEY_W: *k = UI_KEY_UP; return true;      /* right / left up */
	case KEY_J: case KEY_S: *k = UI_KEY_DOWN; return true;
	case KEY_H: case KEY_A: *k = UI_KEY_LEFT; return true;
	case KEY_K: case KEY_D: *k = UI_KEY_RIGHT; return true;
	case KEY_ENTER: case KEY_SPACE: *k = UI_KEY_ENTER; return true;  /* centers */
	case KEY_I: *k = UI_KEY_BACK; return true;                 /* HOME */
	default: return false;
	}
}

static void poll_buttons(int fd)
{
	struct input_event ev;
	enum ui_key k;

	while (read(fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev))
		if (ev.type == EV_KEY && ev.value == 1 && button_to_ui_key(ev.code, &k))
			ui_key(k);
}

static void poll_keys(void)
{
	char c;

	while (read(STDIN_FILENO, &c, 1) == 1) {
		switch (c) {
		case 'a': ui_key(UI_KEY_LEFT); break;
		case 'd': ui_key(UI_KEY_RIGHT); break;
		case 'w': ui_key(UI_KEY_UP); break;
		case 's': ui_key(UI_KEY_DOWN); break;
		case '\n': ui_key(UI_KEY_ENTER); break;
		case 'q': ui_key(UI_KEY_BACK); break;
		}
	}
}

static int run_live(const char *fbdev, bool demo, const char *replay, const char *can,
		    bool stdin_keys)
{
#ifdef HOGTIED_FBDEV
	lv_display_t *d = lv_linux_fbdev_create();
	FILE *rf = NULL;
	int cs = -1, buttons = -1;
	uint32_t start = tick_ms(), next_replay = 0, next_button_scan = 0;
	struct can_frame_lite frames[16];
	char line[256];

	lv_linux_fbdev_set_file(d, fbdev);
	if (replay && !(rf = fopen(replay, "r"))) {
		perror(replay);
		return 1;
	}
	if (can && (cs = open_can(can)) < 0) {
		fprintf(stderr, "cannot open CAN interface %s: %s\n", can, strerror(errno));
		return 1;
	}
	if (stdin_keys)
		fcntl(STDIN_FILENO, F_SETFL, fcntl(STDIN_FILENO, F_GETFL) | O_NONBLOCK);
	ui_create();
	for (;;) {
		uint32_t now = tick_ms() - start;

		/* hbas-iocd may create the button device after we start */
		if (buttons < 0 && now >= next_button_scan) {
			buttons = open_buttons();
			next_button_scan = now + 2000;
		}
		if (buttons >= 0)
			poll_buttons(buttons);

		if (demo) {
			size_t n = demo_frames(now % 38000, frames, 16);

			for (size_t i = 0; i < n; i++)
				feed(&frames[i]);
		}
		while (rf && now >= next_replay && fgets(line, sizeof(line), rf)) {
			if (parse_line(line, &frames[0]))
				feed(&frames[0]);
			next_replay += 20;
		}
		if (cs >= 0)
			poll_can(cs);
		if (stdin_keys)
			poll_keys();
		ui_update(&vehicle);
		usleep(lv_timer_handler() * 1000);
	}
#else
	(void)fbdev; (void)demo; (void)replay; (void)can; (void)stdin_keys;
	(void)open_can; (void)poll_can; (void)parse_line; (void)poll_keys;
	(void)open_buttons; (void)poll_buttons;
	fprintf(stderr, "built without a display backend; use --snapshot\n");
	return 1;
#endif
}

int main(int argc, char **argv)
{
	const char *fbdev = "/dev/fb0", *snap = NULL, *replay = NULL, *can = NULL;
	bool demo = false, stdin_keys = false;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--demo"))
			demo = true;
		else if (!strcmp(argv[i], "--stdin-keys"))
			stdin_keys = true;
		else if (!strcmp(argv[i], "--fb") && i + 1 < argc)
			fbdev = argv[++i];
		else if (!strcmp(argv[i], "--snapshot") && i + 1 < argc)
			snap = argv[++i];
		else if (!strcmp(argv[i], "--replay") && i + 1 < argc)
			replay = argv[++i];
		else if (!strcmp(argv[i], "--can") && i + 1 < argc)
			can = argv[++i];
		else {
			fprintf(stderr, "usage: %s [--demo] [--replay FILE] [--can IFACE] "
				"[--fb DEV] [--stdin-keys] | --snapshot PREFIX\n", argv[0]);
			return 2;
		}
	}
	hbas_vehicle_init(&vehicle);
	use_virtual_clock = snap != NULL;
	lv_init();
	lv_tick_set_cb(tick_ms);
	return snap ? snapshot(snap) : run_live(fbdev, demo, replay, can, stdin_keys);
}
