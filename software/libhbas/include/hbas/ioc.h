/* SPDX-License-Identifier: MIT */
/*
 * IOC (front/IO controller) link, as implemented by stock dev-ipc and the
 * stock Lua services. Every rule here is from docs/findings/CROSS_CHECKS.md
 * sec. 12; addresses in comments are dev-ipc virtual addresses.
 *
 * Transport: SPI mode 3, 8-bit, 750 kHz (stock), plus two GPIOs: REQ (IOC
 * has data) and ACK (IOC finished handling the last transfer).
 *   write: [A2 N] wait ACK, [channel, data...] (N bytes) wait ACK
 *   read:  [A1 00] wait ACK, read [A1 N] wait ACK, read N bytes wait ACK
 * Payload byte 0 carries the channel in bits 5:0.
 */
#ifndef HBAS_IOC_H
#define HBAS_IOC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HBAS_IOC_CMD_WRITE    0xA2
#define HBAS_IOC_CMD_READ     0xA1
#define HBAS_IOC_CH_MASK      0x3F
#define HBAS_IOC_MAX_PAYLOAD  255     /* N is one byte, and includes the channel byte */
#define HBAS_IOC_ACK_TIMEOUT_MS 50    /* stock -A50 */

/* Wire channel numbers: 0 flow control, 1 watchdog (driver-internal), 2+ application */
enum {
	HBAS_IOC_CH_FLOW = 0,
	HBAS_IOC_CH_WATCHDOG = 1,
	HBAS_IOC_CH_POWER = 2,      /* onOff.lua */
	HBAS_IOC_CH_FACEPLATE = 3,  /* faceplate.lua / iocInterface */
	HBAS_IOC_CH_VEHICLE = 4,    /* vehicleCAN.lua */
	HBAS_IOC_CH_ECU_RESET = 7,  /* ecu-reset.lua (KWP2000) */
};
#define HBAS_IOC_NUM_CHANNELS 10      /* stock -c8 data channels + 0 and 1 */

/*
 * Hardware access, supplied by the caller (spidev + GPIO on the unit, a
 * simulator in tests). transfer() is one full-duplex SPI transfer with CS held
 * for its whole length. wait_ack() returns 0 on an ACK edge, -1 on timeout.
 * drain_ack() discards ACK edges that arrived early.
 */
struct hbas_ioc_bus {
	void *ctx;
	int (*transfer)(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len);
	int (*wait_ack)(void *ctx, int timeout_ms);
	void (*drain_ack)(void *ctx);
};

enum hbas_ioc_err {
	HBAS_IOC_OK = 0,
	HBAS_IOC_EBUS = -1,         /* transfer() failed */
	HBAS_IOC_ETIMEDOUT = -2,    /* no ACK within HBAS_IOC_ACK_TIMEOUT_MS */
	HBAS_IOC_EPROTO = -3,       /* read reply didn't start with A1 */
	HBAS_IOC_E2BIG = -4,        /* payload doesn't fit */
};

/* Send one message on a channel. */
int hbas_ioc_send(const struct hbas_ioc_bus *bus, uint8_t channel,
		  const uint8_t *data, size_t len);

/*
 * Fetch one message. Returns 1 and fills channel/data/len if the IOC had
 * one, 0 if it had none (N = 0), or a negative hbas_ioc_err.
 */
int hbas_ioc_receive(const struct hbas_ioc_bus *bus, uint8_t *channel,
		     uint8_t *data, size_t cap, size_t *len);

/*
 * Channel-0 flow-control message (builder 0x102A60): [0x10 version, count,
 * entry per channel 0..count-1], entry bit 7 = XON, bits 6:0 = receive-queue
 * fill. Returns the message length (without the channel byte), or 0 if cap
 * is too small.
 */
size_t hbas_ioc_flow_build(uint8_t *out, size_t cap, const bool xon[HBAS_IOC_NUM_CHANNELS]);

/* Parse a received flow-control message; xon[i] gets each channel's state. */
int hbas_ioc_flow_parse(const uint8_t *msg, size_t len, bool xon[HBAS_IOC_NUM_CHANNELS]);

/* ---- channel 2: power management (onOff.lua) ---------------------------- */

enum {
	HBAS_PWR_IOC_SHUTDOWN_REQUEST = 0,
	HBAS_PWR_IOC_NORMAL_OPERATION = 1,
	HBAS_PWR_IOC_SOFTWARE_VERSION = 2,
	HBAS_PWR_IOC_DIAG_IDENTIFIER = 3,
	HBAS_PWR_IOC_EMERGENCY_SHUTDOWN = 4,
	HBAS_PWR_IOC_LOG_SHUTDOWN = 102,
};
#define HBAS_DID_CONFIGURATION_OPTIONS 0xF1E8
enum { HBAS_PWR_READY_FOR_SHUTDOWN = 0, HBAS_PWR_KEEP_RUNNING = 1 };
enum { HBAS_PWR_REQ_NORMAL = 0, HBAS_PWR_REQ_RESET_TO_BOOTLOADER = 1,
       HBAS_PWR_REQ_RESET_TO_APPLICATION = 2 };
enum { HBAS_IOC_STATE_BOOTLOADER = 0, HBAS_IOC_STATE_APPLICATION = 1,
       HBAS_IOC_STATE_FC_UPDATE = 2 };

struct hbas_power {
	/* configuration (stock defaults: both 0) */
	uint8_t force_on;
	uint8_t expected_amps;      /* bit0 = amp1 .. bit3 = amp4 */
	bool software_update_mode;  /* stock SWDL mode: IOC bootloader is expected */

	/* state reported by the IOC */
	bool seen_normal_operation;
	uint8_t ioc_state;
	uint16_t battery_raw;       /* V = raw * 27.5 / 1023 */
	bool appear_off_request;    /* "silent boot" bit */
	uint8_t amps_present;       /* bit0 = amp1 .. bit3 = amp4 */
	bool shutdown_requested;
	bool emergency_shutdown;
	uint8_t shutdown_reasons[8];
	uint8_t n_shutdown_reasons;

	/* DID 0xF1E8 HD_Configuration_Options (onOff.lua parse_0xF1E8): byte 0
	 * is the bike configuration (hbas/bike.h), byte 1 CB/intercom/nav/TPMS
	 * flags. Set once the IOC has sent a written value. */
	bool have_config_options;
	uint8_t config_options[8];
};

/*
 * Handle one channel-2 message from the IOC. If the stock service would
 * reply immediately, writes the 4-byte reply to reply[] and returns 4,
 * otherwise returns 0. A shutdown request only sets shutdown_requested: the
 * caller finishes its own cleanup, then sends hbas_power_ready_reply().
 */
size_t hbas_power_handle(struct hbas_power *p, const uint8_t *msg, size_t len,
			 uint8_t reply[4]);

/* READY_FOR_SHUTDOWN reply, sent after cleanup (stock completeShutdown()). */
void hbas_power_ready_reply(const struct hbas_power *p, uint8_t reply[4]);

unsigned hbas_power_battery_mv(const struct hbas_power *p);

#endif
