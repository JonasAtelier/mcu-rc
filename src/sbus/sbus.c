/* First: on ESP-IDF it brings in sdkconfig.h, which the #if below reads. */
#include "rc_priv.h"

#if CONFIG_RC_SBUS

#include <stdlib.h>
#include <string.h>

#include "sbus.h"
#include "sbus_proto.h"

struct sbus {
	struct rc_dev rc;		/* the category view; priv points back */
	struct rc_input input;
	uint8_t frame[SBUS_FRAME_LEN];	/* bytes held over between parses */
	size_t have;
	uint8_t before;			/* the byte before frame[0] */
	struct sbus_flags flags;
};

static void sbus_drop(struct sbus *sbus, size_t n)
{
	sbus->before = sbus->frame[n - 1];
	sbus->have -= n;
	memmove(sbus->frame, sbus->frame + n, sbus->have);
}

/*
 * Consume whole frames from what is held. 0x0F turns up inside channel
 * data too, so a header counts only right after a footer -- the end of the
 * frame before, as Bolder Flight requires (sbus.cpp:99-100), or the start
 * of the stream or an idle line -- and only with a footer 24 bytes on. A
 * header that fails either was not one: step one byte and look again.
 */
static void sbus_take_frames(struct sbus *sbus)
{
	while (sbus->have) {
		if (sbus->frame[0] != SBUS_HEADER ||
		    !sbus_footer_ok(sbus->before)) {
			sbus_drop(sbus, 1);
			continue;
		}
		if (sbus->have < SBUS_FRAME_LEN)
			return;
		if (!sbus_frame_ok(sbus->frame)) {
			rc_frame_error(&sbus->rc);
			sbus_drop(sbus, 1);
			continue;
		}
		sbus_unpack_channels(sbus->frame, sbus->rc.raw);
		sbus_unpack_flags(sbus->frame, &sbus->flags);
		rc_frame_done(&sbus->rc, sbus->flags.failsafe);
		sbus_drop(sbus, SBUS_FRAME_LEN);
	}
}

static void sbus_parse(struct rc_dev *rc, const uint8_t *data, size_t len)
{
	struct sbus *sbus = rc->priv;

	while (len) {
		size_t room = sizeof(sbus->frame) - sbus->have;
		size_t n = len < room ? len : room;

		memcpy(sbus->frame + sbus->have, data, n);
		sbus->have += n;
		data += n;
		len -= n;
		sbus_take_frames(sbus);
	}
}

/* The line went quiet: whatever is held is not the start of the next
 * frame. A frame takes about 3 ms on the wire, a byte every 120 us, with
 * no gap inside it (Betaflight sbus.c:61, :73-79). */
static void sbus_idle(struct rc_dev *rc)
{
	struct sbus *sbus = rc->priv;

	if (sbus->have)
		rc_frame_error(rc);
	sbus->have = 0;
	sbus->before = SBUS_FOOTER;
}

static const struct rc_ops sbus_ops = {
	.parse = sbus_parse,
	.idle = sbus_idle,
};

int sbus_init(const struct sbus_config *config, struct sbus **out)
{
	struct rc_line line = {
		.baud = SBUS_BAUD,
		.even_parity = true,
		.two_stop_bits = true,
		.inverted = true,
	};
	struct sbus *sbus;
	int ret;

	if (!config || !out)
		return -EINVAL;
	line.inverted = !config->not_inverted;

	sbus = calloc(1, sizeof(*sbus));
	if (!sbus)
		return -ENOMEM;
	ret = rc_input_open(&sbus->input, &config->port, &line);
	if (ret) {
		free(sbus);
		return ret;
	}
	rc_dev_init(&sbus->rc, "sbus", &sbus_ops, sbus, &sbus->input,
		    SBUS_CHANNELS, SBUS_RAW_MIN, SBUS_RAW_MID, SBUS_RAW_MAX,
		    config->timeout_ms);
	*out = sbus;
	return 0;
}

int sbus_del(struct sbus *sbus)
{
	int ret;

	if (!sbus)
		return -EINVAL;
	ret = rc_input_close(&sbus->input);
	free(sbus);
	return ret;
}

struct rc_dev *sbus_rc(struct sbus *sbus)
{
	return sbus ? &sbus->rc : NULL;
}

int sbus_get_flags(const struct sbus *sbus, struct sbus_flags *flags)
{
	if (!sbus || !flags)
		return -EINVAL;
	if (!sbus->rc.frames)
		return -ENODATA;
	*flags = sbus->flags;
	return 0;
}

#endif /* CONFIG_RC_SBUS */
