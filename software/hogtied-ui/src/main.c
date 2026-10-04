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
 *                     with HOGTIED_FBDEV; a desktop window (2x size, keys:
 *                     arrows, Enter, Esc) when built with HOGTIED_SDL
 *   --snapshot PREFIX offline: run the demo on a virtual clock and write
 *                     PREFIX-<name>.bmp screenshots, no display needed
 *
 * Bike configuration (model, speakers, trike, factory EQ): read from
 * /run/hbas/bike, which hbas-iocd writes when the IOC reports it
 * (--bike-file PATH to use another file), or --bike N to simulate one on a
 * PC (N = HD_Configuration_Options byte 0, e.g. 2 = OE FLTR).
 *
 * Phone music (Media page): hbas-btd's socket, /run/hbas/bt.sock, or
 * --bt-socket PATH (PC window build default: $XDG_RUNTIME_DIR/hbas-bt.sock).
 * GPS page: hbas-gpsd's socket, /run/hbas/gps.sock or --gps-socket PATH
 * (PC window build default: $XDG_RUNTIME_DIR/hbas-gps.sock).
 *
 * Map page: --map-dir DIR (a map from tools/maps/build_map.sh),
 * --map-style FILE (software/maps/hogtied.oss), --map-font FILE (TTF).
 * Saved places: --places FILE; default places.conf next to the settings.
 * Only in builds with HOGTIED_MAP.
 *
 * Settings (volume, output, EQ, ...) are remembered:
 *   --settings FILE   where to keep them; default on a PC window build:
 *                     ~/.config/hogtied/settings.conf
 *   --settings-mount DIR  FILE's filesystem stays read-only except while
 *                     saving (the radio's eMMC, see S30emmc)
 *
 * Keys: handlebar and front-panel buttons come from the "hbas-buttons" input
 * device (hbas-iocd). --stdin-keys also reads a/d (left/right), w/s
 * (up/down), Enter, q (back) from stdin, e.g. over the UART console.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
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
#include <sys/un.h>

#include "lvgl.h"
#include "hbas/dsp.h"
#include "hbas/gpsproto.h"
#include "hbas/vehicle.h"
#include "demo.h"
#include "persist.h"
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

/* ---- daemon clients: hbas-btd (Media page), hbas-gpsd (GPS page) -------- */

/*
 * A line-oriented Unix-socket client that reconnects whenever its daemon
 * (re)appears: on_line gets each line, on_link(true/false) connection changes.
 */
struct line_client {
	const char *path;
	int fd;
	char in[HBAS_GPS_LINE_MAX];
	size_t len;
	uint32_t next_try;
	void (*on_line)(const char *line);
	void (*on_link)(bool up);
};

static void lc_down(struct line_client *c)
{
	if (c->fd >= 0)
		close(c->fd);
	c->fd = -1;
	c->len = 0;
	c->on_link(false);
}

static void lc_poll(struct line_client *c, uint32_t now)
{
	char *nl;
	ssize_t r = -1;

	if (c->fd < 0 && c->path && now >= c->next_try) {
		struct sockaddr_un a = { .sun_family = AF_UNIX };
		int s;

		c->next_try = now + 2000;
		if (strlen(c->path) >= sizeof(a.sun_path))
			return;
		strcpy(a.sun_path, c->path);
		if ((s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)) < 0)
			return;
		if (connect(s, (struct sockaddr *)&a, sizeof(a))) {
			close(s);
			return;
		}
		fcntl(s, F_SETFL, O_NONBLOCK);
		c->fd = s;
		c->on_link(true);
	}
	while (c->fd >= 0 && (r = recv(c->fd, c->in + c->len, sizeof(c->in) - 1 - c->len, 0)) != 0) {
		if (r < 0) {
			if (errno != EAGAIN && errno != EWOULDBLOCK)
				lc_down(c);
			return;
		}
		c->len += (size_t)r;
		c->in[c->len] = '\0';
		while ((nl = strchr(c->in, '\n'))) {
			*nl = '\0';
			c->on_line(c->in);
			c->len -= (size_t)(nl + 1 - c->in);
			memmove(c->in, nl + 1, c->len + 1);
		}
		if (c->len >= sizeof(c->in) - 1)
			c->len = 0;
	}
	if (c->fd >= 0 && r == 0)
		lc_down(c);                         /* daemon went away */
}

/* Media page */
static struct hbas_bt_state bt_state;

static void bt_feed_line(const char *line)
{
	struct hbas_bt_msg m;

	if (!hbas_bt_parse(line, &m) && hbas_bt_apply(&bt_state, &m))
		ui_media_update(&bt_state);
}

static void bt_link(bool up)
{
	hbas_bt_state_init(&bt_state);
	bt_state.daemon = up;
	ui_media_update(&bt_state);
}

static struct line_client bt_client = { .fd = -1, .on_line = bt_feed_line, .on_link = bt_link };

static void bt_send(const char *line)
{
	if (bt_client.fd >= 0 && send(bt_client.fd, line, strlen(line), MSG_NOSIGNAL) < 0)
		fprintf(stderr, "bt: send failed: %s\n", strerror(errno));
}

/* GPS page */
static struct hbas_gps_view gps_view;

static void gps_feed_line(const char *line)
{
	if (hbas_gps_view_apply(&gps_view, line))
		ui_gps_update(&gps_view);
}

static void gps_link(bool up)
{
	hbas_gps_view_init(&gps_view);
	gps_view.daemon = up;
	ui_gps_update(&gps_view);
}

static struct line_client gps_client = { .fd = -1, .on_line = gps_feed_line, .on_link = gps_link };

/* Radio (Music page) */
static void tuner_feed_line(const char *line) { ui_tuner_apply(line); }
static void tuner_link(bool up) { (void)up; }
static struct line_client tuner_client = { .fd = -1, .on_line = tuner_feed_line,
					   .on_link = tuner_link };

static void tuner_send(const char *line)
{
	if (tuner_client.fd >= 0 && send(tuner_client.fd, line, strlen(line), MSG_NOSIGNAL) < 0)
		fprintf(stderr, "tuner: send failed: %s\n", strerror(errno));
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

/* ---- audio backend ------------------------------------------------------ */

/*
 * The DSP writes for volume/fade/mute are known from static analysis
 * (libhbas/dsp.h, docs/findings/AUDIO.md sec. 7) but not yet confirmed on
 * hardware, and there is no DSP SPI writer yet: they are only logged.
 */
static bool audio_log_quiet;

static void log_apply(void *ctx, const struct hbas_audio_db *db, bool muted,
		      enum hbas_audio_output out)
{
	struct hbas_dsp_write w[HBAS_DSP_AUDIO_WRITES];
	size_t n = hbas_dsp_audio_writes(db, muted, out, w);

	(void)ctx;
	if (audio_log_quiet)
		return;
	fprintf(stderr, "audio: out=%d vol=%d dB fade front=%d rear=%d dB%s -> %zu DSP writes:",
		out, db->volume_db, db->fade_front_db, db->fade_rear_db, muted ? " MUTED" : "", n);
	for (size_t i = 0; i < n; i++)
		if (w[i].word)                     /* the rest are 0 (off) */
			fprintf(stderr, " %u=0x%07x", w[i].addr, (unsigned)w[i].word);
	fprintf(stderr, " (others 0; not sent)\n");
}

static void log_eq(void *ctx, const char *name)
{
	(void)ctx;
	if (!audio_log_quiet)
		fprintf(stderr, "audio: factory EQ profile %s (not sent: profile loading not written yet)\n", name);
}

/* What a DSP backend would send for the user EQ: one safe-load of the 7
 * tone-slot biquads, replacing stock's tone stage (AUDIO.md sec. 7.3). */
static void log_user_eq(void *ctx, const struct hbas_eq *eq)
{
	int32_t words[HBAS_EQ_BANDS * 5];
	uint8_t frames[3][3 + 4 * HBAS_DSP_SAFELOAD_MAX];
	size_t len[3];
	int ok = 1;

	(void)ctx;
	for (int b = 0; b < HBAS_EQ_BANDS; b++) {
		struct hbas_biquad bq;

		hbas_biquad_peaking(&bq, HBAS_DSP_FS, hbas_eq_band_hz[b], eq->gain_db[b], HBAS_EQ_Q);
		ok &= hbas_biquad_to_dsp(&bq, &words[5 * b]) == 0;
	}
	if (audio_log_quiet)
		return;
	fprintf(stderr, "audio: EQ %s [%+d %+d %+d %+d %+d %+d %+d] dB -> %s%zu-frame safe-load "
		"of %d words to tone biquads 0-6 (not sent)\n",
		hbas_eq_preset_name(eq->preset), eq->gain_db[0], eq->gain_db[1], eq->gain_db[2],
		eq->gain_db[3], eq->gain_db[4], eq->gain_db[5], eq->gain_db[6], ok ? "" : "OUT OF RANGE ",
		hbas_dsp_safeload(HBAS_DSP_BIQUAD_BASE, words, HBAS_EQ_BANDS * 5, frames, len),
		HBAS_EQ_BANDS * 5);
}

static const struct hbas_audio_backend log_backend = { NULL, log_apply, log_eq, log_user_eq };

static void press_keys(const char *keys)
{
	for (; *keys; keys++) {
		switch (*keys) {
		case 'L': ui_key(UI_KEY_LEFT); break;
		case 'R': ui_key(UI_KEY_RIGHT); break;
		case 'U': ui_key(UI_KEY_UP); break;
		case 'D': ui_key(UI_KEY_DOWN); break;
		case 'E': ui_key(UI_KEY_ENTER); break;
		case 'B': ui_key(UI_KEY_BACK); break;
		}
	}
}

static int snapshot(const char *prefix, const char *map_dir, const char *map_style,
		    const char *map_font)
{
	static uint16_t buf[UI_WIDTH * UI_HEIGHT];
	/* keys: L/R/U/D = arrows, E = enter, B = back */
	static const struct { uint32_t t; int go; const char *keys; const char *name; } script[] = {
		{ 2000, UI_PAGE_HOME, "", "01-home" },
		{ 11000, UI_PAGE_NAV, "", "02-nav" },
		{ 11050, -1, "E", "02a-nav-menu" },               /* open the map menu */
		{ 11060, -1, "DDEE", "02b-nav-route" },            /* Devil's Lake -> Go */
		{ 22000, UI_PAGE_INFO, "", "03-info" },
		{ 22100, UI_PAGE_SETTINGS, "", "04-settings" },
		{ 22150, UI_PAGE_BLUETOOTH, "", "04b-bluetooth" },
		{ 22200, UI_PAGE_MEDIA, "", "05-media" },
		{ 22320, -1, "R", "05b-radio" },
		{ 22340, -1, "", "05c-radio-tuned" },
		{ 22360, UI_PAGE_MEDIA, "LDDE", "05d-pair-nav" },
		{ 22300, UI_PAGE_AUDIO, "", "06-audio" },
		{ 22420, UI_PAGE_AUDIO, "UE", "06b-audio-eq-nav" },
		{ 22400, UI_PAGE_EQ, "", "07-eq" },
		{ 23000, UI_PAGE_HOME, "", "08-home-warn" },       /* low fuel: FUEL turns amber */
	};
	lv_display_t *d = lv_display_create(UI_WIDTH, UI_HEIGHT);
	char path[512];

	lv_display_set_color_format(d, LV_COLOR_FORMAT_RGB565);
	lv_display_set_buffers(d, buf, NULL, sizeof(buf), LV_DISPLAY_RENDER_MODE_FULL);
	lv_display_set_flush_cb(d, snap_flush);
	ui_create();
	audio_log_quiet = true;
	ui_set_audio_backend(&log_backend);
	ui_set_bike(2);                         /* demo bike: OE FLTR, as the IOC would report */
	ui_map_open(map_dir, map_style, map_font);
	ui_map_places(NULL);
	ui_map_add_place("H-D Museum", 43.0317, -87.9165);
	ui_map_add_place("Devil's Lake", 43.4147, -89.7300);
	/* demo phone, as hbas-btd would report it */
	bt_state.daemon = true;
	bt_feed_line("bt powered=1 pairable=0 agent=1 connected=1 name=\"Rider's Phone\" player=1");
	bt_feed_line("track title=\"Born to Be Wild\" artist=Steppenwolf album=Steppenwolf "
		     "duration=210000");
	bt_feed_line("play status=playing position=64000");
	bt_feed_line("devices n=2");
	bt_feed_line("device id=\"/org/bluez/hci0/dev_RIDER\" name=\"Rider's Phone\" paired=1 connected=1");
	bt_feed_line("device id=\"/org/bluez/hci0/dev_GARMIN\" name=\"Garmin Zumo\" paired=1 connected=0");
	/* demo GPS fix (Milwaukee), as hbas-gpsd would report it */
	gps_link(true);
	gps_feed_line("gps link=1 valid=1 fix=3d quality=1 lat=43.038902 lon=-87.906474 "
		      "speed=88.5 course=272.0 alt=181.0 used=9 view=12 hdop=0.8 time=1790885730 "
		      "sats=\"12:47 5:45 25:44 2:41 29:38 15:33 18:29 21:22 31:-1\"");
	/* demo radio station, as hbas-tunerd would report it */
	ui_tuner_apply("tuner powered=1 band=FM freq=97500 seeking=0 stereo=1 rssi=60 snr=22");
	ui_tuner_apply("rds ps=\"KQRS\" rt=\"Steppenwolf - Born to Be Wild\"");
	for (size_t i = 0; i < sizeof(script) / sizeof(script[0]); i++) {
		run_to(script[i].t);
		if (script[i].go >= 0)
			ui_goto((enum ui_page)script[i].go);
		press_keys(script[i].keys);
		if (!strncmp(script[i].name, "02", 2))
			/* the map draws (and routes) on its own thread: give it
			 * time (real time) */
			for (int ms = 0; ms < 8000 && !(ui_map_has_frame() && ui_map_idle());
			     ms += 10) {
				ui_map_tick();
				usleep(10000);
			}
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

#if defined(HOGTIED_FBDEV) || defined(HOGTIED_SDL)
static volatile sig_atomic_t stop_requested;

static void on_stop_signal(int sig)
{
	(void)sig;
	stop_requested = 1;
}
#endif

#if defined(HOGTIED_SDL)
/*
 * LVGL 9.2's SDL driver handles SDL_QUIT by calling SDL_Quit() and then
 * lv_deinit(), which crashes (segfault on Ctrl+C, found in testing). SDL
 * raises SDL_QUIT on SIGINT/SIGTERM and when the last window closes, so keep
 * it from doing either: we stop on our own signal flag, and closing the
 * window deletes the display, which ends the main loop.
 */
static void sdl_no_quit_event(void)
{
	setenv("SDL_NO_SIGNAL_HANDLERS", "1", 1);
	setenv("SDL_QUIT_ON_LAST_WINDOW_CLOSE", "0", 1);
}

/* PC window: arrow keys, Enter and Esc drive the UI like the handlebars. */
static void window_key_cb(lv_event_t *e)
{
	switch (lv_event_get_key(e)) {
	case LV_KEY_LEFT: ui_key(UI_KEY_LEFT); break;
	case LV_KEY_RIGHT: ui_key(UI_KEY_RIGHT); break;
	case LV_KEY_UP: ui_key(UI_KEY_UP); break;
	case LV_KEY_DOWN: ui_key(UI_KEY_DOWN); break;
	case LV_KEY_ENTER: ui_key(UI_KEY_ENTER); break;
	case LV_KEY_ESC: case LV_KEY_BACKSPACE: ui_key(UI_KEY_BACK); break;
	default: break;
	}
}

static lv_display_t *create_display(const char *fbdev, const char *touch)
{
	lv_display_t *d = lv_sdl_window_create(UI_WIDTH, UI_HEIGHT);
	lv_indev_t *kb = lv_sdl_keyboard_create();

	lv_sdl_mouse_create();              /* the mouse stands in for touch */
	lv_group_t *g = lv_group_create();
	lv_obj_t *catcher;

	(void)fbdev;
	(void)touch;
	lv_sdl_window_set_zoom(d, 2);       /* 400x240 is tiny on a PC monitor */
	ui_create();
	/* an invisible focused object receives the keyboard's key events */
	catcher = lv_obj_create(lv_layer_top());
	lv_obj_set_size(catcher, 1, 1);
	lv_obj_set_style_opa(catcher, LV_OPA_TRANSP, 0);
	lv_obj_add_event_cb(catcher, window_key_cb, LV_EVENT_KEY, NULL);
	lv_group_add_obj(g, catcher);
	lv_indev_set_group(kb, g);
	return d;
}
#elif defined(HOGTIED_FBDEV)
static lv_display_t *create_display(const char *fbdev, const char *touch)
{
	lv_display_t *d = lv_linux_fbdev_create();

	lv_linux_fbdev_set_file(d, fbdev);
	ui_create();
	/* touchscreen: which controller the radio has, and its calibration
	 * (the EEPROM's touchCal), are still unknown; raw evdev for now */
	if (touch && !lv_evdev_create(LV_INDEV_TYPE_POINTER, touch))
		fprintf(stderr, "touch: cannot open %s\n", touch);
	return d;
}
#endif

struct live_opts {
	const char *fbdev, *replay, *can, *settings, *settings_mount, *bike_file, *bt_socket;
	const char *gps_socket, *map_dir, *map_style, *map_font, *places, *touch, *tuner_socket;
	bool demo, stdin_keys;
	int speakers, bike;
};

/* config=N from hbas-iocd's bike file; -1 if absent or unreadable */
static int read_bike_file(const char *path)
{
	FILE *f = fopen(path, "r");
	char line[64];
	int cfg = -1;

	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f))
		if (sscanf(line, "config=%d", &cfg) == 1)
			break;
	fclose(f);
	return cfg >= 0 && cfg <= 255 ? cfg : -1;
}

static int run_live(const struct live_opts *o)
{
#if defined(HOGTIED_FBDEV) || defined(HOGTIED_SDL)
	FILE *rf = NULL;
	int cs = -1, buttons = -1;
	uint32_t start = tick_ms(), next_replay = 0, next_button_scan = 0, next_bike_check = 0;
	int bike = -1;
	struct can_frame_lite frames[16];
	char line[256];

	if (o->replay && !(rf = fopen(o->replay, "r"))) {
		perror(o->replay);
		return 1;
	}
	if (o->can && (cs = open_can(o->can)) < 0) {
		fprintf(stderr, "cannot open CAN interface %s: %s\n", o->can, strerror(errno));
		return 1;
	}
	if (o->stdin_keys)
		fcntl(STDIN_FILENO, F_SETFL, fcntl(STDIN_FILENO, F_GETFL) | O_NONBLOCK);
	signal(SIGINT, on_stop_signal);
	signal(SIGTERM, on_stop_signal);
	create_display(o->fbdev, o->touch);
	ui_set_speaker_count(o->speakers);
	ui_media_set_sender(bt_send);
	ui_tuner_set_sender(tuner_send);
	ui_map_open(o->map_dir, o->map_style, o->map_font);
	bt_client.path = o->bt_socket;
	gps_client.path = o->gps_socket;
	tuner_client.path = o->tuner_socket;
	if (o->bike >= 0)
		ui_set_bike(bike = o->bike);
	if (o->settings)
		persist_init(o->settings, o->settings_mount);
	ui_map_places(o->places);
	{
		static char radio_cfg[512];
		const char *slash = o->settings ? strrchr(o->settings, '/') : NULL;

		if (o->settings) {
			snprintf(radio_cfg, sizeof(radio_cfg), "%.*sradio.conf",
				 slash ? (int)(slash - o->settings + 1) : 0, o->settings);
			ui_tuner_load_presets(radio_cfg);
		}
	}
	/* only now connect the audio output, so the first thing it gets is the
	 * saved settings (no jump from the defaults at power-on) */
	ui_set_audio_backend(&log_backend);
	/* window closed -> display deleted; Ctrl+C / SIGTERM -> stop_requested */
	while (!stop_requested && lv_display_get_default()) {
		uint32_t now = tick_ms() - start;

		/* hbas-iocd may create the button device after we start */
		if (buttons < 0 && now >= next_button_scan) {
			buttons = open_buttons();
			next_button_scan = now + 2000;
		}
		if (buttons >= 0)
			poll_buttons(buttons);
		/* hbas-btd / hbas-gpsd may start after us, or restart */
		lc_poll(&bt_client, now);
		lc_poll(&gps_client, now);
		lc_poll(&tuner_client, now);
		ui_map_tick();
		ui_media_tick();
		/* the IOC sends the bike configuration once, at startup */
		if (o->bike < 0 && now >= next_bike_check) {
			int cfg = read_bike_file(o->bike_file);

			if (cfg != bike && cfg >= 0) {
				fprintf(stderr, "bike configuration %d\n", cfg);
				ui_set_bike(bike = cfg);
			}
			next_bike_check = now + 2000;
		}

		if (o->demo) {
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
		if (o->stdin_keys)
			poll_keys();
		ui_update(&vehicle);
		persist_poll(now);
		usleep(lv_timer_handler() * 1000);
	}
	persist_flush();                        /* a change made just before exit */
	ui_map_close();
	return 0;
#else
	(void)o;
	(void)open_can; (void)poll_can; (void)parse_line; (void)poll_keys;
	(void)open_buttons; (void)poll_buttons; (void)read_bike_file; (void)lc_poll;
	(void)bt_send; (void)gps_client; (void)tuner_send; (void)tuner_client;
	fprintf(stderr, "built without a display backend; use --snapshot\n");
	return 1;
#endif
}

int main(int argc, char **argv)
{
	struct live_opts o = { .fbdev = "/dev/fb0", .speakers = 4, .bike = -1,
				.bike_file = "/run/hbas/bike", .bt_socket = "/run/hbas/bt.sock",
				.gps_socket = "/run/hbas/gps.sock",
				.tuner_socket = "/run/hbas/tuner.sock" };
	static char default_bt[256], default_gps[256], default_tuner[256];
	const char *snap = NULL;
	static char default_settings[512], default_places[512];

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--demo"))
			o.demo = true;
		else if (!strcmp(argv[i], "--speakers") && i + 1 < argc)
			o.speakers = atoi(argv[++i]) == 2 ? 2 : 4;
		else if (!strcmp(argv[i], "--stdin-keys"))
			o.stdin_keys = true;
		else if (!strcmp(argv[i], "--fb") && i + 1 < argc)
			o.fbdev = argv[++i];
		else if (!strcmp(argv[i], "--snapshot") && i + 1 < argc)
			snap = argv[++i];
		else if (!strcmp(argv[i], "--replay") && i + 1 < argc)
			o.replay = argv[++i];
		else if (!strcmp(argv[i], "--can") && i + 1 < argc)
			o.can = argv[++i];
		else if (!strcmp(argv[i], "--bike") && i + 1 < argc)
			o.bike = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--bt-socket") && i + 1 < argc)
			o.bt_socket = argv[++i];
		else if (!strcmp(argv[i], "--gps-socket") && i + 1 < argc)
			o.gps_socket = argv[++i];
		else if (!strcmp(argv[i], "--tuner-socket") && i + 1 < argc)
			o.tuner_socket = argv[++i];
		else if (!strcmp(argv[i], "--map-dir") && i + 1 < argc)
			o.map_dir = argv[++i];
		else if (!strcmp(argv[i], "--map-style") && i + 1 < argc)
			o.map_style = argv[++i];
		else if (!strcmp(argv[i], "--map-font") && i + 1 < argc)
			o.map_font = argv[++i];
		else if (!strcmp(argv[i], "--bike-file") && i + 1 < argc)
			o.bike_file = argv[++i];
		else if (!strcmp(argv[i], "--settings") && i + 1 < argc)
			o.settings = argv[++i];
		else if (!strcmp(argv[i], "--settings-mount") && i + 1 < argc)
			o.settings_mount = argv[++i];
		else if (!strcmp(argv[i], "--places") && i + 1 < argc)
			o.places = argv[++i];
		else if (!strcmp(argv[i], "--touch") && i + 1 < argc)
			o.touch = argv[++i];
		else {
			fprintf(stderr, "usage: %s [--demo] [--replay FILE] [--can IFACE] "
				"[--fb DEV] [--stdin-keys] [--speakers 2|4] [--bike N | --bike-file F]\n"
				"       [--bt-socket PATH] [--gps-socket PATH]\n"
				"       [--map-dir DIR --map-style OSS --map-font TTF]\n"
				"       [--settings FILE [--settings-mount DIR]] [--places FILE]\n"
				"       [--touch /dev/input/eventN]\n"
				"       | --snapshot PREFIX\n",
				argv[0]);
			return 2;
		}
	}
#if defined(HOGTIED_SDL)
	/* PC: hbas-btd runs as the user, its socket lives in the runtime dir */
	if (!strcmp(o.bt_socket, "/run/hbas/bt.sock") && getenv("XDG_RUNTIME_DIR")) {
		snprintf(default_bt, sizeof(default_bt), "%s/hbas-bt.sock", getenv("XDG_RUNTIME_DIR"));
		o.bt_socket = default_bt;
	}
	if (!strcmp(o.gps_socket, "/run/hbas/gps.sock") && getenv("XDG_RUNTIME_DIR")) {
		snprintf(default_gps, sizeof(default_gps), "%s/hbas-gps.sock", getenv("XDG_RUNTIME_DIR"));
		o.gps_socket = default_gps;
	}
	if (!strcmp(o.tuner_socket, "/run/hbas/tuner.sock") && getenv("XDG_RUNTIME_DIR")) {
		snprintf(default_tuner, sizeof(default_tuner), "%s/hbas-tuner.sock",
			 getenv("XDG_RUNTIME_DIR"));
		o.tuner_socket = default_tuner;
	}
	/* PC: remember settings in the usual per-user place */
	if (!o.settings) {
		const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");

		if (xdg && *xdg)
			snprintf(default_settings, sizeof(default_settings), "%s/hogtied/settings.conf", xdg);
		else if (home && *home)
			snprintf(default_settings, sizeof(default_settings),
				 "%s/.config/hogtied/settings.conf", home);
		if (default_settings[0])
			o.settings = default_settings;
	}
#else
	(void)default_settings;
	(void)default_tuner;
	(void)default_bt;
	(void)default_gps;
#endif
	/* places live next to the settings (same filesystem, same remounting) */
	if (!o.places && o.settings) {
		const char *slash = strrchr(o.settings, '/');

		snprintf(default_places, sizeof(default_places), "%.*splaces.conf",
			 slash ? (int)(slash - o.settings + 1) : 0, o.settings);
		o.places = default_places;
	}
	hbas_vehicle_init(&vehicle);
	use_virtual_clock = snap != NULL;
#if defined(HOGTIED_SDL)
	sdl_no_quit_event();                    /* before SDL is initialised */
#endif
	lv_init();
	lv_tick_set_cb(tick_ms);
	return snap ? snapshot(snap, o.map_dir, o.map_style, o.map_font) : run_live(&o);
}
