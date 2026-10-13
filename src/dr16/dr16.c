/* First: on ESP-IDF it brings in sdkconfig.h, which the #if below reads. */
#include "rc_priv.h"

#if CONFIG_RC_DR16

#include <stdlib.h>
#include <string.h>

#include "dr16.h"
#include "dr16_proto.h"

struct dr16 {
	struct rc_dev rc;		/* the category view; priv points back */
	struct rc_input input;
	uint8_t frame[DR16_FRAME_LEN];
	size_t have;			/* bytes since the last idle; past
					 * DR16_FRAME_LEN only counted */
	struct dr16_input last;
};

/* Hold the first 18 bytes of a burst and count the rest. */
static void dr16_parse(struct rc_dev *rc, const uint8_t *data, size_t len)
{
	struct dr16 *dr16 = rc->priv;

	for (size_t i = 0; i < len; i++) {
		if (dr16->have < DR16_FRAME_LEN)
			dr16->frame[dr16->have] = data[i];
		if (dr16->have <= DR16_FRAME_LEN)
			dr16->have++;
	}
}

/* The line went quiet: the burst since the last idle is one frame, or
 * nothing. */
static void dr16_idle(struct rc_dev *rc)
{
	struct dr16 *dr16 = rc->priv;
	size_t have = dr16->have;

	dr16->have = 0;
	if (!have)
		return;
	if (have != DR16_FRAME_LEN ||
	    !dr16_unpack(dr16->frame, rc->raw, &dr16->last)) {
		rc_frame_error(rc);
		return;
	}
	rc_frame_done(rc, false);
}

static const struct rc_ops dr16_ops = {
	.parse = dr16_parse,
	.idle = dr16_idle,
};

int dr16_init(const struct dr16_config *config, struct dr16 **out)
{
	static const struct rc_line line = {
		.baud = DR16_BAUD,
		.even_parity = true,
	};
	struct dr16 *dr16;
	int ret;

	if (!config || !out)
		return -EINVAL;
	if (!config->port.fed)
		return -EOPNOTSUPP;

	dr16 = calloc(1, sizeof(*dr16));
	if (!dr16)
		return -ENOMEM;
	ret = rc_input_open(&dr16->input, &config->port, &line);
	if (ret) {
		free(dr16);
		return ret;
	}
	rc_dev_init(&dr16->rc, "dr16", &dr16_ops, dr16, &dr16->input,
		    DR16_CHANNELS, DR16_RAW_MIN, DR16_RAW_MID, DR16_RAW_MAX,
		    config->timeout_ms);
	*out = dr16;
	return 0;
}

int dr16_del(struct dr16 *dr16)
{
	int ret;

	if (!dr16)
		return -EINVAL;
	ret = rc_input_close(&dr16->input);
	free(dr16);
	return ret;
}

struct rc_dev *dr16_rc(struct dr16 *dr16)
{
	return dr16 ? &dr16->rc : NULL;
}

int dr16_get_input(const struct dr16 *dr16, struct dr16_input *input)
{
	if (!dr16 || !input)
		return -EINVAL;
	if (!dr16->rc.frames)
		return -ENODATA;
	*input = dr16->last;
	return 0;
}

#endif /* CONFIG_RC_DR16 */
