/* First: on ESP-IDF it brings in sdkconfig.h, which the #if below reads. */
#include "rc_priv.h"

#if CONFIG_RC_CRSF

#include <stdlib.h>
#include <string.h>

#include "crsf.h"
#include "crsf_proto.h"

struct crsf {
	struct rc_dev rc;		/* the category view; priv points back */
	struct rc_input input;
	uint8_t frame[CRSF_MAX_FRAME];	/* bytes held over between parses */
	size_t have;			/* how many of them are valid */
	struct crsf_link_stats link;
	bool have_link;
};

static void crsf_drop(struct crsf *crsf, size_t n)
{
	crsf->have -= n;
	memmove(crsf->frame, crsf->frame + n, crsf->have);
}

/* File one frame that passed its CRC by type. */
static void crsf_handle_frame(struct crsf *crsf)
{
	const uint8_t *f = crsf->frame;
	size_t len = crsf_frame_payload_len(f);

	switch (crsf_frame_type(f)) {
	case CRSF_TYPE_RC_CHANNELS:
		/*
		 * A channel frame shorter than 16 channels is not one,
		 * whatever its CRC says: unpacking it would read past the
		 * payload. Longer is fine: ExpressLRS 4 appends an arm-status
		 * byte, and the first 22 bytes are the channels either way
		 * (see CRSF_RC_PAYLOAD_ARMED).
		 */
		if (len < CRSF_RC_PAYLOAD) {
			rc_frame_error(&crsf->rc);
			break;
		}
		crsf_unpack_channels(crsf_frame_payload(f), crsf->rc.raw);
		rc_frame_done(&crsf->rc, false);
		break;
	case CRSF_TYPE_LINK_STATS:
		if (len < CRSF_LINK_PAYLOAD) {
			rc_frame_error(&crsf->rc);
			break;
		}
		crsf_unpack_link(crsf_frame_payload(f), &crsf->link);
		crsf->have_link = true;
		break;
	default:
		/* Telemetry, parameter and MSP frames share this wire:
		 * framed, checked and thrown away. */
		break;
	}
}

/*
 * Consume whole frames from what is held. A receiver talks continuously and
 * never waits to be asked, so the first bytes after power-up almost always
 * land in the middle of a frame. Frames are variable length, so the byte
 * that says how long this one is comes from the same run of bytes whose
 * alignment is in doubt: resynchronise one byte at a time.
 */
static void crsf_take_frames(struct crsf *crsf)
{
	for (;;) {
		size_t size;

		/* Two bytes say whether this can start a frame, and its
		 * length. */
		if (crsf->have < 2)
			return;
		size = crsf_frame_size(crsf->frame);
		if (!size) {
			crsf_drop(crsf, 1);
			continue;
		}
		if (crsf->have < size)
			return;
		if (!crsf_crc_ok(crsf->frame)) {
			/* Step one byte, not one frame. The length that would
			 * say how far to skip came out of the same doubtful
			 * bytes as the CRC that just failed, and skipping by
			 * it would step straight over the real sync byte if
			 * the length were the corrupted part. */
			rc_frame_error(&crsf->rc);
			crsf_drop(crsf, 1);
			continue;
		}
		crsf_handle_frame(crsf);
		crsf_drop(crsf, size);
	}
}

/*
 * After crsf_take_frames() fewer than one whole frame is held, at most
 * CRSF_MAX_FRAME - 1 bytes, so every pass takes at least one new byte.
 */
static void crsf_parse(struct rc_dev *rc, const uint8_t *data, size_t len)
{
	struct crsf *crsf = rc->priv;

	while (len) {
		size_t room = sizeof(crsf->frame) - crsf->have;
		size_t n = len < room ? len : room;

		memcpy(crsf->frame + crsf->have, data, n);
		crsf->have += n;
		data += n;
		len -= n;
		crsf_take_frames(crsf);
	}
}

static const struct rc_ops crsf_ops = {
	.parse = crsf_parse,
};

int crsf_init(const struct crsf_config *config, struct crsf **out)
{
	struct rc_line line = { .baud = CRSF_BAUD_DEFAULT };
	struct crsf *crsf;
	int ret;

	if (!config || !out)
		return -EINVAL;
	if (config->baud)
		line.baud = config->baud;

	crsf = calloc(1, sizeof(*crsf));
	if (!crsf)
		return -ENOMEM;
	ret = rc_input_open(&crsf->input, &config->port, &line);
	if (ret) {
		free(crsf);
		return ret;
	}
	rc_dev_init(&crsf->rc, "crsf", &crsf_ops, crsf, &crsf->input,
		    CRSF_CHANNELS, CRSF_RAW_MIN, CRSF_RAW_MID, CRSF_RAW_MAX,
		    config->timeout_ms);
	*out = crsf;
	return 0;
}

int crsf_del(struct crsf *crsf)
{
	int ret;

	if (!crsf)
		return -EINVAL;
	ret = rc_input_close(&crsf->input);
	free(crsf);
	return ret;
}

struct rc_dev *crsf_rc(struct crsf *crsf)
{
	return crsf ? &crsf->rc : NULL;
}

int crsf_get_link_stats(const struct crsf *crsf,
			struct crsf_link_stats *stats)
{
	if (!crsf || !stats)
		return -EINVAL;
	if (!crsf->have_link)
		return -ENODATA;
	*stats = crsf->link;
	return 0;
}

#endif /* CONFIG_RC_CRSF */
