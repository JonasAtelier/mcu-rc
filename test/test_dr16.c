/*
 * Host-side check of the DR16 driver on the ESP-IDF stubs: fed only, framed
 * by the idle mark. The frame is test_dr16_proto.c's, packed by hand.
 */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "dr16.h"
#include "dr16_proto.h"
#include "rc.h"

static const uint8_t fixture[DR16_FRAME_LEN] = {
	0x6C, 0x01, 0x20, 0xA5, 0xD1, 0x77,
	0xFB, 0xFF, 0x2C, 0x01, 0x00, 0x00,
	0x01, 0x00, 0x11, 0x80, 0x64, 0x04,
};

static struct dr16 *open_fed(void)
{
	struct dr16_config config = { .port = { .fed = true } };
	struct dr16 *dr16 = NULL;

	assert(dr16_init(&config, &dr16) == 0);
	assert(dr16 && dr16_rc(dr16));
	return dr16;
}

static void send_frame(struct rc_dev *rc, const uint8_t *frame, size_t len)
{
	assert(rc_rx_feed(rc, frame, len) == len);
	rc_rx_idle(rc);
}

static struct rc_link link_of(struct rc_dev *rc, uint32_t now_ms)
{
	struct rc_link link;

	assert(rc_get_link(rc, now_ms, &link) == 0);
	return link;
}

/* The read path does not exist for the DR16: no idle line to frame by. */
static void test_fed_only(void)
{
	struct dr16_config config = { .port = { .fed = false } };
	struct dr16 *dr16 = NULL;

	assert(dr16_init(&config, &dr16) == -EOPNOTSUPP && !dr16);
	assert(dr16_init(NULL, &dr16) == -EINVAL);
	config.port.fed = true;
	assert(dr16_init(&config, NULL) == -EINVAL);
	assert(dr16_del(NULL) == -EINVAL);
	assert(dr16_rc(NULL) == NULL);
}

static void test_a_frame(void)
{
	struct dr16 *dr16 = open_fed();
	struct rc_dev *rc = dr16_rc(dr16);
	struct dr16_input input;
	float sticks[DR16_CHANNELS];
	uint16_t raw[DR16_CHANNELS];

	assert(dr16_get_input(dr16, &input) == -ENODATA);
	assert(link_of(rc, 0).state == RC_LINK_NO_FRAME);

	/* Bytes alone are not a frame: the idle says where it ends. */
	assert(rc_rx_feed(rc, fixture, sizeof(fixture)) == sizeof(fixture));
	assert(rc_update(rc, 10) == -EAGAIN);
	rc_rx_idle(rc);
	assert(rc_update(rc, 20) == 0);

	assert(rc_get_raw(rc, raw, DR16_CHANNELS) == 0);
	assert(raw[0] == 364 && raw[1] == 1024 && raw[2] == 1684 &&
	       raw[3] == 1000);
	/* -1, 0, 1 at 364, 1024, 1684: 660 each way. */
	assert(rc_get_channels(rc, sticks, DR16_CHANNELS) == 0);
	assert(sticks[0] == -1.0f && sticks[1] == 0.0f && sticks[2] == 1.0f);
	assert(sticks[3] == -24.0f / 660.0f);
	assert(rc_get_channels(rc, sticks, DR16_CHANNELS + 1) == -EINVAL);

	assert(dr16_get_input(dr16, &input) == 0);
	assert(input.switch_s1 == DR16_SWITCH_UP &&
	       input.switch_s2 == DR16_SWITCH_MIDDLE);
	assert(input.mouse_x == -5 && input.mouse_y == 300 && input.mouse_left);
	assert(input.keys == (DR16_KEY_W | DR16_KEY_SHIFT | DR16_KEY_B));
	assert(input.wheel == 100);

	/* No failsafe flag in DBUS: up, then a timeout when frames stop. */
	assert(link_of(rc, 20).state == RC_LINK_UP);
	assert(link_of(rc, 121).state == RC_LINK_TIMEOUT);

	assert(dr16_get_input(NULL, &input) == -EINVAL);
	assert(dr16_get_input(dr16, NULL) == -EINVAL);
	assert(dr16_del(dr16) == 0);
}

/*
 * A burst that is not 18 bytes is not a frame: the first one after start
 * is usually the tail of a frame, and a burst with two frames in it means
 * an idle was missed. Both are counted, neither is decoded, and the next
 * whole frame is.
 */
static void test_burst_lengths(void)
{
	struct dr16 *dr16 = open_fed();
	struct rc_dev *rc = dr16_rc(dr16);
	uint8_t two[2 * DR16_FRAME_LEN];

	send_frame(rc, fixture + 5, DR16_FRAME_LEN - 5);	/* a tail */
	assert(rc_update(rc, 0) == -EAGAIN);
	assert(link_of(rc, 0).errors == 1);

	memcpy(two, fixture, DR16_FRAME_LEN);
	memcpy(two + DR16_FRAME_LEN, fixture, DR16_FRAME_LEN);
	send_frame(rc, two, sizeof(two));
	assert(rc_update(rc, 0) == -EAGAIN);
	assert(link_of(rc, 0).errors == 2);

	send_frame(rc, fixture, 17);
	assert(rc_update(rc, 29) == -EAGAIN);
	send_frame(rc, fixture, DR16_FRAME_LEN);
	assert(rc_update(rc, 30) == 0);
	assert(link_of(rc, 30).errors == 3 && link_of(rc, 30).frames == 1);

	/* An idle with nothing before it is no error. */
	rc_rx_idle(rc);
	assert(rc_update(rc, 31) == -EAGAIN);
	assert(link_of(rc, 31).errors == 3);

	assert(dr16_del(dr16) == 0);
}

/* 18 bytes with a stick out of range: counted, and the last good frame
 * stays what the getters return. */
static void test_out_of_range_frame(void)
{
	struct dr16 *dr16 = open_fed();
	struct rc_dev *rc = dr16_rc(dr16);
	uint8_t bad[DR16_FRAME_LEN];
	struct dr16_input input;
	uint16_t raw[DR16_CHANNELS];

	send_frame(rc, fixture, DR16_FRAME_LEN);
	assert(rc_update(rc, 0) == 0);

	memcpy(bad, fixture, sizeof(bad));
	bad[0] = 0x6B;			/* channel 0 at 363 */
	bad[14] = 0x00;
	send_frame(rc, bad, sizeof(bad));
	assert(rc_update(rc, 1) == -EAGAIN);
	assert(link_of(rc, 1).errors == 1);
	assert(rc_get_raw(rc, raw, 1) == 0 && raw[0] == 364);
	assert(dr16_get_input(dr16, &input) == 0 &&
	       input.keys == (DR16_KEY_W | DR16_KEY_SHIFT | DR16_KEY_B));

	assert(dr16_del(dr16) == 0);
}

/*
 * A loop slower than the radio: several frames, each with its idle, between
 * two updates. All decode, and the newest is what the getters return. More
 * than RC_IDLE_MARKS: the oldest marks are lost, so the first frames run
 * together and are thrown away as one, never misread.
 */
static void test_frames_between_updates(void)
{
	struct dr16 *dr16 = open_fed();
	struct rc_dev *rc = dr16_rc(dr16);
	uint8_t frame[DR16_FRAME_LEN];
	uint16_t raw[DR16_CHANNELS];
	struct rc_link link;

	memcpy(frame, fixture, sizeof(frame));
	for (int i = 0; i < 3; i++) {
		frame[0] = (uint8_t)(0x6C + i);	/* channel 0 at 364 + i */
		send_frame(rc, frame, DR16_FRAME_LEN);
	}
	assert(rc_update(rc, 0) == 0);
	link = link_of(rc, 0);
	assert(link.frames == 3 && link.errors == 0);
	assert(rc_get_raw(rc, raw, 1) == 0 && raw[0] == 366);

	/* RC_IDLE_MARKS + 1 frames: 26 * 18 bytes fit the ring. */
	for (int i = 0; i < RC_IDLE_MARKS + 1; i++) {
		frame[0] = (uint8_t)(0x70 + i);	/* channel 0 at 368 + i */
		send_frame(rc, frame, DR16_FRAME_LEN);
	}
	assert(rc_update(rc, 1) == 0);
	link = link_of(rc, 1);
	assert(link.frames == 3 + RC_IDLE_MARKS - 1 && link.errors == 1);
	assert(rc_get_raw(rc, raw, 1) == 0 &&
	       raw[0] == 368 + RC_IDLE_MARKS);
	assert(link.dropped == 0);

	assert(dr16_del(dr16) == 0);
}

int main(void)
{
	test_fed_only();
	test_a_frame();
	test_burst_lengths();
	test_out_of_range_frame();
	test_frames_between_updates();
	printf("test_dr16: all passed\n");
	return 0;
}
