/*
 * Host-side check of the SBUS driver against a fake receiver, on the
 * ESP-IDF stubs: the line it sets up, framing and resync, the flags, and
 * the idle mark, read through a fake UART and fed.
 *
 * The channel payloads are the two groups packed by hand in
 * test_crsf_proto.c; test_sbus_proto.c shows SBUS packs its channels the
 * same way.
 */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "rc.h"
#include "sbus.h"
#include "sbus_proto.h"

#define UART_PORT	1
#define RX_GPIO		16

static const uint8_t group_alternating[11] = {
	0xFF, 0x07, 0xC0, 0xFF, 0x01, 0xF0, 0x7F, 0x00, 0xFC, 0x1F, 0x00
};
static const uint8_t group_centred[11] = {
	0xE0, 0x03, 0x1F, 0xF8, 0xC0, 0x07, 0x3E, 0xF0, 0x81, 0x0F, 0x7C
};

/* ------------------------------------------------------ the fake UART -- */

static uint8_t stream[4096];
static size_t stream_len, stream_pos;
static int installed;
static uart_config_t param;
static int inverse_calls;
static uint32_t inverse_mask;

esp_err_t uart_driver_install(uart_port_t port, int rx_size, int tx_size,
			      int queue_size, QueueHandle_t *queue, int flags)
{
	(void)rx_size;
	(void)queue_size;
	(void)queue;
	(void)flags;
	assert(port == UART_PORT && tx_size == 0);
	installed++;
	return ESP_OK;
}

esp_err_t uart_driver_delete(uart_port_t port)
{
	assert(port == UART_PORT);
	installed--;
	return ESP_OK;
}

bool uart_is_driver_installed(uart_port_t port)
{
	return port == UART_PORT && installed > 0;
}

esp_err_t uart_param_config(uart_port_t port, const uart_config_t *config)
{
	assert(port == UART_PORT);
	param = *config;
	return ESP_OK;
}

esp_err_t uart_set_pin(uart_port_t port, int tx, int rx, int rts, int cts)
{
	(void)rts;
	(void)cts;
	assert(port == UART_PORT && rx == RX_GPIO && tx == UART_PIN_NO_CHANGE);
	return ESP_OK;
}

esp_err_t uart_set_line_inverse(uart_port_t port, uint32_t mask)
{
	assert(port == UART_PORT);
	inverse_calls++;
	inverse_mask = mask;
	return ESP_OK;
}

esp_err_t uart_get_buffered_data_len(uart_port_t port, size_t *size)
{
	assert(port == UART_PORT);
	*size = stream_len - stream_pos;
	return ESP_OK;
}

int uart_read_bytes(uart_port_t port, void *buf, uint32_t length,
		    TickType_t ticks_to_wait)
{
	size_t avail = stream_len - stream_pos;
	size_t n = avail < (size_t)length ? avail : (size_t)length;

	assert(port == UART_PORT && ticks_to_wait == 0);
	memcpy(buf, stream + stream_pos, n);
	stream_pos += n;
	return (int)n;
}

/* -------------------------------------------------- the fake receiver -- */

static void reset(void)
{
	stream_len = stream_pos = 0;
}

static size_t push_frame(const uint8_t group[11], uint8_t flags,
			 uint8_t footer)
{
	size_t at = stream_len;
	uint8_t *f = stream + at;

	f[0] = SBUS_HEADER;
	memcpy(f + 1, group, 11);
	memcpy(f + 12, group, 11);
	f[23] = flags;
	f[24] = footer;
	stream_len += SBUS_FRAME_LEN;
	return at;
}

static void push_bytes(const uint8_t *bytes, size_t len)
{
	memcpy(stream + stream_len, bytes, len);
	stream_len += len;
}

static struct sbus *open_read(void)
{
	struct sbus_config config = {
		.port = { .uart = UART_PORT, .rx_gpio = RX_GPIO },
	};
	struct sbus *sbus = NULL;

	assert(sbus_init(&config, &sbus) == 0);
	assert(sbus && sbus_rc(sbus));
	return sbus;
}

static void assert_raw(const struct rc_dev *rc, bool centred)
{
	uint16_t raw[SBUS_CHANNELS];

	assert(rc_get_raw(rc, raw, SBUS_CHANNELS) == 0);
	for (int i = 0; i < SBUS_CHANNELS; i++)
		assert(raw[i] == (centred ? 992 : (i % 2 == 0 ? 2047 : 0)));
}

static uint32_t errors(struct sbus *sbus)
{
	struct rc_link link;

	assert(rc_get_link(sbus_rc(sbus), 0, &link) == 0);
	return link.errors;
}

/* ------------------------------------------------------------- tests -- */

/* 100000 baud, 8E2, inverted unless told otherwise. */
static void test_line(void)
{
	struct sbus_config config = {
		.port = { .uart = UART_PORT, .rx_gpio = RX_GPIO },
	};
	struct sbus *sbus = NULL;

	inverse_calls = 0;
	assert(sbus_init(&config, &sbus) == 0);
	assert(param.baud_rate == 100000 && param.data_bits == UART_DATA_8_BITS &&
	       param.parity == UART_PARITY_EVEN &&
	       param.stop_bits == UART_STOP_BITS_2);
	assert(inverse_calls == 1 && inverse_mask == UART_SIGNAL_RXD_INV);
	assert(sbus_del(sbus) == 0 && installed == 0);

	inverse_calls = 0;
	config.not_inverted = true;
	assert(sbus_init(&config, &sbus) == 0);
	assert(inverse_calls == 0);
	assert(sbus_del(sbus) == 0 && installed == 0);

	assert(sbus_init(NULL, &sbus) == -EINVAL);
	assert(sbus_init(&config, NULL) == -EINVAL);
	assert(sbus_del(NULL) == -EINVAL);
	assert(sbus_rc(NULL) == NULL);
}

static void test_frames_and_flags(void)
{
	struct sbus_flags flags;
	struct rc_link link;
	struct sbus *sbus;
	struct rc_dev *rc;

	reset();
	sbus = open_read();
	rc = sbus_rc(sbus);
	assert(sbus_get_flags(sbus, &flags) == -ENODATA);

	/* A plain frame. */
	push_frame(group_alternating, 0x00, 0x00);
	assert(rc_update(rc, 100) == 0);
	assert_raw(rc, false);
	assert(sbus_get_flags(sbus, &flags) == 0);
	assert(!flags.channel_17 && !flags.channel_18 && !flags.frame_lost &&
	       !flags.failsafe);
	assert(rc_get_link(rc, 100, &link) == 0 && link.state == RC_LINK_UP);

	/* The digital channels, and a lost frame: still up. */
	push_frame(group_centred, SBUS_FLAG_CHANNEL_17 | SBUS_FLAG_FRAME_LOST,
		   0x00);
	assert(rc_update(rc, 110) == 0);
	assert_raw(rc, true);
	assert(sbus_get_flags(sbus, &flags) == 0);
	assert(flags.channel_17 && !flags.channel_18 && flags.frame_lost &&
	       !flags.failsafe);
	assert(rc_get_link(rc, 110, &link) == 0 && link.state == RC_LINK_UP);

	/* The receiver's failsafe. */
	push_frame(group_centred, SBUS_FLAG_FAILSAFE | SBUS_FLAG_CHANNEL_18,
		   0x00);
	assert(rc_update(rc, 120) == 0);
	assert(sbus_get_flags(sbus, &flags) == 0);
	assert(flags.failsafe && flags.channel_18);
	assert(rc_get_link(rc, 120, &link) == 0 &&
	       link.state == RC_LINK_FAILSAFE);

	/* And back. */
	push_frame(group_centred, 0x00, 0x00);
	assert(rc_update(rc, 130) == 0);
	assert(rc_get_link(rc, 130, &link) == 0 && link.state == RC_LINK_UP);
	assert(link.errors == 0 && link.frames == 4);

	assert(sbus_get_flags(NULL, &flags) == -EINVAL);
	assert(sbus_get_flags(sbus, NULL) == -EINVAL);
	assert(sbus_del(sbus) == 0);
}

/*
 * The footers the sources name ([BFS] sbus.h:83-84, sbus.cpp:110): 0x00, and
 * the SBUS2 ones with a low nibble of 4. Another footer is not a frame --
 * this is where the library is stricter than Betaflight and ArduPilot,
 * which do not look at the byte.
 */
static void test_footers(void)
{
	static const uint8_t sbus2[] = { 0x04, 0x14, 0x24, 0x34 };
	struct sbus *sbus;
	struct rc_dev *rc;

	reset();
	sbus = open_read();
	rc = sbus_rc(sbus);
	for (size_t i = 0; i < sizeof(sbus2); i++) {
		push_frame(group_centred, 0x00, sbus2[i]);
		assert(rc_update(rc, 0) == 0);
	}
	push_frame(group_alternating, 0x00, 0xFF);
	assert(rc_update(rc, 0) == -EAGAIN);
	assert_raw(rc, true);
	assert(errors(sbus) >= 1);
	assert(sbus_del(sbus) == 0);
}

/*
 * Picked up mid-frame, with a 0x0F in the junk. A header counts only after
 * a footer, as Bolder Flight has it: the first false one follows 0x3E and
 * is passed over without a word; the second follows a 0x00, so it is tried,
 * and its "footer" 24 bytes on is the real frame's channel data: counted,
 * and the real frame behind it still decodes. Stepping by a whole frame on
 * the failure would skip it.
 */
static void test_resync(void)
{
	static const uint8_t quiet_junk[] = { 0x3E, 0x0F, 0x81, 0x7C, 0x11,
					      0x00 };
	static const uint8_t loud_junk[] = { 0x00, 0x0F, 0x22, 0x00 };
	struct sbus *sbus;
	struct rc_dev *rc;

	reset();
	push_bytes(quiet_junk, sizeof(quiet_junk));
	push_frame(group_alternating, 0x00, 0x00);
	push_frame(group_centred, 0x00, 0x00);
	sbus = open_read();
	rc = sbus_rc(sbus);
	assert(rc_update(rc, 0) == 0);
	assert(rc->frames == 2 && errors(sbus) == 0);
	assert_raw(rc, true);		/* the newest */
	assert(rc_update(rc, 1) == -EAGAIN);
	assert(sbus_del(sbus) == 0);

	reset();
	push_bytes(loud_junk, sizeof(loud_junk));
	push_frame(group_alternating, 0x00, 0x00);
	push_frame(group_centred, 0x00, 0x00);
	sbus = open_read();
	rc = sbus_rc(sbus);
	assert(rc_update(rc, 0) == 0);
	assert(rc->frames == 2 && "both real frames survive");
	assert(errors(sbus) == 1 && "the false header is counted");
	assert_raw(rc, true);
	assert(sbus_del(sbus) == 0);

	/* A real-looking header straight after channel data is not taken. */
	reset();
	push_bytes((const uint8_t *)"\x55", 1);
	push_frame(group_alternating, 0x00, 0x00);
	sbus = open_read();
	rc = sbus_rc(sbus);
	assert(rc_update(rc, 0) == -EAGAIN);
	assert(rc->frames == 0);
	/* ... and the next one, after that frame's footer, is. */
	push_frame(group_centred, 0x00, 0x00);
	assert(rc_update(rc, 1) == 0);
	assert_raw(rc, true);
	assert(sbus_del(sbus) == 0);
}

static void test_split_across_updates(void)
{
	struct sbus *sbus;
	struct rc_dev *rc;

	reset();
	push_frame(group_centred, 0x00, 0x00);
	stream_len = 10;
	sbus = open_read();
	rc = sbus_rc(sbus);
	assert(rc_update(rc, 0) == -EAGAIN);
	stream_len = SBUS_FRAME_LEN;
	assert(rc_update(rc, 1) == 0);
	assert_raw(rc, true);
	assert(errors(sbus) == 0);
	assert(sbus_del(sbus) == 0);
}

/* Normalised on Betaflight's fit: 173, 993 and 1812 are -1, 0 and 1.
 * ArduPilot's 200..1800 end points come out a few percent inside. */
static void test_normalised(void)
{
	struct sbus *sbus;
	struct rc_dev *rc;
	float channels[SBUS_CHANNELS];

	reset();
	push_frame(group_centred, 0x00, 0x00);
	sbus = open_read();
	rc = sbus_rc(sbus);
	assert(rc_update(rc, 0) == 0);
	rc->raw[0] = 173;
	rc->raw[1] = 993;
	rc->raw[2] = 1812;
	rc->raw[3] = 200;
	rc->raw[4] = 1800;
	assert(rc_get_channels(rc, channels, SBUS_CHANNELS) == 0);
	assert(channels[0] == -1.0f && channels[1] == 0.0f &&
	       channels[2] == 1.0f);
	assert(channels[3] == (200.0f - 993.0f) / 820.0f);
	assert(channels[4] == (1800.0f - 993.0f) / 819.0f);
	assert(channels[3] > -0.97f && channels[4] < 0.986f);
	assert(sbus_del(sbus) == 0);
}

/*
 * Fed, with the idle mark: a partial frame cut off by an idle line is
 * dropped and counted, and the next frame starts clean even though the
 * partial one had swallowed its first bytes' worth of room.
 */
static void test_fed_with_idle(void)
{
	struct sbus_config config = { .port = { .fed = true } };
	struct sbus *sbus = NULL;
	struct rc_dev *rc;
	uint8_t frame[SBUS_FRAME_LEN];

	assert(sbus_init(&config, &sbus) == 0);
	assert(installed == 0 && "fed never touches the UART");
	rc = sbus_rc(sbus);

	reset();
	push_frame(group_alternating, 0x00, 0x00);
	memcpy(frame, stream, SBUS_FRAME_LEN);

	/* Twelve bytes of a frame, then the line goes quiet. */
	assert(rc_rx_feed(rc, frame, 12) == 12);
	rc_rx_idle(rc);
	assert(rc_rx_feed(rc, frame, SBUS_FRAME_LEN) == SBUS_FRAME_LEN);
	rc_rx_idle(rc);
	assert(rc_update(rc, 5) == 0);
	assert_raw(rc, false);
	assert(errors(sbus) == 1);

	/* Junk, then an idle: the next byte starts a frame, whatever came
	 * before the idle. */
	assert(rc_rx_feed(rc, (const uint8_t *)"\x55", 1) == 1);
	rc_rx_idle(rc);
	assert(rc_rx_feed(rc, frame, SBUS_FRAME_LEN) == SBUS_FRAME_LEN);
	assert(rc_update(rc, 6) == 0);

	/* An idle after a whole frame costs nothing. */
	assert(rc_rx_feed(rc, frame, SBUS_FRAME_LEN) == SBUS_FRAME_LEN);
	rc_rx_idle(rc);
	assert(rc_update(rc, 6) == 0);
	assert(errors(sbus) == 1);

	assert(sbus_del(sbus) == 0);
}

int main(void)
{
	test_line();
	test_frames_and_flags();
	test_footers();
	test_resync();
	test_split_across_updates();
	test_normalised();
	test_fed_with_idle();
	printf("test_sbus: all passed\n");
	return 0;
}
