// SPDX-License-Identifier: MIT
#include "hbas/buttons.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK(%s) failed\n", \
	__FILE__, __LINE__, #c); failures++; } } while (0)

#define MSG(id, ...) ((const uint8_t[]){ (id) & 0xFF, (id) >> 8, \
	sizeof((const uint8_t[]){ __VA_ARGS__ }), __VA_ARGS__ })
#define DECODE(b, id, ...) hbas_buttons_decode((b), MSG(id, __VA_ARGS__), \
	3 + sizeof((const uint8_t[]){ __VA_ARGS__ }), ev, 16)

static struct hbas_key_event ev[16];

static void test_right_handlebar_layout_a(void)
{
	struct hbas_buttons b;

	hbas_buttons_init(&b, HBAS_LAYOUT_A);
	/* press right-up: payload byte 3 bit 0 */
	CHECK(DECODE(&b, 0x0570, 0, 0, 0, 0x01, 0) == 1);
	CHECK(ev[0].code == 'U' && ev[0].pressed);
	/* still held: no new event */
	CHECK(DECODE(&b, 0x0570, 0, 0, 0, 0x01, 0) == 0);
	/* release up, press center (bit 2) */
	CHECK(DECODE(&b, 0x0570, 0, 0, 0, 0x04, 0) == 2);
	CHECK(ev[0].code == 'U' && !ev[0].pressed);
	CHECK(ev[1].code == 13 && ev[1].pressed);
	/* squelch up (byte 4 bit 0) + system info (byte 1 bit 0) */
	CHECK(DECODE(&b, 0x0570, 0, 0x01, 0, 0x04, 0x01) == 2);
	CHECK(ev[0].code == 'X' && ev[1].code == 'T');
	CHECK(!strcmp(hbas_key_name('X'), "SYSTEM_INFO"));
}

static void test_left_handlebar_layout_a(void)
{
	struct hbas_buttons b;

	hbas_buttons_init(&b, HBAS_LAYOUT_A);
	CHECK(DECODE(&b, 0x0550, 0, 0, 0, 0x02 | 0x10, 0, 0) == 2);   /* left + down */
	CHECK(ev[0].code == 'A' && ev[1].code == 'S');
	CHECK(DECODE(&b, 0x0550, 0, 0, 0, 0, 0, 0x01) == 3);           /* release both, V */
	CHECK(ev[2].code == 'V' && ev[2].pressed);
}

static void test_layout_b_differs(void)
{
	struct hbas_buttons b;

	hbas_buttons_init(&b, HBAS_LAYOUT_B);
	CHECK(DECODE(&b, 0x0570, 0x10, 0, 0, 0, 0) == 1 && ev[0].code == 'U');
	CHECK(DECODE(&b, 0x0570, 0x10, 0, 0, 0x01, 0) == 0);           /* A's up bit: nothing in B */
}

static void test_front_controls(void)
{
	struct hbas_buttons b;

	hbas_buttons_init(&b, HBAS_LAYOUT_A);
	CHECK(DECODE(&b, 0xFACE, 0x01, 0, 0, 0) == 1 && ev[0].code == 'O' && ev[0].pressed);
	CHECK(DECODE(&b, 0xFACE, 0, 0x04, 0, 0) == 2);                 /* release power, press preset 3 */
	CHECK(ev[0].code == 'O' && !ev[0].pressed && ev[1].code == '3');
	CHECK(DECODE(&b, 0xFACE, 0, 0, 0, 0x02) == 2 && ev[1].code == 'U');   /* byte3 bit1 */
	CHECK(DECODE(&b, 0xFACE, 0, 0, 0, 0x02 | 0x40) == 0);          /* unmapped bit 6 */
}

static void test_short_and_other_messages(void)
{
	struct hbas_buttons b;

	hbas_buttons_init(&b, HBAS_LAYOUT_A);
	CHECK(DECODE(&b, 0xFEE1, 10, 20) == 0);                        /* touch */
	CHECK(DECODE(&b, 0x1111, 1, 2, 3) == 0);
	CHECK(hbas_buttons_decode(&b, (const uint8_t[]){ 0x70, 0x05 }, 2, ev, 16) == 0);
	/* short right-handlebar payload: missing bytes count as released */
	CHECK(DECODE(&b, 0x0570, 0, 0) == 0);
	/* length byte smaller than the data: only `length` bytes are used */
	{
		const uint8_t m[] = { 0x70, 0x05, 3, 0, 0, 0, 0x01, 0 };

		CHECK(hbas_buttons_decode(&b, m, sizeof(m), ev, 16) == 0);
	}
	CHECK(hbas_key_name('?') == NULL);
}

int main(void)
{
	test_right_handlebar_layout_a();
	test_left_handlebar_layout_a();
	test_layout_b_differs();
	test_front_controls();
	test_short_and_other_messages();
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return EXIT_FAILURE;
	}
	puts("test_buttons: all checks passed");
	return EXIT_SUCCESS;
}
