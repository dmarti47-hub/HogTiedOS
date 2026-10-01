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

/* ---- hbas-btd client (Media page) ---------------------------------------- */

static struct hbas_bt_state bt_state;
static int bt_fd = -1;
static char bt_in[HBAS_BT_LINE_MAX];
static size_t bt_len;

static void bt_send(const char *line)
{
	if (bt_fd >= 0 && send(bt_fd, line, strlen(line), MSG_NOSIGNAL) < 0)
		fprintf(stderr, "bt: send failed: %s\n", strerror(errno));
}

static void bt_feed_line(const char *line)
{
	struct hbas_bt_msg m;

	if (!hbas_bt_parse(line, &m) && hbas_bt_apply(&bt_state, &m))
		ui_media_update(&bt_state);
}

static void bt_disconnected(void)
{
	if (bt_fd >= 0)
		close(bt_fd);
	bt_fd = -1;
	bt_len = 0;
	hbas_bt_state_init(&bt_state);          /* daemon = false */
	ui_media_update(&bt_state);
}

static void bt_connect(const char *path)
{
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	int s;

	if (strlen(path) >= sizeof(a.sun_path))
		return;
	strcpy(a.sun_path, path);
	s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (s < 0)
		return;
	if (connect(s, (struct sockaddr *)&a, sizeof(a))) {
		close(s);
		return;
	}
	fcntl(s, F_SETFL, O_NONBLOCK);
	bt_fd = s;
	bt_state.daemon = true;
	ui_media_update(&bt_state);
}

static void bt_poll(void)
{
	char *nl;
	ssize_t r;

	while (bt_fd >= 0 && (r = recv(bt_fd, bt_in + bt_len, sizeof(bt_in) - 1 - bt_len, 0)) != 0) {
		if (r < 0) {
			if (errno != EAGAIN && errno != EWOULDBLOCK)
				bt_disconnected();
			return;
		}
		bt_len += (size_t)r;
		bt_in[bt_len] = '\0';
		while ((nl = strchr(bt_in, '\n'))) {
			*nl = '\0';
			bt_feed_line(bt_in);
			bt_len -= (size_t)(nl + 1 - bt_in);
			memmove(bt_in, nl + 1, bt_len + 1);
		}
		if (bt_len >= sizeof(bt_in) - 1)
			bt_len = 0;
	}
	if (bt_fd >= 0 && r == 0)
		bt_disconnected();                  /* daemon went away */
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

static int snapshot(const char *prefix)
{
	static uint16_t buf[UI_WIDTH * UI_HEIGHT];
	/* keys: L/R/U/D = arrows, E = enter, B = back */
	static const struct { uint32_t t; const char *keys; const char *name; } script[] = {
		{ 500, "", "01-key-on" },
		{ 11000, "", "02-accelerating" },
		{ 22000, "", "03-cruise-low-fuel" },
		{ 22050, "R", "03b-media" },                       /* demo phone playing */
		{ 22100, "R", "04-audio" },
		{ 22200, "DERRR", "05-audio-fade-adjust" },        /* fade 3 steps front */
		{ 22300, "EDER", "06-audio-custom-system" },       /* output -> custom system */
		{ 22350, "EDER", "06b-audio-driver-headset" },     /* headset -> driver, fade hides */
		{ 22400, "ER", "07-eq-flat" },
		{ 22450, "D", "07b-eq-harley" },                   /* Harley tone for the demo FLTR */
		{ 22500, "DDD", "08-eq-preset-highway" },          /* Harley -> Bass -> Vocal -> Highway */
		{ 22600, "ERRRUUU", "09-eq-adjust-1k" },           /* 1 kHz band +3 -> Custom */
		{ 22700, "ER", "10-tires" },
		{ 22800, "R", "11-system" },
		{ 22900, "B", "12-back-to-dash" },
		/* same bike reporting itself as a trike: third tire appears */
		{ 23000, "RRRR", "13-tires-trike" },
		/* 2-speaker bike, back on speakers: no fade row */
		{ 23100, "BRRUELEDELE", "14-audio-2-speakers" },  /* stock, headset off: no fade on 2 speakers */
		{ 23150, "DERE", "14b-audio-speed-volume" },       /* speed volume on at demo speed */
		{ 23200, "RD", "15-eq-harley-speakers" },          /* EQ page, Custom -> Harley */
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
	/* demo phone, as hbas-btd would report it */
	bt_state.daemon = true;
	bt_feed_line("bt powered=1 pairable=0 agent=1 connected=1 name=\"Rider's Phone\" player=1");
	bt_feed_line("track title=\"Born to Be Wild\" artist=Steppenwolf album=Steppenwolf "
		     "duration=210000");
	bt_feed_line("play status=playing position=64000");
	for (size_t i = 0; i < sizeof(script) / sizeof(script[0]); i++) {
		if (!strcmp(script[i].name, "13-tires-trike"))
			demo_set_trike(1);
		if (!strcmp(script[i].name, "14-audio-2-speakers"))
			ui_set_speaker_count(2);
		run_to(script[i].t);
		press_keys(script[i].keys);
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

static lv_display_t *create_display(const char *fbdev)
{
	lv_display_t *d = lv_sdl_window_create(UI_WIDTH, UI_HEIGHT);
	lv_indev_t *kb = lv_sdl_keyboard_create();
	lv_group_t *g = lv_group_create();
	lv_obj_t *catcher;

	(void)fbdev;
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
static lv_display_t *create_display(const char *fbdev)
{
	lv_display_t *d = lv_linux_fbdev_create();

	lv_linux_fbdev_set_file(d, fbdev);
	ui_create();
	return d;
}
#endif

struct live_opts {
	const char *fbdev, *replay, *can, *settings, *settings_mount, *bike_file, *bt_socket;
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
	uint32_t next_bt_connect = 0;
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
	create_display(o->fbdev);
	ui_set_speaker_count(o->speakers);
	ui_media_set_sender(bt_send);
	if (o->bike >= 0)
		ui_set_bike(bike = o->bike);
	if (o->settings)
		persist_init(o->settings, o->settings_mount);
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
		/* hbas-btd may start after us, or restart */
		if (bt_fd < 0 && o->bt_socket && now >= next_bt_connect) {
			bt_connect(o->bt_socket);
			next_bt_connect = now + 2000;
		}
		bt_poll();
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
	return 0;
#else
	(void)o;
	(void)open_can; (void)poll_can; (void)parse_line; (void)poll_keys;
	(void)open_buttons; (void)poll_buttons; (void)read_bike_file; (void)bt_connect;
	(void)bt_poll; (void)bt_send;
	fprintf(stderr, "built without a display backend; use --snapshot\n");
	return 1;
#endif
}

int main(int argc, char **argv)
{
	struct live_opts o = { .fbdev = "/dev/fb0", .speakers = 4, .bike = -1,
				.bike_file = "/run/hbas/bike", .bt_socket = "/run/hbas/bt.sock" };
	static char default_bt[256];
	const char *snap = NULL;
	static char default_settings[512];

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
		else if (!strcmp(argv[i], "--bike-file") && i + 1 < argc)
			o.bike_file = argv[++i];
		else if (!strcmp(argv[i], "--settings") && i + 1 < argc)
			o.settings = argv[++i];
		else if (!strcmp(argv[i], "--settings-mount") && i + 1 < argc)
			o.settings_mount = argv[++i];
		else {
			fprintf(stderr, "usage: %s [--demo] [--replay FILE] [--can IFACE] "
				"[--fb DEV] [--stdin-keys] [--speakers 2|4] [--bike N | --bike-file F]\n"
				"       [--bt-socket PATH]\n"
				"       [--settings FILE [--settings-mount DIR]] | --snapshot PREFIX\n",
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
	(void)default_bt;
#endif
	hbas_vehicle_init(&vehicle);
	use_virtual_clock = snap != NULL;
#if defined(HOGTIED_SDL)
	sdl_no_quit_event();                    /* before SDL is initialised */
#endif
	lv_init();
	lv_tick_set_cb(tick_ms);
	return snap ? snapshot(snap) : run_live(&o);
}
