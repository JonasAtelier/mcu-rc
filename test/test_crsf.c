/*
 * Host-side check of the CRSF driver against a fake receiver, on the
 * ESP-IDF stubs: read through a fake UART, and fed. Ported from esp-crsf-rc's
 * test/test_driver.c, in its four-space style; each of its cases is here,
 * changed where the API did (marked "mcu-rc:").
 *
 * These tests are about framing, not about the wire format: what happens to a
 * stream picked up mid-frame, to a frame that fails its CRC, to a frame split
 * across two calls, and to a backlog of frames when the caller is slower than
 * the link. The channel payloads used below are the two packed by hand in
 * test_crsf_proto.c, so nothing here has to pack bits and no test-side packer
 * can quietly agree with a wrong unpacker.
 */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "crsf.h"
#include "crsf_proto.h"
#include "driver/uart.h"
#include "rc.h"

#define UART_PORT  1
#define RX_GPIO    16

/* The two hand-packed payload groups from test_crsf_proto.c, each repeated
 * to fill 22 bytes: alternating 2047/0, and every channel at 992. */
static const uint8_t group_alternating[11] = {
    0xFF, 0x07, 0xC0, 0xFF, 0x01, 0xF0, 0x7F, 0x00, 0xFC, 0x1F, 0x00
};
static const uint8_t group_centred[11] = {
    0xE0, 0x03, 0x1F, 0xF8, 0xC0, 0x07, 0x3E, 0xF0, 0x81, 0x0F, 0x7C
};

/* ------------------------------------------------------ the fake UART ---- */

static uint8_t    stream[4096];
static size_t     stream_len, stream_pos;
static int        installed;
static int        last_rx_buf_size;
static int        expect_baud = CRSF_BAUD_DEFAULT;
static esp_err_t  install_err;

esp_err_t uart_driver_install(uart_port_t port, int rx_size, int tx_size,
                              int queue_size, QueueHandle_t *queue, int flags)
{
    (void)queue_size; (void)queue; (void)flags;
    assert(port == UART_PORT);
    assert(tx_size == 0 && "the driver never transmits");
    if (install_err != ESP_OK)
        return install_err;
    last_rx_buf_size = rx_size;
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

esp_err_t uart_param_config(uart_port_t port, const uart_config_t *c)
{
    (void)port;
    assert(c->baud_rate == expect_baud);
    assert(c->data_bits == UART_DATA_8_BITS);
    assert(c->parity == UART_PARITY_DISABLE);
    assert(c->stop_bits == UART_STOP_BITS_1);
    return ESP_OK;
}

esp_err_t uart_set_pin(uart_port_t port, int tx, int rx, int rts, int cts)
{
    (void)port; (void)rts; (void)cts;
    assert(rx == RX_GPIO);
    assert(tx == UART_PIN_NO_CHANGE && "no telemetry, so no TX wire");
    return ESP_OK;
}

/* mcu-rc: CRSF is not inverted ([TBS] crsf.md:138), so the stub's
 * uart_set_line_inverse(), which aborts, must never be reached. */

/* mcu-rc: the library polls, so every read is a 0-tick one, and the fake
 * never has to charge a wait. */
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

/* -------------------------------------------------- the fake receiver ---- */

/* Append a well-formed frame and return where its first byte landed, so a
 * test can corrupt it afterwards. */
static size_t push_frame(uint8_t type, const uint8_t *payload, size_t n)
{
    size_t at = stream_len;
    uint8_t *f = stream + at;

    f[0] = CRSF_ADDR_FC;
    f[1] = (uint8_t)(n + 2);
    f[2] = type;
    memcpy(f + 3, payload, n);
    f[3 + n] = crsf_crc8(f + 2, n + 1);
    stream_len += n + 4;

    return at;
}

static size_t push_channels(const uint8_t group[11])
{
    uint8_t payload[CRSF_RC_PAYLOAD];

    memcpy(payload, group, 11);
    memcpy(payload + 11, group, 11);

    return push_frame(CRSF_TYPE_RC_CHANNELS, payload, sizeof payload);
}

static size_t push_link(uint8_t rssi1, uint8_t rssi2, uint8_t lq, int8_t snr)
{
    uint8_t payload[CRSF_LINK_PAYLOAD] = {
        rssi1, rssi2, lq, (uint8_t)snr, 0, 2, 3, 40, 99, 7
    };

    return push_frame(CRSF_TYPE_LINK_STATS, payload, sizeof payload);
}

static void reset(void)
{
    stream_len = stream_pos = 0;
}

static struct crsf *open_ok(void)
{
    struct crsf_config cfg = {
        .port = { .uart = UART_PORT, .rx_gpio = RX_GPIO },
    };
    struct crsf *d = NULL;

    assert(crsf_init(&cfg, &d) == 0);
    assert(d != NULL && crsf_rc(d) != NULL);
    return d;
}

static void assert_alternating(const struct rc_dev *rc)
{
    uint16_t raw[CRSF_CHANNELS];

    assert(rc_get_raw(rc, raw, CRSF_CHANNELS) == 0);
    for (int i = 0; i < CRSF_CHANNELS; i++)
        assert(raw[i] == (i % 2 == 0 ? 2047 : 0));
}

static void assert_centred(const struct rc_dev *rc)
{
    uint16_t raw[CRSF_CHANNELS];
    float channels[CRSF_CHANNELS];

    assert(rc_get_raw(rc, raw, CRSF_CHANNELS) == 0);
    assert(rc_get_channels(rc, channels, CRSF_CHANNELS) == 0);
    for (int i = 0; i < CRSF_CHANNELS; i++)
        assert(raw[i] == 992 && channels[i] == 0.0f);
}

static uint32_t errors(struct crsf *d)
{
    struct rc_link link;

    assert(rc_get_link(crsf_rc(d), 0, &link) == 0);
    return link.errors;
}

/* ------------------------------------------------------------- tests ---- */

/*
 * mcu-rc: init no longer waits for a frame. A receiver that is unpowered,
 * miswired or at the wrong baud shows as RC_LINK_NO_FRAME: no frames, no
 * errors when the line is silent, many errors when the baud is wrong.
 */
static void test_init_returns_at_once_and_silence_is_no_frame(void)
{
    struct crsf_config cfg = {
        .port = { .uart = UART_PORT, .rx_gpio = RX_GPIO },
    };
    struct crsf *d = NULL;
    struct rc_link link;

    reset();
    d = open_ok();
    assert(installed == 1);
    assert(rc_update(crsf_rc(d), 0) == -EAGAIN);
    assert(rc_get_link(crsf_rc(d), 5000, &link) == 0);
    assert(link.state == RC_LINK_NO_FRAME && link.age_ms == UINT32_MAX);
    assert(link.frames == 0 && link.errors == 0);
    assert(crsf_del(d) == 0);
    assert(installed == 0 && "del must give the port back");

    /* A failed install leaves nothing behind. */
    install_err = ESP_ERR_NO_MEM;
    d = NULL;
    assert(crsf_init(&cfg, &d) == -ENOMEM && d == NULL);
    assert(installed == 0);
    install_err = ESP_OK;

    assert(crsf_init(NULL, &d) == -EINVAL);
    assert(crsf_init(&cfg, NULL) == -EINVAL);
}

/* A link statistics frame arrives with no radio bound; it is kept, and it
 * is not a channel frame. */
static void test_a_link_frame_alone(void)
{
    struct crsf_link_stats link;
    struct crsf *d;

    reset();
    push_link(85, 61, 100, 9);

    d = open_ok();
    assert(crsf_get_link_stats(d, &link) == -ENODATA);
    assert(rc_update(crsf_rc(d), 0) == -EAGAIN && "no channels yet");
    assert(crsf_get_link_stats(d, &link) == 0);
    assert(link.link_quality == 100);
    assert(crsf_del(d) == 0);
    assert(installed == 0);
}

static void test_rx_buffer_size_is_configurable(void)
{
    struct crsf_config cfg = {
        .port = { .uart = UART_PORT, .rx_gpio = RX_GPIO },
    };
    struct crsf *d = NULL;

    reset();
    assert(crsf_init(&cfg, &d) == 0);
    assert(last_rx_buf_size == 2048 && "zero means the documented default");
    assert(crsf_del(d) == 0);

    cfg.port.rx_buffer_len = 8192;
    assert(crsf_init(&cfg, &d) == 0);
    assert(last_rx_buf_size == 8192);
    assert(crsf_del(d) == 0);
}

static void test_the_baud_rate_is_configurable(void)
{
    struct crsf_config cfg = {
        .port = { .uart = UART_PORT, .rx_gpio = RX_GPIO },
    };
    struct crsf *d = NULL;

    /* Zero means the documented default, which is what ExpressLRS ships. */
    expect_baud = CRSF_BAUD_DEFAULT;
    assert(crsf_init(&cfg, &d) == 0);
    assert(crsf_del(d) == 0);

    /* The TBS specification's dual-wire rate. */
    cfg.baud = 416666;
    expect_baud = 416666;
    assert(crsf_init(&cfg, &d) == 0);
    assert(crsf_del(d) == 0);

    expect_baud = CRSF_BAUD_DEFAULT;
}

/* Any of the accepted addresses opens a frame. EdgeTX puts the transmitter's
 * address on the wire, and a strict test against 0xC8 would drop it. */
static void test_every_accepted_address_is_read(void)
{
    static const uint8_t addrs[3] = {
        CRSF_ADDR_FC, CRSF_ADDR_BROADCAST, CRSF_ADDR_TRANSMITTER
    };

    for (size_t i = 0; i < sizeof addrs / sizeof addrs[0]; i++) {
        struct crsf *d;
        size_t at;

        reset();
        at = push_channels(group_centred);

        /* The address sits outside the CRC, so overwriting it after the fact
         * leaves the frame well formed. */
        stream[at] = addrs[i];

        d = open_ok();
        assert(rc_update(crsf_rc(d), 0) == 0);
        assert_centred(crsf_rc(d));
        assert(crsf_del(d) == 0);
    }
}

/*
 * ExpressLRS 4.0 may append an arm-status byte, making the channel payload
 * 23 rather than 22. Rejecting that frame would silently drop everything a
 * current receiver sends. mcu-rc: and any longer payload, as the
 * specification asks of every frame (crsf.md:177); esp-crsf-rc took 22 and
 * 23 only. Both cases are here.
 */
static void test_longer_channel_frames_are_accepted(void)
{
    uint8_t payload[CRSF_RC_PAYLOAD + 4];
    struct crsf *d;

    memcpy(payload, group_centred, 11);
    memcpy(payload + 11, group_centred, 11);
    payload[22] = 0x01;                 /* the arm-status byte */
    payload[23] = 0x5A;
    payload[24] = 0xA5;
    payload[25] = 0xFF;

    reset();
    push_channels(group_alternating);
    push_frame(CRSF_TYPE_RC_CHANNELS, payload, CRSF_RC_PAYLOAD_ARMED);
    d = open_ok();
    assert(rc_update(crsf_rc(d), 7) == 0);
    assert_centred(crsf_rc(d));         /* the extra byte changed nothing */
    assert(errors(d) == 0);
    assert(crsf_del(d) == 0);

    reset();
    push_frame(CRSF_TYPE_RC_CHANNELS, payload, sizeof payload);
    d = open_ok();
    assert(rc_update(crsf_rc(d), 7) == 0);
    assert_centred(crsf_rc(d));
    assert(crsf_del(d) == 0);
}

/*
 * The stream starts mid-frame, and the junk before the first real frame
 * includes a false sync byte followed by a plausible length. That candidate
 * swallows part of the real frame behind it and then fails its CRC, which is
 * exactly the case that decides whether resynchronisation may step by the
 * claimed length: it may not. Stepping by 26 here would skip the real frame
 * entirely.
 */
static void test_resyncs_from_a_stream_picked_up_mid_frame(void)
{
    static const uint8_t junk[] = { 0x00, 0xC8, 0x18, 0x16, 0xFF, 0xFF };
    struct crsf *d;

    reset();
    memcpy(stream, junk, sizeof junk);
    stream_len = sizeof junk;
    push_channels(group_alternating);
    push_channels(group_centred);

    d = open_ok();
    assert(rc_update(crsf_rc(d), 0) == 0);
    assert(errors(d) >= 1 && "the false candidate must be counted");
    assert(crsf_rc(d)->frames == 2 && "both real frames survive");
    assert_centred(crsf_rc(d));

    /* Nothing left: no frame, and the last one stays. */
    assert(rc_update(crsf_rc(d), 1) == -EAGAIN);
    assert_centred(crsf_rc(d));

    assert(crsf_del(d) == 0);
    assert(installed == 0 && "del must release the port it installed");
}

static void test_a_corrupt_frame_is_skipped_and_counted(void)
{
    struct crsf *d;
    size_t bad;

    reset();
    push_channels(group_centred);
    bad = push_channels(group_alternating);
    stream[bad + 5] ^= 0x20;            /* a payload byte flipped in flight */
    push_channels(group_centred);

    d = open_ok();

    /* The corrupt frame is dropped rather than delivered. Delivering it would
     * put a wrong stick position into a control loop, which is the one thing
     * a CRC is here to prevent. */
    assert(rc_update(crsf_rc(d), 0) == 0);
    assert_centred(crsf_rc(d));
    assert(crsf_rc(d)->frames == 2);
    assert(errors(d) >= 1);

    assert(crsf_del(d) == 0);
}

/* A frame of the right type but too short is not a channel frame, whatever
 * its CRC says, and must not be unpacked. mcu-rc: it is counted in errors,
 * which now counts every frame thrown away, not only CRC failures. */
static void test_a_short_channel_frame_is_not_channels(void)
{
    uint8_t stub[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    struct rc_link link;
    struct crsf *d;

    reset();
    push_link(85, 61, 100, 9);
    push_frame(CRSF_TYPE_RC_CHANNELS, stub, sizeof stub);

    d = open_ok();
    assert(rc_update(crsf_rc(d), 0) == -EAGAIN);
    assert(rc_get_link(crsf_rc(d), 0, &link) == 0);
    assert(link.state == RC_LINK_NO_FRAME && "no channel frame has ever arrived");
    assert(link.errors == 1);

    /* A short link statistics frame likewise. */
    reset();
    push_frame(CRSF_TYPE_LINK_STATS, stub, 4);
    assert(rc_update(crsf_rc(d), 0) == -EAGAIN);
    assert(errors(d) == 2);

    assert(crsf_del(d) == 0);
}

/*
 * A caller slower than the link finds several frames waiting. It must be
 * handed the newest, not the oldest: acting on stick positions from several
 * frames ago is a lag that grows for as long as the loop runs behind.
 */
static void test_update_takes_the_newest_queued_frame(void)
{
    struct crsf *d;

    reset();
    push_link(85, 61, 100, 9);
    push_channels(group_alternating);
    push_channels(group_alternating);
    push_channels(group_centred);       /* the newest */

    d = open_ok();
    assert(rc_update(crsf_rc(d), 0) == 0);
    assert_centred(crsf_rc(d));
    assert(crsf_rc(d)->frames == 3);

    /* All three were consumed, so there is nothing behind it. */
    assert(rc_update(crsf_rc(d), 1) == -EAGAIN);

    assert(crsf_del(d) == 0);
}

/* Telemetry, parameter and MSP frames share the wire. They are framed and
 * checked like any other, then dropped, and they must not stall a read or
 * count as corruption. */
static void test_unknown_frame_types_are_stepped_over(void)
{
    uint8_t gps[15] = { 0 };
    struct crsf_link_stats link;
    struct crsf *d;

    reset();
    push_frame(0x02, gps, sizeof gps);  /* GPS, or whatever it is */
    push_link(85, 61, 100, 9);
    push_frame(0x28, gps, 4);           /* a device ping */
    push_channels(group_centred);

    d = open_ok();
    assert(crsf_get_link_stats(d, &link) == -ENODATA);

    assert(rc_update(crsf_rc(d), 0) == 0);
    assert_centred(crsf_rc(d));
    assert(errors(d) == 0);

    /* The link frame went past in the same update, and was kept. */
    assert(crsf_get_link_stats(d, &link) == 0);
    assert(link.rssi1 == 85);
    assert(link.rssi2 == 61);
    assert(link.antenna == 0);
    assert(link.link_quality == 100);
    assert(link.snr_db == 9);

    /* mcu-rc: a longer link frame, as ExpressLRS 4.1 declares one. */
    {
        uint8_t longer[CRSF_LINK_PAYLOAD + 1] = { 70, 72, 55, 0xFB, 1, 2, 3,
                                                  40, 99, 7, 66 };

        reset();
        push_frame(CRSF_TYPE_LINK_STATS, longer, sizeof longer);
        assert(rc_update(crsf_rc(d), 0) == -EAGAIN);
        assert(crsf_get_link_stats(d, &link) == 0);
        assert(link.link_quality == 55 && link.snr_db == -5 &&
               link.antenna == 1);
    }

    assert(crsf_get_link_stats(NULL, &link) == -EINVAL);
    assert(crsf_get_link_stats(d, NULL) == -EINVAL);
    assert(crsf_del(d) == 0);
}

/* A frame that arrives in two pieces must be held, not thrown away. */
static void test_a_frame_split_across_two_updates_is_assembled(void)
{
    size_t at, whole;
    struct crsf *d;

    reset();
    at = stream_len;
    push_channels(group_centred);
    whole = stream_len - at;

    /* Hide all but the first nine bytes of the frame. */
    stream_len = at + 9;

    d = open_ok();
    assert(rc_update(crsf_rc(d), 0) == -EAGAIN);

    stream_len = at + whole;            /* the rest turns up */
    assert(rc_update(crsf_rc(d), 1) == 0);
    assert_centred(crsf_rc(d));

    assert(crsf_del(d) == 0);
}

/* mcu-rc: fed, the same stream a byte at a time, the mid-frame junk first:
 * the framing does not depend on how the bytes are cut. */
static void test_fed_a_byte_at_a_time(void)
{
    static const uint8_t junk[] = { 0x00, 0xC8, 0x18, 0x16, 0xFF, 0xFF };
    struct crsf_config cfg = { .port = { .fed = true } };
    struct crsf *d = NULL;
    struct rc_dev *rc;
    int frames_seen = 0;

    reset();
    memcpy(stream, junk, sizeof junk);
    stream_len = sizeof junk;
    push_channels(group_alternating);
    push_link(85, 61, 100, 9);
    push_channels(group_centred);

    assert(crsf_init(&cfg, &d) == 0);
    assert(installed == 0 && "fed never touches the UART");
    rc = crsf_rc(d);
    for (size_t i = 0; i < stream_len; i++) {
        assert(rc_rx_feed(rc, &stream[i], 1) == 1);
        if (rc_update(rc, (uint32_t)i) == 0) {
            frames_seen++;
            if (frames_seen == 1)
                assert_alternating(rc);
        }
    }
    assert(frames_seen == 2);
    assert_centred(rc);
    assert(crsf_del(d) == 0);
}

/* The failsafe input. Age is what a caller gates on, so it must climb when
 * the link stops, whether or not anyone is still calling rc_update.
 * mcu-rc: CRSF has no failsafe flag, so a stopped link is a timeout. */
static void test_age_climbs_when_the_link_goes_quiet(void)
{
    struct crsf_config cfg = {
        .port = { .uart = UART_PORT, .rx_gpio = RX_GPIO },
        .timeout_ms = 200,
    };
    struct rc_link link;
    struct crsf *d;

    reset();
    push_link(85, 61, 100, 9);
    push_channels(group_alternating);

    assert(crsf_init(&cfg, &d) == 0);
    assert(rc_get_link(crsf_rc(d), 0, &link) == 0);
    assert(link.age_ms == UINT32_MAX && "nothing has arrived yet");

    assert(rc_update(crsf_rc(d), 1000) == 0);
    assert_alternating(crsf_rc(d));
    assert(rc_get_link(crsf_rc(d), 1000, &link) == 0);
    assert(link.age_ms == 0 && link.state == RC_LINK_UP);

    /* The radio walks out of range. */
    assert(rc_update(crsf_rc(d), 1050) == -EAGAIN);
    assert(rc_get_link(crsf_rc(d), 1050, &link) == 0);
    assert(link.age_ms == 50 && link.state == RC_LINK_UP);
    assert(rc_get_link(crsf_rc(d), 1201, &link) == 0);
    assert(link.age_ms == 201 && link.state == RC_LINK_TIMEOUT &&
           "and it keeps climbing");

    assert(crsf_del(d) == 0);
}

static void test_null_arguments(void)
{
    assert(crsf_rc(NULL) == NULL);
    assert(crsf_del(NULL) == -EINVAL);
}

int main(void)
{
    test_init_returns_at_once_and_silence_is_no_frame();
    test_a_link_frame_alone();
    test_rx_buffer_size_is_configurable();
    test_the_baud_rate_is_configurable();
    test_every_accepted_address_is_read();
    test_longer_channel_frames_are_accepted();
    test_resyncs_from_a_stream_picked_up_mid_frame();
    test_a_corrupt_frame_is_skipped_and_counted();
    test_a_short_channel_frame_is_not_channels();
    test_update_takes_the_newest_queued_frame();
    test_unknown_frame_types_are_stepped_over();
    test_a_frame_split_across_two_updates_is_assembled();
    test_fed_a_byte_at_a_time();
    test_age_climbs_when_the_link_goes_quiet();
    test_null_arguments();
    printf("test_crsf: all passed\n");
    return 0;
}
