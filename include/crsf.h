/*
 * CRSF receivers: TBS Crossfire, ExpressLRS and anything else that speaks
 * the same frames. A port of esp-crsf-rc onto struct rc_dev.
 *
 * A bound receiver streams sixteen channels, typically 50 to 500 times a
 * second, and interleaves a link statistics frame saying how healthy the
 * radio link is. Every frame carries a CRC. The line is 8N1 and not
 * inverted ([TBS] crsf.md:138). This driver only ever reads: no telemetry
 * goes back to the radio.
 *
 * CRSF has no failsafe flag. When the receiver loses the radio it stops
 * sending channel frames, in "cut" failsafe mode ([TBS] crsf.md:522), or
 * keeps sending whatever its failsafe is set to. So the link state goes to
 * RC_LINK_TIMEOUT, never RC_LINK_FAILSAFE; gate on that, and on the link
 * quality, which sags before frames stop.
 *
 *   struct crsf *receiver;
 *   crsf_init(&(struct crsf_config){ .port = { .uart = UART_NUM_1,
 *                                              .rx_gpio = 16 } },
 *             &receiver);
 *   struct rc_dev *rc = crsf_rc(receiver);
 *   loop:
 *     rc_update(rc, now_ms);
 *     rc_get_link(rc, now_ms, &link);
 *     rc_get_channels(rc, sticks, 4);
 *
 * Every byte offset, scale and flag is cited in src/crsf/crsf_proto.h.
 * Never tested against a real receiver.
 */
#ifndef CRSF_H
#define CRSF_H

#include <stdint.h>

#include "rc.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CRSF_CHANNELS		16

/*
 * The nominal ends and centre of a stick, in raw units: 988, 1500 and
 * 2012 us (ExpressLRS 4.1.0 crsf_protocol.h:20-24; the centre also TBS
 * crsf.md:528). A receiver is free to send outside this range, 0 to 1984
 * with ExpressLRS's extended limits on; nothing clamps.
 */
#define CRSF_RAW_MIN		172
#define CRSF_RAW_MID		992
#define CRSF_RAW_MAX		1811

/*
 * What ExpressLRS ships and Betaflight defaults to (Betaflight
 * crsf_protocol.h:33-37; crsf-wg wiki Physical-Layer.md:28). Not the
 * specification's: that says 416666 for dual-wire ([TBS] crsf.md:138) and
 * 400000 for single-wire (crsf.md:134). If no frame arrives, or the error
 * count explodes, try those.
 */
#define CRSF_BAUD_DEFAULT	420000

struct crsf;

struct crsf_config {
	struct rc_port_config port;
	/*
	 * ESP-IDF, read only: 0 for CRSF_BAUD_DEFAULT. On the HAL and when
	 * fed, the baud is the app's.
	 */
	uint32_t baud;
	uint32_t timeout_ms;	/* RC_LINK_TIMEOUT after this; 0: 100 ms */
};

/* The uplink half of the last link statistics frame. */
struct crsf_link_stats {
	/* Uplink link quality, 0..100 %. The number to gate an early
	 * warning on: it falls before the link dies, where RSSI stays flat
	 * until it does not. */
	uint8_t link_quality;
	/* Uplink RSSI per antenna, as the positive magnitude the wire
	 * carries: 85 means -85 dBm, and bigger is weaker. A diversity
	 * receiver has two; on one with a single antenna, rssi2 is whatever
	 * it sends. */
	uint8_t rssi1;
	uint8_t rssi2;
	uint8_t antenna;	/* the active one, 0 or 1 */
	int8_t snr_db;		/* uplink signal-to-noise ratio, signed */
};

/*
 * Make the receiver. Read on ESP-IDF: the init installs the UART at the
 * baud, 8N1, on rx_gpio, and del deletes it. Returns at once: a receiver
 * that never talks shows as RC_LINK_NO_FRAME with no frames and no errors.
 * -EINVAL for no config or out, -ENOMEM, -EOPNOTSUPP for read on a HAL
 * build without HAL_UART_MODULE_ENABLED, or the UART's error.
 */
int crsf_init(const struct crsf_config *config, struct crsf **out);

/* Close the UART when the init opened it, and free. */
int crsf_del(struct crsf *crsf);

/* The receiver as a struct rc_dev; NULL for NULL. Valid until del. */
struct rc_dev *crsf_rc(struct crsf *crsf);

/*
 * The last link statistics frame, which rc_update() collects with the
 * channels. -ENODATA before the first: a powered receiver sends them even
 * with no radio bound. Kept as it was when the link goes: after a loss
 * this is the last frame before it, not a live reading -- check
 * rc_get_link() first.
 */
int crsf_get_link_stats(const struct crsf *crsf,
			struct crsf_link_stats *stats);

/*
 * Raw units to servo microseconds. Exact at all three fixed points: 172
 * gives 988, 992 gives 1500 and 1811 gives 2012, because 1811 - 172 is 1639
 * and 2012 - 988 is 1024. Values outside the nominal range extend the same
 * straight line rather than clamping, so 0 gives 881.
 *
 * This is Betaflight's 0.62477120195241 * raw + 881 (crsf.c:624-627). The
 * specification's TICKS_TO_US, (raw - 992) * 5 / 8 + 1500 (crsf.md:525),
 * gives 2011 at the top in integer arithmetic: upstream disagrees with
 * itself by a microsecond.
 */
static inline uint16_t crsf_us(uint16_t raw)
{
	return (uint16_t)(988 + ((int32_t)raw - CRSF_RAW_MIN) * 1024 / 1639);
}

#ifdef __cplusplus
}
#endif

#endif /* CRSF_H */
