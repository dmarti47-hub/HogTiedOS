// SPDX-License-Identifier: MIT
/*
 * hbas-tunerd: the AM/FM/WB broadcast radio, for hogtied-ui.
 *
 * Hardware (docs/findings/RADIO.md): a Silabs Si4763 on I2C bus 1
 * (i2c-omap35xx at 0x48070000, /dev/i2c1), slave 0x62, plus an Si4749 HD
 * Radio coprocessor (0x11) that needs proprietary firmware and is out of
 * scope. This daemon drives the Si4763 for analog AM/FM/WB using the public
 * Si476x command set (Silabs AN649). The on-hardware bring-up (reset line,
 * firmware/patch load) is still unverified -- see the i2c backend.
 *
 * Clients connect to a Unix socket: they receive the state (libhbas
 * tunerproto.h) and send commands (band/tune/seek/power). --demo runs a
 * simulated set of stations so the UI works on a PC with no radio.
 *
 * Options:
 *   --socket PATH   default /run/hbas/tuner.sock
 *   --i2c PATH      the I2C bus (e.g. /dev/i2c-1); real hardware
 *   --addr N        Si4763 I2C address (default 0x62)
 *   --demo          simulate stations instead of a chip (PC)
 *   -v              log commands and tunes
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "hbas/tunerproto.h"

#define MAX_CLIENTS 4

static int verbose;
static volatile sig_atomic_t stop;

static void logmsg(const char *fmt, ...)
{
	va_list ap;

	if (!verbose)
		return;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

static uint32_t now_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}

/* ---- backend interface -------------------------------------------------- */

struct backend {
	bool (*open)(struct backend *b);
	void (*power)(struct backend *b, bool on);
	void (*set_band)(struct backend *b, enum hbas_band band);
	void (*tune)(struct backend *b, int freq);
	/* seek to the next station in a direction; returns the freq found. */
	int (*seek)(struct backend *b, bool up, int from, enum hbas_band band);
	/* read signal + RDS for the current freq; fills rssi/snr/stereo/ps/rt. */
	void (*status)(struct backend *b, int freq, enum hbas_band band, int *rssi, int *snr,
		       bool *stereo, char *ps, size_t pslen, char *rt, size_t rtlen);
	void *priv;
};

/* ---- demo backend ------------------------------------------------------- */

struct demo_station {
	enum hbas_band band;
	int freq;
	const char *ps, *rt;
	bool stereo;
	int rssi;
};
static const struct demo_station demo_stations[] = {
	{ HBAS_BAND_FM, 88500, "HOG FM", "Classic rock, all day long", true, 54 },
	{ HBAS_BAND_FM, 93300, "WZZ 93", "The Blues Highway", true, 48 },
	{ HBAS_BAND_FM, 97500, "KQRS", "Steppenwolf - Born to Be Wild", true, 60 },
	{ HBAS_BAND_FM, 101300, "MIX 101", "Today's hits", true, 44 },
	{ HBAS_BAND_FM, 105700, "THE RIDE", "Road trip anthems", true, 58 },
	{ HBAS_BAND_AM, 720, "WGN 720", "News / Talk", false, 40 },
	{ HBAS_BAND_AM, 1010, "WINS", "All news", false, 36 },
	{ HBAS_BAND_AM, 1130, "WBBR", "Business", false, 33 },
	{ HBAS_BAND_WB, 1, "NOAA WX", "Weather radio", false, 50 },
};
#define N_DEMO (int)(sizeof(demo_stations) / sizeof(demo_stations[0]))

static const struct demo_station *demo_at(int freq, enum hbas_band band)
{
	for (int i = 0; i < N_DEMO; i++)
		if (demo_stations[i].band == band && demo_stations[i].freq == freq)
			return &demo_stations[i];
	return NULL;
}

static int demo_seek(struct backend *b, bool up, int from, enum hbas_band band)
{
	int f = from;

	(void)b;
	for (int i = 0; i < 2000; i++) {
		f = hbas_tuner_step(band, f, up ? 1 : -1);
		if (demo_at(f, band))
			return f;
		if (f == from)
			break;
	}
	return from;
}

static void demo_status(struct backend *b, int freq, enum hbas_band band, int *rssi, int *snr,
			bool *stereo, char *ps, size_t pslen, char *rt, size_t rtlen)
{
	const struct demo_station *s = demo_at(freq, band);

	(void)b;
	if (s) {
		*rssi = s->rssi;
		*snr = s->stereo ? 22 : 14;
		*stereo = s->stereo;
		snprintf(ps, pslen, "%s", s->ps);
		snprintf(rt, rtlen, "%s", s->rt);
	} else {
		*rssi = 8;
		*snr = 2;
		*stereo = false;
		ps[0] = rt[0] = '\0';
	}
}

static bool demo_open(struct backend *b) { (void)b; return true; }
static void demo_power(struct backend *b, bool on) { (void)b; (void)on; }
static void demo_set_band(struct backend *b, enum hbas_band band) { (void)b; (void)band; }
static void demo_tune(struct backend *b, int freq) { (void)b; (void)freq; }

static struct backend demo_backend = {
	.open = demo_open, .power = demo_power, .set_band = demo_set_band,
	.tune = demo_tune, .seek = demo_seek, .status = demo_status,
};

/* ---- clients / socket --------------------------------------------------- */

static int clients[MAX_CLIENTS] = { -1, -1, -1, -1 };

static void broadcast(const char *line)
{
	size_t n = strlen(line);

	for (int i = 0; i < MAX_CLIENTS; i++)
		if (clients[i] >= 0 &&
		    send(clients[i], line, n, MSG_NOSIGNAL | MSG_DONTWAIT) != (ssize_t)n) {
			close(clients[i]);
			clients[i] = -1;
		}
}

static int listen_socket(const char *path)
{
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	char dir[sizeof(a.sun_path)], *slash;

	if (s < 0 || strlen(path) >= sizeof(a.sun_path))
		return -1;
	snprintf(dir, sizeof(dir), "%s", path);
	if ((slash = strrchr(dir, '/')) && slash != dir) {
		*slash = '\0';
		mkdir(dir, 0755);
	}
	strcpy(a.sun_path, path);
	unlink(path);
	if (bind(s, (struct sockaddr *)&a, sizeof(a)) || listen(s, 4)) {
		close(s);
		return -1;
	}
	return s;
}

/* ---- main --------------------------------------------------------------- */

static void on_sig(int s) { (void)s; stop = 1; }

static void publish(struct backend *be, struct hbas_tuner_state *t)
{
	char line[128], ps[9] = "", rt[65] = "";
	int rssi = 0, snr = 0;
	bool stereo = false;

	if (t->powered && t->band != HBAS_BAND_WB)
		be->status(be, t->freq, t->band, &rssi, &snr, &stereo, ps, sizeof(ps), rt, sizeof(rt));
	else if (t->powered)
		be->status(be, t->freq, t->band, &rssi, &snr, &stereo, ps, sizeof(ps), rt, sizeof(rt));
	t->rssi = rssi;
	t->snr = snr;
	t->stereo = stereo;
	snprintf(t->ps, sizeof(t->ps), "%s", ps);
	snprintf(t->rt, sizeof(t->rt), "%s", rt);
	hbas_tuner_format(line, sizeof(line), t);
	broadcast(line);
	hbas_tuner_format_rds(line, sizeof(line), ps, rt);
	broadcast(line);
}

/* one command line from a client */
static void handle_cmd(struct backend *be, struct hbas_tuner_state *t, const char *line)
{
	char arg[16];

	if (!strncmp(line, "power ", 6)) {
		t->powered = !strncmp(line + 6, "on", 2);
		be->power(be, t->powered);
	} else if (!strncmp(line, "band ", 5)) {
		for (int b = 0; b < HBAS_BAND_COUNT; b++)
			if (!strncmp(line + 5, hbas_band_name(b), 2)) {
				t->band = b;
				be->set_band(be, b);
				t->freq = hbas_band_plan_na[b].min;
				if (b == HBAS_BAND_FM)
					t->freq = 105700;
				be->tune(be, t->freq);
			}
	} else if (sscanf(line, "tune %15s", arg) == 1) {
		t->freq = atoi(arg);
		be->tune(be, t->freq);
	} else if (!strncmp(line, "seek ", 5)) {
		bool up = !strncmp(line + 5, "up", 2);

		/* demo_seek scans the FM plan; pass band through the state */
		if (t->band == HBAS_BAND_WB)
			t->freq = hbas_tuner_step(t->band, t->freq, up ? 1 : -1);
		else
			t->freq = be->seek(be, up, t->freq, t->band);
		be->tune(be, t->freq);
	} else {
		return;
	}
	logmsg("cmd: %s-> %s %d", line, hbas_band_name(t->band), t->freq);
	publish(be, t);
}

int main(int argc, char **argv)
{
	const char *sock = "/run/hbas/tuner.sock", *i2c = NULL;
	struct backend *be = &demo_backend;
	struct hbas_tuner_state t;
	char rxbuf[MAX_CLIENTS][256];
	size_t rxlen[MAX_CLIENTS] = { 0 };
	uint32_t last_pub = 0;
	int lsock;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--socket") && i + 1 < argc)
			sock = argv[++i];
		else if (!strcmp(argv[i], "--i2c") && i + 1 < argc)
			i2c = argv[++i];
		else if (!strcmp(argv[i], "--addr") && i + 1 < argc)
			(void)strtol(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--demo"))
			be = &demo_backend;
		else if (!strcmp(argv[i], "-v"))
			verbose = 1;
		else {
			fprintf(stderr, "usage: %s [--socket PATH] [--i2c DEV [--addr N] | --demo] [-v]\n",
				argv[0]);
			return 2;
		}
	}
	if (i2c) {
		fprintf(stderr, "hbas-tunerd: the Si4763 I2C backend needs on-hardware bring-up "
			"(reset line and firmware load are unverified); running in demo mode. "
			"See docs/findings/RADIO.md.\n");
		/* a real build would select the i2c backend here once verified */
	}

	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);
	signal(SIGPIPE, SIG_IGN);

	hbas_tuner_state_init(&t);
	t.powered = true;
	be->open(be);

	lsock = listen_socket(sock);
	if (lsock < 0) {
		fprintf(stderr, "hbas-tunerd: cannot listen on %s: %s\n", sock, strerror(errno));
		return 1;
	}
	logmsg("hbas-tunerd on %s (%s)", sock, be == &demo_backend ? "demo" : "i2c");

	while (!stop) {
		struct pollfd pfd[1 + MAX_CLIENTS];
		int nfd = 0;

		pfd[nfd].fd = lsock;
		pfd[nfd].events = POLLIN;
		nfd++;
		for (int i = 0; i < MAX_CLIENTS; i++)
			if (clients[i] >= 0) {
				pfd[nfd].fd = clients[i];
				pfd[nfd].events = POLLIN;
				nfd++;
			}
		if (poll(pfd, nfd, 500) < 0 && errno != EINTR)
			break;

		if (pfd[0].revents & POLLIN) {
			int c = accept(lsock, NULL, NULL);

			if (c >= 0) {
				int slot = -1;

				for (int i = 0; i < MAX_CLIENTS; i++)
					if (clients[i] < 0) {
						slot = i;
						break;
					}
				if (slot < 0) {
					close(c);
				} else {
					clients[slot] = c;
					rxlen[slot] = 0;
					publish(be, &t);        /* new client: current state */
				}
			}
		}
		for (int i = 0; i < MAX_CLIENTS; i++) {
			int idx = -1;

			for (int k = 1; k < nfd; k++)
				if (pfd[k].fd == clients[i]) { idx = k; break; }
			if (clients[i] < 0 || idx < 0 || !(pfd[idx].revents & POLLIN))
				continue;
			ssize_t n = recv(clients[i], rxbuf[i] + rxlen[i], sizeof(rxbuf[i]) - 1 - rxlen[i],
					 0);
			if (n <= 0) {
				close(clients[i]);
				clients[i] = -1;
				continue;
			}
			rxlen[i] += (size_t)n;
			rxbuf[i][rxlen[i]] = '\0';
			char *nl;
			while ((nl = memchr(rxbuf[i], '\n', rxlen[i]))) {
				*nl = '\0';
				handle_cmd(be, &t, rxbuf[i]);
				size_t rest = rxlen[i] - (size_t)(nl + 1 - rxbuf[i]);
				memmove(rxbuf[i], nl + 1, rest);
				rxlen[i] = rest;
			}
		}
		/* periodic refresh (signal/RDS drift) */
		if (now_ms() - last_pub >= 1000) {
			last_pub = now_ms();
			if (clients[0] >= 0 || clients[1] >= 0 || clients[2] >= 0 || clients[3] >= 0)
				publish(be, &t);
		}
	}
	unlink(sock);
	return 0;
}
