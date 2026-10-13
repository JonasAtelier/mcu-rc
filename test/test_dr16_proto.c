/*
 * Host-side check of the DBUS wire format against a frame packed by hand.
 *
 * The four sticks and two switches share the first six bytes as one bit
 * stream, least significant bit first (dr16_proto.h's table). The fixture
 * sets channel 0 to 364, channel 1 to 1024, channel 2 to 1684, channel 3
 * to 1000, switch S2 to 3 (middle) and S1 to 1 (up):
 *
 *   364  = 001 0110 1100     1024 = 100 0000 0000
 *   1684 = 110 1001 0100     1000 = 011 1110 1000
 *
 *   byte  stream bits  fields, low bit first          binary     hex
 *   ----  -----------  -----------------------------  ---------  ----
 *    0      0 ..  7    ch0[0..7]                      0110 1100  0x6C
 *    1      8 .. 15    ch0[8..10], ch1[0..4]          0000 0001  0x01
 *    2     16 .. 23    ch1[5..10], ch2[0..1]          0010 0000  0x20
 *    3     24 .. 31    ch2[2..9]                      1010 0101  0xA5
 *    4     32 .. 39    ch2[10], ch3[0..6]             1101 0001  0xD1
 *    5     40 .. 47    ch3[7..10], S2[0..1], S1[0..1] 0111 0111  0x77
 *
 * Then little-endian 16-bit words and single bytes: mouse x -5 (FB FF),
 * y 300 (2C 01), z 0, left button pressed, right not, keys W, SHIFT and B
 * (0x8011: 11 80), and the wheel at 1124, 100 past its rest (64 04).
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "dr16_proto.h"

static const uint8_t fixture[DR16_FRAME_LEN] = {
	0x6C, 0x01, 0x20, 0xA5, 0xD1, 0x77,
	0xFB, 0xFF, 0x2C, 0x01, 0x00, 0x00,
	0x01, 0x00, 0x11, 0x80, 0x64, 0x04,
};

static void test_fixture(void)
{
	uint16_t raw[DR16_CHANNELS];
	struct dr16_input input;

	assert(dr16_unpack(fixture, raw, &input));
	assert(raw[0] == 364 && raw[1] == 1024 && raw[2] == 1684 &&
	       raw[3] == 1000);
	assert(input.switch_s1 == DR16_SWITCH_UP);
	assert(input.switch_s2 == DR16_SWITCH_MIDDLE);
	assert(input.mouse_x == -5 && input.mouse_y == 300 &&
	       input.mouse_z == 0);
	assert(input.mouse_left && !input.mouse_right);
	assert(input.keys == (DR16_KEY_W | DR16_KEY_SHIFT | DR16_KEY_B));
	assert(input.wheel == 100);
}

/* Swapping the switches' bits swaps S1 and S2: the right switch is the
 * high pair. */
static void test_switch_order(void)
{
	uint8_t frame[DR16_FRAME_LEN];
	uint16_t raw[DR16_CHANNELS];
	struct dr16_input input;

	memcpy(frame, fixture, sizeof(frame));
	frame[5] = (uint8_t)((frame[5] & 0x0F) | (DR16_SWITCH_DOWN << 6) |
			     (DR16_SWITCH_UP << 4));
	assert(dr16_unpack(frame, raw, &input));
	assert(input.switch_s1 == DR16_SWITCH_DOWN &&
	       input.switch_s2 == DR16_SWITCH_UP);
}

/* A stick outside 364..1684, or a switch at 0, is not a frame, and nothing
 * is written. */
static void test_out_of_range(void)
{
	uint8_t frame[DR16_FRAME_LEN];
	uint16_t raw[DR16_CHANNELS] = { 7, 7, 7, 7 };
	struct dr16_input input = { .switch_s1 = 9 };

	/* Channel 0 at 363: byte 0 one lower. */
	memcpy(frame, fixture, sizeof(frame));
	frame[0] = 0x6B;
	assert(!dr16_unpack(frame, raw, &input));
	assert(raw[0] == 7 && input.switch_s1 == 9);

	/* Channel 2 at 1685: its low two bits sit in byte 2's top. */
	memcpy(frame, fixture, sizeof(frame));
	frame[2] = 0x60;
	assert(!dr16_unpack(frame, raw, &input));

	/* The ends themselves are fine: 364 and 1684 are in the fixture. */

	/* S1 at 0. */
	memcpy(frame, fixture, sizeof(frame));
	frame[5] &= 0x3F;
	assert(!dr16_unpack(frame, raw, &input));

	/* S2 at 0. */
	memcpy(frame, fixture, sizeof(frame));
	frame[5] &= 0xCF;
	assert(!dr16_unpack(frame, raw, &input));
	assert(raw[0] == 7);
}

int main(void)
{
	test_fixture();
	test_switch_order();
	test_out_of_range();
	printf("test_dr16_proto: all passed\n");
	return 0;
}
