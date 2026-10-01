// SPDX-License-Identifier: MIT
#include "hbas/ioc.h"

#include <string.h>

#define FLOW_VERSION 0x10     /* stock default (IPC protocol v1.0), dev-ipc cfg+0x4c */

static int xfer_ack(const struct hbas_ioc_bus *bus, const uint8_t *tx, uint8_t *rx, size_t len)
{
	if (bus->transfer(bus->ctx, tx, rx, len))
		return HBAS_IOC_EBUS;
	if (bus->wait_ack(bus->ctx, HBAS_IOC_ACK_TIMEOUT_MS))
		return HBAS_IOC_ETIMEDOUT;
	return HBAS_IOC_OK;
}

static int send_header(const struct hbas_ioc_bus *bus, uint8_t cmd, uint8_t n)
{
	const uint8_t hdr[2] = { cmd, n };
	uint8_t rx[2];

	bus->drain_ack(bus->ctx);         /* 0x103560 drains stale ACKs first */
	return xfer_ack(bus, hdr, rx, sizeof(hdr));
}

int hbas_ioc_send(const struct hbas_ioc_bus *bus, uint8_t channel,
		  const uint8_t *data, size_t len)
{
	uint8_t tx[HBAS_IOC_MAX_PAYLOAD], rx[HBAS_IOC_MAX_PAYLOAD];
	int rc;

	if (len + 1 > HBAS_IOC_MAX_PAYLOAD)
		return HBAS_IOC_E2BIG;
	tx[0] = channel & HBAS_IOC_CH_MASK;
	memcpy(tx + 1, data, len);
	rc = send_header(bus, HBAS_IOC_CMD_WRITE, (uint8_t)(len + 1));
	return rc ? rc : xfer_ack(bus, tx, rx, len + 1);
}

int hbas_ioc_receive(const struct hbas_ioc_bus *bus, uint8_t *channel,
		     uint8_t *data, size_t cap, size_t *len)
{
	uint8_t zeros[HBAS_IOC_MAX_PAYLOAD] = { 0 }, rx[HBAS_IOC_MAX_PAYLOAD];
	uint8_t n;
	int rc;

	if ((rc = send_header(bus, HBAS_IOC_CMD_READ, 0)))
		return rc;
	if ((rc = xfer_ack(bus, zeros, rx, 2)))
		return rc;
	if (rx[0] != HBAS_IOC_CMD_READ)
		return HBAS_IOC_EPROTO;
	n = rx[1];
	if (n == 0)
		return 0;                 /* "No message from ioc" */
	if ((rc = xfer_ack(bus, zeros, rx, n)))
		return rc;
	if ((size_t)(n - 1) > cap)
		return HBAS_IOC_E2BIG;
	*channel = rx[0] & HBAS_IOC_CH_MASK;
	memcpy(data, rx + 1, n - 1);
	*len = n - 1;
	return 1;
}

size_t hbas_ioc_flow_build(uint8_t *out, size_t cap, const bool xon[HBAS_IOC_NUM_CHANNELS])
{
	if (cap < 2 + HBAS_IOC_NUM_CHANNELS)
		return 0;
	out[0] = FLOW_VERSION;
	out[1] = HBAS_IOC_NUM_CHANNELS;
	for (int i = 0; i < HBAS_IOC_NUM_CHANNELS; i++)
		out[2 + i] = xon[i] ? 0x80 : 0x00;   /* fill level 0: we drain immediately */
	return 2 + HBAS_IOC_NUM_CHANNELS;
}

int hbas_ioc_flow_parse(const uint8_t *msg, size_t len, bool xon[HBAS_IOC_NUM_CHANNELS])
{
	if (len < 2 || len < 2 + (size_t)msg[1])
		return -1;
	for (int i = 0; i < HBAS_IOC_NUM_CHANNELS && i < msg[1]; i++)
		xon[i] = msg[2 + i] & 0x80;
	return 0;
}

static void reply4(const struct hbas_power *p, uint8_t reply[4], uint8_t id, uint8_t req,
		   uint8_t force_on)
{
	reply[0] = id;
	reply[1] = req;
	reply[2] = force_on;
	reply[3] = p->expected_amps;
}

size_t hbas_power_handle(struct hbas_power *p, const uint8_t *m, size_t len, uint8_t reply[4])
{
	if (len < 1)
		return 0;
	switch (m[0]) {
	case HBAS_PWR_IOC_NORMAL_OPERATION:            /* onOff.lua normal_operation() */
		if (len < 5)
			return 0;
		p->seen_normal_operation = true;
		p->ioc_state = m[1];
		p->battery_raw = (uint16_t)(m[3] << 8 | m[2]);   /* msg[4]*256 + msg[3] */
		p->appear_off_request = m[4] & 0x01;
		p->amps_present = m[4] >> 4;
		if (p->ioc_state == HBAS_IOC_STATE_APPLICATION) {
			reply4(p, reply, HBAS_PWR_KEEP_RUNNING, HBAS_PWR_REQ_NORMAL, p->force_on);
			return 4;
		}
		if (p->ioc_state == HBAS_IOC_STATE_BOOTLOADER && !p->software_update_mode) {
			/* stock: IOC shouldn't be in its bootloader; reset it to application */
			reply4(p, reply, HBAS_PWR_KEEP_RUNNING, HBAS_PWR_REQ_RESET_TO_APPLICATION, 0);
			return 4;
		}
		return 0;                         /* FC update / expected bootloader: no reply */

	case HBAS_PWR_IOC_SHUTDOWN_REQUEST:
		p->shutdown_requested = true;
		return 0;

	case HBAS_PWR_IOC_EMERGENCY_SHUTDOWN:
		p->emergency_shutdown = true;
		return 0;

	case HBAS_PWR_IOC_LOG_SHUTDOWN:                /* [102, count, reason...] */
		p->n_shutdown_reasons = 0;
		for (size_t i = 0; len >= 2 && i < m[1] && 2 + i < len &&
		     i < sizeof(p->shutdown_reasons); i++)
			p->shutdown_reasons[p->n_shutdown_reasons++] = m[2 + i];
		return 0;

	case HBAS_PWR_IOC_DIAG_IDENTIFIER: {          /* [3, DID lo, DID hi, value...] */
		/* onOff.lua diag_identifier(): the IOC sends every DID at startup,
		 * and an empty one hasn't been written, so it changes nothing */
		uint16_t did = len >= 3 ? (uint16_t)(m[2] << 8 | m[1]) : 0;

		if (did == HBAS_DID_CONFIGURATION_OPTIONS && len > 3) {
			size_t n = len - 3 < sizeof(p->config_options) ? len - 3
								       : sizeof(p->config_options);

			memcpy(p->config_options, m + 3, n);
			p->have_config_options = true;
		}
		return 0;
	}

	default:
		return 0;                         /* version info: not needed yet */
	}
}

void hbas_power_ready_reply(const struct hbas_power *p, uint8_t reply[4])
{
	reply4(p, reply, HBAS_PWR_READY_FOR_SHUTDOWN, HBAS_PWR_REQ_NORMAL, p->force_on);
}

unsigned hbas_power_battery_mv(const struct hbas_power *p)
{
	return (unsigned)(((unsigned long)p->battery_raw * 27500 + 511) / 1023);  /* rounded */
}
