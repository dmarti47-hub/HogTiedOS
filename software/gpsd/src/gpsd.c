// SPDX-License-Identifier: MIT
/*
 * hbas-gpsd: the u-blox G5/G6 GPS on UART2 (/dev/ttyS1), for hogtied-ui.
 *
 * Bring-up does what stock boot.sh does, then checks: open at 9600, send
 * $PUBX,41,...,57600 (u-blox "set port": UBX+NMEA, 57600 baud), switch to
 * 57600, and wait for sentences with good checksums. If none come, try the
 * other usual rates until they do; if the receiver goes quiet later, hunt
 * again. The reset line (GPIO112) is left as the device tree sets it (low,
 * as stock's gpio-harley.conf does at boot).
 *
 * Once a second the current fix goes to every client of the socket as one
 * line (libhbas gpsproto.h). With --set-clock, a valid fix also sets the
 * system clock when it's off by more than 2 s (the unit has no other time
 * source until the IOC's clock is decoded).
 *
 * Options:
 *   --device PATH   default /dev/ttyS1
 *   --socket PATH   default /run/hbas/gps.sock
 *   --set-clock     set the system clock from GPS time (needs root)
 *   --replay FILE   PC: play an NMEA log instead of a receiver (loops)
 *   -v              log link changes and sentences
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "hbas/gpsproto.h"
#include "hbas/nmea.h"

#define MAX_CLIENTS     4
#define HUNT_LISTEN_MS  2500     /* per baud rate while hunting */
#define QUIET_MS        5000     /* no good sentence this long: hunt again */
#define LOCK_SENTENCES  2        /* good sentences in a row to call it locked */

static int verbose;
static volatile sig_atomic_t stop;

static void logmsg(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	fputs("hbas-gpsd: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

static uint64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* ---- serial port ----------------------------------------------------------- */

static speed_t speed_of(unsigned baud)
{
	switch (baud) {
	case 4800: return B4800;
	case 9600: return B9600;
	case 19200: return B19200;
	case 38400: return B38400;
	case 57600: return B57600;
	default: return B115200;
	}
}

static int set_baud(int fd, unsigned baud)
{
	struct termios t;

	if (tcgetattr(fd, &t))
		return -1;
	cfmakeraw(&t);
	t.c_cflag |= CLOCAL | CREAD;
	t.c_cflag &= ~(CRTSCTS | CSTOPB | PARENB);          /* 8N1, no flow control (stock -F) */
	cfsetispeed(&t, speed_of(baud));
	cfsetospeed(&t, speed_of(baud));
	t.c_cc[VMIN] = 0;
	t.c_cc[VTIME] = 0;
	if (tcsetattr(fd, TCSANOW, &t))
		return -1;
	tcflush(fd, TCIFLUSH);
	return 0;
}

/* ---- clients -------------------------------------------------------------- */

static int clients[MAX_CLIENTS] = { -1, -1, -1, -1 };

static void broadcast(const char *line)
{
	size_t n = strlen(line);

	for (int i = 0; i < MAX_CLIENTS; i++) {
		if (clients[i] >= 0 &&
		    send(clients[i], line, n, MSG_NOSIGNAL | MSG_DONTWAIT) != (ssize_t)n) {
			close(clients[i]);
			clients[i] = -1;
		}
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

/* ---- clock ---------------------------------------------------------------- */

static bool set_clock;
static uint64_t last_clock_set_ms;

static void maybe_set_clock(const struct hbas_gps *g)
{
	struct timespec now, want;
	int64_t t, diff;

	if (!set_clock || !g->valid || !hbas_gps_unix_time(g, &t))
		return;
	clock_gettime(CLOCK_REALTIME, &now);
	diff = t - (int64_t)now.tv_sec;
	if (diff < 0)
		diff = -diff;
	/* small drift: leave it; recheck at most every 10 min unless far off */
	if (diff <= 2 || (last_clock_set_ms && now_ms() - last_clock_set_ms < 600000 && diff < 60))
		return;
	want.tv_sec = (time_t)t;
	want.tv_nsec = (long)g->msec * 1000000L;
	if (clock_settime(CLOCK_REALTIME, &want))
		logmsg("can't set the clock: %s", strerror(errno));
	else
		logmsg("clock set from GPS (was off by %llds)", (long long)diff);
	last_clock_set_ms = now_ms();
}

/* ---- main --------------------------------------------------------------- */

static void on_sig(int s) { (void)s; stop = 1; }

int main(int argc, char **argv)
{
	static const unsigned hunt[] = { 57600, 9600, 115200, 38400, 19200, 4800 };
	const char *dev = "/dev/ttyS1", *sock_path = "/run/hbas/gps.sock", *replay = NULL;
	struct hbas_gps g;
	char line[HBAS_NMEA_MAX + 8], out[HBAS_GPS_LINE_MAX];
	size_t llen = 0;
	int fd = -1, ls;
	FILE *rf = NULL;
	unsigned hunt_i = 0, good_run = 0;
	bool locked = false;
	uint64_t hunt_until = 0, last_good = 0, next_publish = 0, next_replay = 0;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--device") && i + 1 < argc)
			dev = argv[++i];
		else if (!strcmp(argv[i], "--socket") && i + 1 < argc)
			sock_path = argv[++i];
		else if (!strcmp(argv[i], "--set-clock"))
			set_clock = true;
		else if (!strcmp(argv[i], "--replay") && i + 1 < argc)
			replay = argv[++i];
		else if (!strcmp(argv[i], "-v"))
			verbose++;
		else {
			fprintf(stderr, "usage: %s [--device PATH] [--socket PATH] [--set-clock] "
				"[--replay FILE] [-v]\n", argv[0]);
			return 2;
		}
	}
	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);
	signal(SIGPIPE, SIG_IGN);
	hbas_gps_init(&g);

	if (replay) {
		if (!(rf = fopen(replay, "r"))) {
			logmsg("%s: %s", replay, strerror(errno));
			return 1;
		}
	} else {
		if ((fd = open(dev, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC)) < 0) {
			logmsg("%s: %s", dev, strerror(errno));
			return 1;
		}
		/* exactly what stock boot.sh does: 9600, PUBX,41 -> 57600 */
		if (set_baud(fd, 9600) == 0) {
			if (write(fd, HBAS_UBX_PUBX41_57600, strlen(HBAS_UBX_PUBX41_57600)) < 0)
				logmsg("write: %s", strerror(errno));
			tcdrain(fd);
			usleep(100000);                     /* let it switch */
		}
		set_baud(fd, hunt[hunt_i]);
		hunt_until = now_ms() + HUNT_LISTEN_MS;
	}
	if ((ls = listen_socket(sock_path)) < 0) {
		logmsg("%s: %s", sock_path, strerror(errno));
		return 1;
	}
	logmsg("ready on %s (%s)", sock_path, replay ? replay : dev);

	while (!stop) {
		struct pollfd p[2 + MAX_CLIENTS];
		int n = 0;
		uint64_t t;

		p[n++] = (struct pollfd){ .fd = fd, .events = POLLIN };
		p[n++] = (struct pollfd){ .fd = ls, .events = POLLIN };
		for (int i = 0; i < MAX_CLIENTS; i++)
			p[n++] = (struct pollfd){ .fd = clients[i], .events = POLLIN };
		if (poll(p, (nfds_t)n, 100) < 0 && errno != EINTR)
			break;
		t = now_ms();

		/* receiver (or replay) bytes -> lines -> parser */
		if (rf && t >= next_replay) {
			/* replay: feed lines up to and including the next RMC, once a second */
			while (fgets(line, sizeof(line), rf)) {
				if (hbas_nmea_feed(&g, line) == 1)
					last_good = t;
				if (strstr(line, "RMC,"))
					break;
			}
			if (feof(rf))
				rewind(rf);
			locked = true;
			next_replay = t + 1000;
		}
		if (fd >= 0 && (p[0].revents & POLLIN)) {
			char buf[256];
			ssize_t r;

			while ((r = read(fd, buf, sizeof(buf))) > 0) {
				for (ssize_t i = 0; i < r; i++) {
					char c = buf[i];

					if (c == '$')
						llen = 0;       /* resync on every sentence start */
					if (c == '\n' || llen >= sizeof(line) - 1) {
						int rc;

						line[llen] = '\0';
						rc = llen ? hbas_nmea_feed(&g, line) : 0;
						llen = 0;
						if (rc >= 0 && line[0] == '$') {
							good_run++;
							last_good = t;
							if (verbose > 1)
								logmsg("%s", line);
						} else if (rc < 0) {
							good_run = 0;
						}
						continue;
					}
					if (c != '\r')
						line[llen++] = c;
				}
			}
		}

		/* link state: lock after a few good sentences; hunt when quiet */
		if (fd >= 0) {
			if (!locked && good_run >= LOCK_SENTENCES) {
				locked = true;
				logmsg("receiver found at %u baud", hunt[hunt_i]);
			}
			if (locked && t - last_good > QUIET_MS) {
				locked = false;
				good_run = 0;
				hunt_until = t;
				logmsg("receiver went quiet: searching again");
			}
			if (!locked && t >= hunt_until) {
				hunt_i = (hunt_i + 1) % (sizeof(hunt) / sizeof(hunt[0]));
				set_baud(fd, hunt[hunt_i]);
				good_run = 0;
				hunt_until = t + HUNT_LISTEN_MS;
				if (verbose)
					logmsg("trying %u baud", hunt[hunt_i]);
			}
		}

		/* new clients get the current state at once */
		if (p[1].revents & POLLIN) {
			int c = accept4(ls, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);

			for (int i = 0; c >= 0 && i < MAX_CLIENTS; i++) {
				if (clients[i] < 0) {
					clients[i] = c;
					c = -1;
					next_publish = 0;
				}
			}
			if (c >= 0)
				close(c);
		}
		for (int i = 0; i < MAX_CLIENTS; i++) {
			char junk[64];

			if (clients[i] >= 0 && (p[2 + i].revents & (POLLIN | POLLHUP | POLLERR)) &&
			    recv(clients[i], junk, sizeof(junk), MSG_DONTWAIT) <= 0) {
				close(clients[i]);
				clients[i] = -1;
			}
		}

		if (t >= next_publish) {
			if (hbas_gps_format(out, sizeof(out), &g, locked) > 0)
				broadcast(out);
			maybe_set_clock(&g);
			next_publish = t + 1000;
		}
	}
	unlink(sock_path);
	return 0;
}
