// SPDX-License-Identifier: MIT
/*
 * IOC link tests against a simulated IOC that enforces the stock framing:
 * every transfer must be one the IOC expects at that point, and it only
 * produces an ACK after a transfer it accepted.
 */
#include "hbas/ioc.h"
#include "hbas/vehicle.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK(%s) failed\n", \
	__FILE__, __LINE__, #c); failures++; } } while (0)

/* ---- simulated IOC ------------------------------------------------------ */

enum sim_phase { WANT_HEADER, WANT_WRITE_DATA, WANT_READ_LEN, WANT_READ_DATA };

struct sim {
	enum sim_phase phase;
	int pending_acks;
	bool drop_acks;             /* simulate a dead/unpowered IOC */
	uint8_t expect_n;
	/* host -> IOC, as received */
	uint8_t rx_ch[16], rx_msg[16][64];
	size_t rx_len[16], n_rx;
	/* IOC -> host queue: [channel, data...] */
	uint8_t tx_q[16][64];
	size_t tx_len[16], tx_head, tx_tail;
	int protocol_errors;
};

static int sim_transfer(void *c, const uint8_t *tx, uint8_t *rx, size_t len)
{
	struct sim *s = c;

	memset(rx, 0, len);
	switch (s->phase) {
	case WANT_HEADER:
		if (len != 2) { s->protocol_errors++; return 0; }
		if (tx[0] == HBAS_IOC_CMD_WRITE && tx[1] >= 1) {
			s->expect_n = tx[1];
			s->phase = WANT_WRITE_DATA;
		} else if (tx[0] == HBAS_IOC_CMD_READ && tx[1] == 0) {
			s->phase = WANT_READ_LEN;
		} else {
			s->protocol_errors++;
			return 0;
		}
		break;
	case WANT_WRITE_DATA:
		if (len != s->expect_n) { s->protocol_errors++; return 0; }
		s->rx_ch[s->n_rx] = tx[0] & 0x3F;
		memcpy(s->rx_msg[s->n_rx], tx + 1, len - 1);
		s->rx_len[s->n_rx++] = len - 1;
		s->phase = WANT_HEADER;
		break;
	case WANT_READ_LEN:
		if (len != 2) { s->protocol_errors++; return 0; }
		rx[0] = HBAS_IOC_CMD_READ;
		rx[1] = s->tx_head == s->tx_tail ? 0 : (uint8_t)s->tx_len[s->tx_head];
		s->expect_n = rx[1];
		s->phase = rx[1] ? WANT_READ_DATA : WANT_HEADER;
		break;
	case WANT_READ_DATA:
		if (len != s->expect_n) { s->protocol_errors++; return 0; }
		memcpy(rx, s->tx_q[s->tx_head], len);
		s->tx_head++;
		s->phase = WANT_HEADER;
		break;
	}
	if (!s->drop_acks)
		s->pending_acks++;
	return 0;
}

static int sim_wait_ack(void *c, int timeout_ms)
{
	struct sim *s = c;

	(void)timeout_ms;
	if (!s->pending_acks)
		return -1;
	s->pending_acks--;
	return 0;
}

static void sim_drain(void *c) { ((struct sim *)c)->pending_acks = 0; }

static void sim_queue(struct sim *s, uint8_t ch, const uint8_t *d, size_t n)
{
	s->tx_q[s->tx_tail][0] = ch;
	memcpy(&s->tx_q[s->tx_tail][1], d, n);
	s->tx_len[s->tx_tail++] = n + 1;
}

static struct hbas_ioc_bus bus_for(struct sim *s)
{
	struct hbas_ioc_bus b = { s, sim_transfer, sim_wait_ack, sim_drain };
	return b;
}

/* ---- tests -------------------------------------------------------------- */

static void test_send_framing(void)
{
	struct sim s = { 0 };
	struct hbas_ioc_bus b = bus_for(&s);
	const uint8_t msg[] = { 1, 0, 0, 0 };

	CHECK(hbas_ioc_send(&b, HBAS_IOC_CH_POWER, msg, sizeof(msg)) == HBAS_IOC_OK);
	CHECK(s.protocol_errors == 0);
	CHECK(s.n_rx == 1 && s.rx_ch[0] == 2 && s.rx_len[0] == 4);
	CHECK(memcmp(s.rx_msg[0], msg, 4) == 0);
}

static void test_receive_framing(void)
{
	struct sim s = { 0 };
	struct hbas_ioc_bus b = bus_for(&s);
	const uint8_t can[] = { 0x41, 0x05, 8, 0x0B, 0xB8, 0x03, 0xE8, 0, 0, 4, 0 };
	uint8_t ch, buf[64];
	size_t n;

	CHECK(hbas_ioc_receive(&b, &ch, buf, sizeof(buf), &n) == 0);   /* nothing queued */
	sim_queue(&s, HBAS_IOC_CH_VEHICLE, can, sizeof(can));
	CHECK(hbas_ioc_receive(&b, &ch, buf, sizeof(buf), &n) == 1);
	CHECK(ch == 4 && n == sizeof(can) && memcmp(buf, can, n) == 0);
	CHECK(s.protocol_errors == 0);

	/* end to end: channel-4 payload -> CAN decoder */
	{
		struct hbas_vehicle v;
		uint16_t id;
		const uint8_t *d;
		size_t dl;
		unsigned kph;

		hbas_vehicle_init(&v);
		CHECK(hbas_ioc_can_unwrap(buf, n, &id, &d, &dl) == 0);
		CHECK(hbas_vehicle_decode(&v, id, d, dl) == HBAS_DECODED);
		CHECK(v.rpm == 3000 && hbas_speed_kph_x10(&v, &kph) && kph == 1000 && v.gear_raw == 4);
	}
}

static void test_dead_ioc_times_out(void)
{
	struct sim s = { .drop_acks = true };
	struct hbas_ioc_bus b = bus_for(&s);
	const uint8_t msg[] = { 1 };
	uint8_t ch, buf[8];
	size_t n;

	CHECK(hbas_ioc_send(&b, 2, msg, 1) == HBAS_IOC_ETIMEDOUT);
	s.phase = WANT_HEADER;
	CHECK(hbas_ioc_receive(&b, &ch, buf, sizeof(buf), &n) == HBAS_IOC_ETIMEDOUT);
}

static void test_flow_control(void)
{
	bool xon[HBAS_IOC_NUM_CHANNELS] = { [0] = true, [2] = true, [3] = true, [4] = true };
	bool back[HBAS_IOC_NUM_CHANNELS] = { 0 };
	uint8_t m[32];
	size_t n = hbas_ioc_flow_build(m, sizeof(m), xon);

	CHECK(n == 12 && m[0] == 0x10 && m[1] == 10);
	CHECK(m[2] == 0x80 && m[3] == 0x00 && m[4] == 0x80 && m[7] == 0x00);
	CHECK(hbas_ioc_flow_parse(m, n, back) == 0);
	CHECK(memcmp(xon, back, sizeof(xon)) == 0);
	CHECK(hbas_ioc_flow_parse(m, 5, back) == -1);              /* truncated */
}

static void test_power_keepalive(void)
{
	struct hbas_power p = { .expected_amps = 0x03 };
	uint8_t r[4];
	/* normal operation, IOC in application, battery raw 0x01D2 (466 -> 12.53 V), amp1+2 */
	const uint8_t normal[] = { 1, 1, 0xD2, 0x01, 0x30 };

	CHECK(hbas_power_handle(&p, normal, sizeof(normal), r) == 4);
	CHECK(r[0] == 1 && r[1] == 0 && r[2] == 0 && r[3] == 0x03);  /* KEEP_RUNNING */
	CHECK(p.battery_raw == 0x1D2 && hbas_power_battery_mv(&p) == 12527);
	CHECK(p.amps_present == 0x3 && !p.appear_off_request);
}

static void test_power_ioc_in_bootloader(void)
{
	struct hbas_power p = { 0 };
	uint8_t r[4];
	const uint8_t bolo[] = { 1, 0, 0, 0, 0 };

	CHECK(hbas_power_handle(&p, bolo, sizeof(bolo), r) == 4);
	CHECK(r[0] == 1 && r[1] == 2 && r[2] == 0);                  /* reset IOC to app */
	p.software_update_mode = true;
	CHECK(hbas_power_handle(&p, bolo, sizeof(bolo), r) == 0);    /* expected in SWDL */
}

static void test_power_shutdown(void)
{
	struct hbas_power p = { .force_on = 0 };
	uint8_t r[4];
	const uint8_t req[] = { 0 }, log[] = { 102, 2, 1, 10 };

	CHECK(hbas_power_handle(&p, req, 1, r) == 0);                /* no immediate reply */
	CHECK(p.shutdown_requested);
	hbas_power_ready_reply(&p, r);
	CHECK(r[0] == 0 && r[1] == 0);                               /* READY_FOR_SHUTDOWN */
	CHECK(hbas_power_handle(&p, log, sizeof(log), r) == 0);
	CHECK(p.n_shutdown_reasons == 2 && p.shutdown_reasons[1] == 10);
}

static void test_short_messages_ignored(void)
{
	struct hbas_power p = { 0 };
	uint8_t r[4];
	const uint8_t shortnorm[] = { 1, 1, 0 };

	CHECK(hbas_power_handle(&p, shortnorm, sizeof(shortnorm), r) == 0);
	CHECK(!p.seen_normal_operation);
	CHECK(hbas_power_handle(&p, NULL, 0, r) == 0);
}

int main(void)
{
	test_send_framing();
	test_receive_framing();
	test_dead_ioc_times_out();
	test_flow_control();
	test_power_keepalive();
	test_power_ioc_in_bootloader();
	test_power_shutdown();
	test_short_messages_ignored();
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return EXIT_FAILURE;
	}
	puts("test_ioc: all checks passed");
	return EXIT_SUCCESS;
}
