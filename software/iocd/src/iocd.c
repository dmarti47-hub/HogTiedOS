// SPDX-License-Identifier: MIT
/*
 * hbas-iocd: talks to the IOC (front/IO controller) on the unit.
 *
 *  - answers the IOC's power keep-alive on channel 2 exactly like stock
 *    onOff.lua; without it the IOC cuts power ("J3 IPC Watchdog")
 *  - on an IOC shutdown request: sync(), then READY_FOR_SHUTDOWN
 *  - forwards channel-4 bike CAN frames to a SocketCAN interface (vcan0),
 *    where hogtied-ui (--can vcan0) and candump can read them
 *  - turns channel-3 handlebar/front-panel buttons into key events on a
 *    uinput keyboard ("hbas-buttons"), using the stock key letters
 *
 * Hardware: McSPI3 via spidev (mode 3, 8-bit, 750 kHz as stock programs it),
 * IPC_REQ = GPIO136 and IPC_ACK = GPIO137 (gpio5 lines 8 and 9) via the GPIO
 * v1 character device. Edge polarity of REQ/ACK isn't known, so both edges
 * are used. Protocol rules: docs/findings/CROSS_CHECKS.md sec. 12.
 *
 * Untested on hardware.
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
#include <time.h>
#include <unistd.h>

#include <linux/can.h>
#include <linux/gpio.h>
#include <linux/spi/spidev.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

#include <linux/input.h>
#include <linux/uinput.h>

#include "hbas/buttons.h"
#include "hbas/ioc.h"
#include "hbas/vehicle.h"

#define GPIO_CHIP_LABEL "49056000.gpio"   /* OMAP GPIO5: GPIO128-159 */
#define REQ_LINE 8                         /* GPIO136 */
#define ACK_LINE 9                         /* GPIO137 */
#define SPI_HZ 750000
#define POLL_MS 100                        /* also poll without REQ, in case its edge is missed */

static int verbose;
static volatile sig_atomic_t stop;

static void logmsg(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	fputs("hbas-iocd: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

/* ---- hardware bus ------------------------------------------------------- */

struct hw {
	int spi, req_fd, ack_fd;
};

static int hw_transfer(void *c, const uint8_t *tx, uint8_t *rx, size_t len)
{
	struct hw *h = c;
	struct spi_ioc_transfer t = {
		.tx_buf = (unsigned long)tx, .rx_buf = (unsigned long)rx,
		.len = (uint32_t)len, .speed_hz = SPI_HZ, .bits_per_word = 8,
	};

	return ioctl(h->spi, SPI_IOC_MESSAGE(1), &t) < 0 ? -1 : 0;
}

static int read_edge(int fd)
{
	struct gpioevent_data ev;

	return read(fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev) ? 0 : -1;
}

static int hw_wait_ack(void *c, int timeout_ms)
{
	struct hw *h = c;
	struct pollfd p = { .fd = h->ack_fd, .events = POLLIN };

	if (poll(&p, 1, timeout_ms) != 1)
		return -1;
	return read_edge(h->ack_fd);
}

static void hw_drain_ack(void *c)
{
	struct hw *h = c;
	struct pollfd p = { .fd = h->ack_fd, .events = POLLIN };

	while (poll(&p, 1, 0) == 1 && read_edge(h->ack_fd) == 0)
		;
}

static int open_spi(const char *dev)
{
	uint8_t mode = SPI_MODE_3, bits = 8;
	uint32_t hz = SPI_HZ;
	int fd = open(dev, O_RDWR | O_CLOEXEC);

	if (fd < 0 || ioctl(fd, SPI_IOC_WR_MODE, &mode) < 0 ||
	    ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
	    ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &hz) < 0) {
		logmsg("%s: %s", dev, strerror(errno));
		return -1;
	}
	return fd;
}

static int find_gpiochip(char *path, size_t n)
{
	for (int i = 0; i < 16; i++) {
		struct gpiochip_info info;
		int fd;

		snprintf(path, n, "/dev/gpiochip%d", i);
		fd = open(path, O_RDONLY | O_CLOEXEC);
		if (fd < 0)
			continue;
		if (ioctl(fd, GPIO_GET_CHIPINFO_IOCTL, &info) == 0 &&
		    !strcmp(info.label, GPIO_CHIP_LABEL))
			return fd;
		close(fd);
	}
	return -1;
}

static int request_edges(int chip, unsigned line, const char *label)
{
	struct gpioevent_request r = {
		.lineoffset = line,
		.handleflags = GPIOHANDLE_REQUEST_INPUT,
		.eventflags = GPIOEVENT_REQUEST_BOTH_EDGES,
	};

	snprintf(r.consumer_label, sizeof(r.consumer_label), "%s", label);
	if (ioctl(chip, GPIO_GET_LINEEVENT_IOCTL, &r) < 0) {
		logmsg("GPIO line %u (%s): %s", line, label, strerror(errno));
		return -1;
	}
	return r.fd;
}

/* ---- SocketCAN output --------------------------------------------------- */

static int open_can(const char *ifname)
{
	struct sockaddr_can a = { .can_family = AF_CAN };
	struct ifreq ifr;
	int s = socket(PF_CAN, SOCK_RAW, CAN_RAW);

	if (s < 0)
		return -1;
	snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname);
	if (ioctl(s, SIOCGIFINDEX, &ifr) < 0)
		goto fail;
	a.can_ifindex = ifr.ifr_ifindex;
	if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0)
		goto fail;
	return s;
fail:
	logmsg("CAN %s: %s", ifname, strerror(errno));
	close(s);
	return -1;
}

static void forward_can(int s, const uint8_t *msg, size_t len)
{
	struct can_frame f = { 0 };
	uint16_t id;
	const uint8_t *d;
	size_t dl;

	if (s < 0 || hbas_ioc_can_unwrap(msg, len, &id, &d, &dl))
		return;
	f.can_id = id & CAN_SFF_MASK;
	f.can_dlc = dl > 8 ? 8 : (uint8_t)dl;
	memcpy(f.data, d, f.can_dlc);
	if (write(s, &f, sizeof(f)) != (ssize_t)sizeof(f) && verbose)
		logmsg("CAN write: %s", strerror(errno));
}

/* ---- buttons -> uinput keyboard ----------------------------------------- */

/*
 * Stock key codes are ASCII (CROSS_CHECKS sec. 13). They're reported as the
 * Linux key with the same character, so the mapping stays 1:1 with stock:
 * right handlebar U/J/H/K/Enter, left W/S/A/D/Space, home I, power O, ...
 */
static int linux_key(uint8_t c)
{
	static const int letters[26] = {
		KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I, KEY_J,
		KEY_K, KEY_L, KEY_M, KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T,
		KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z,
	};
	static const int digits[10] = {
		KEY_0, KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9,
	};

	if (c >= 'A' && c <= 'Z')
		return letters[c - 'A'];
	if (c >= '0' && c <= '9')
		return digits[c - '0'];
	switch (c) {
	case 13: return KEY_ENTER;
	case ' ': return KEY_SPACE;
	case '\\': return KEY_BACKSLASH;
	case ',': return KEY_COMMA;
	case '.': return KEY_DOT;
	case '+': return KEY_KPPLUS;
	case ';': return KEY_SEMICOLON;
	case '\'': return KEY_APOSTROPHE;
	case '`': return KEY_GRAVE;
	case '/': return KEY_SLASH;
	case '-': return KEY_MINUS;
	case '[': return KEY_LEFTBRACE;
	case ']': return KEY_RIGHTBRACE;
	default: return -1;
	}
}

static int open_uinput(void)
{
	struct uinput_setup us = { .id = { .bustype = BUS_VIRTUAL } };
	int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);

	if (fd < 0) {
		logmsg("/dev/uinput: %s (buttons disabled)", strerror(errno));
		return -1;
	}
	ioctl(fd, UI_SET_EVBIT, EV_KEY);
	for (int c = 0; c < 128; c++)
		if (linux_key((uint8_t)c) >= 0)
			ioctl(fd, UI_SET_KEYBIT, linux_key((uint8_t)c));
	snprintf(us.name, sizeof(us.name), "hbas-buttons");
	if (ioctl(fd, UI_DEV_SETUP, &us) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
		logmsg("uinput setup: %s (buttons disabled)", strerror(errno));
		close(fd);
		return -1;
	}
	return fd;
}

static void emit_key(int fd, int code, int value)
{
	struct input_event ev[2] = {
		{ .type = EV_KEY, .code = (uint16_t)code, .value = value },
		{ .type = EV_SYN, .code = SYN_REPORT },
	};

	if (write(fd, ev, sizeof(ev)) != (ssize_t)sizeof(ev) && verbose)
		logmsg("uinput write: %s", strerror(errno));
}

static void handle_buttons(int ui, struct hbas_buttons *btn, const uint8_t *msg, size_t len)
{
	struct hbas_key_event ev[32];
	size_t n = hbas_buttons_decode(btn, msg, len, ev, 32);

	for (size_t i = 0; i < n; i++) {
		int key = linux_key(ev[i].code);
		const char *name = hbas_key_name(ev[i].code);

		if (verbose)
			logmsg("key %s (%d) %s", name ? name : "?", ev[i].code,
			       ev[i].pressed ? "down" : "up");
		if (ui >= 0 && key >= 0)
			emit_key(ui, key, ev[i].pressed);
	}
}

/* ---- main loop ---------------------------------------------------------- */

static void hexlog(const char *what, uint8_t ch, const uint8_t *d, size_t n)
{
	char buf[3 * 64 + 1] = "";

	for (size_t i = 0; i < n && i < 64; i++)
		snprintf(buf + 3 * i, 4, "%02X ", d[i]);
	logmsg("%s ch%u len %zu: %s", what, ch, n, buf);
}

static void on_sig(int s) { (void)s; stop = 1; }

int main(int argc, char **argv)
{
	const char *spidev = NULL, *canif = "vcan0";
	struct hw h = { -1, -1, -1 };
	struct hbas_ioc_bus bus = { &h, hw_transfer, hw_wait_ack, hw_drain_ack };
	struct hbas_power pwr = { 0 };
	struct hbas_buttons btn;
	int ui;
	bool xon[HBAS_IOC_NUM_CHANNELS] = { 0 };
	char chip_path[32];
	uint8_t flow[32], msg[HBAS_IOC_MAX_PAYLOAD], reply[4];
	int chip, can, opt;
	bool shutdown_answered = false;

	while ((opt = getopt(argc, argv, "s:c:v")) != -1) {
		switch (opt) {
		case 's': spidev = optarg; break;
		case 'c': canif = optarg; break;
		case 'v': verbose++; break;
		default:
			fprintf(stderr, "usage: %s -s /dev/spidevB.C [-c vcan0] [-v]\n", argv[0]);
			return 2;
		}
	}
	if (!spidev) {
		fprintf(stderr, "%s: -s /dev/spidevB.C is required\n", argv[0]);
		return 2;
	}
	signal(SIGTERM, on_sig);
	signal(SIGINT, on_sig);

	if ((h.spi = open_spi(spidev)) < 0)
		return 1;
	if ((chip = find_gpiochip(chip_path, sizeof(chip_path))) < 0) {
		logmsg("no gpiochip labelled %s", GPIO_CHIP_LABEL);
		return 1;
	}
	if ((h.req_fd = request_edges(chip, REQ_LINE, "ioc-req")) < 0 ||
	    (h.ack_fd = request_edges(chip, ACK_LINE, "ioc-ack")) < 0)
		return 1;
	can = open_can(canif);          /* optional: keep running without it */
	ui = open_uinput();             /* optional too */
	hbas_buttons_init(&btn, HBAS_LAYOUT_A);   /* the layout stock always uses */

	/* Announce the channels we serve (unopened channels are XOFF in stock). */
	xon[HBAS_IOC_CH_FLOW] = xon[HBAS_IOC_CH_POWER] = true;
	xon[HBAS_IOC_CH_FACEPLATE] = xon[HBAS_IOC_CH_VEHICLE] = true;
	{
		size_t n = hbas_ioc_flow_build(flow, sizeof(flow), xon);
		int rc = hbas_ioc_send(&bus, HBAS_IOC_CH_FLOW, flow, n);

		logmsg("flow control XON ch0,2,3,4: %s", rc ? "no ACK (IOC not responding?)" : "ok");
	}

	while (!stop) {
		struct pollfd p = { .fd = h.req_fd, .events = POLLIN };
		uint8_t ch;
		size_t len;
		int rc;

		if (poll(&p, 1, POLL_MS) == 1)
			read_edge(h.req_fd);
		/* drain everything the IOC has queued */
		while ((rc = hbas_ioc_receive(&bus, &ch, msg, sizeof(msg), &len)) == 1) {
			if (verbose > 1)
				hexlog("rx", ch, msg, len);
			switch (ch) {
			case HBAS_IOC_CH_POWER:
				if (hbas_power_handle(&pwr, msg, len, reply) &&
				    hbas_ioc_send(&bus, HBAS_IOC_CH_POWER, reply, 4))
					logmsg("power reply not acknowledged");
				if (pwr.shutdown_requested && !shutdown_answered) {
					logmsg("IOC shutdown request: syncing");
					sync();
					hbas_power_ready_reply(&pwr, reply);
					hbas_ioc_send(&bus, HBAS_IOC_CH_POWER, reply, 4);
					shutdown_answered = true;
				}
				break;
			case HBAS_IOC_CH_VEHICLE:
				forward_can(can, msg, len);
				break;
			case HBAS_IOC_CH_FACEPLATE:
				if (verbose)          /* raw bytes, to confirm layout A on hardware */
					hexlog("faceplate", ch, msg, len);
				handle_buttons(ui, &btn, msg, len);
				break;
			case HBAS_IOC_CH_FLOW: {
				bool their[HBAS_IOC_NUM_CHANNELS] = { 0 };

				if (!hbas_ioc_flow_parse(msg, len, their) && verbose)
					logmsg("IOC flow: link %s", their[0] ? "on" : "off");
				break;
			}
			default:
				if (verbose)
					hexlog("unhandled", ch, msg, len);
			}
		}
		if (rc < 0 && verbose)
			logmsg("receive error %d", rc);
	}
	logmsg("exiting");
	return 0;
}
