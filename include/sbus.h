/*
 * SBUS receivers (Futaba, FrSky and the many that copy them): 16
 * proportional channels and two digital ones, with the receiver's own
 * frame-lost and failsafe flags, every 25-byte frame.
 *
 * The line is 100000 baud, 8E2, and inverted: idle low. ESP-IDF inverts in
 * the UART itself, which the init sets up. On the STM32 the inversion is
 * the app's: an inverting buffer in front of the pin, or the USART's own RX
 * inversion where the family has one (HAL AdvancedInit) -- see
 * docs/hardware.md.
 *
 * The failsafe flag is what the receiver says; RC_LINK_FAILSAFE follows
 * it. While it is set the channels are the receiver's failsafe values,
 * not the sticks. Frame lost is a single dropped radio packet, not a lost
 * link: it is in sbus_get_flags() only.
 *
 *   struct sbus *receiver;
 *   sbus_init(&(struct sbus_config){ .port = { .uart = UART_NUM_1,
 *                                              .rx_gpio = 16 } },
 *             &receiver);
 *   struct rc_dev *rc = sbus_rc(receiver);
 *
 * Every byte offset and flag is cited in src/sbus/sbus_proto.h. Never
 * tested against a real receiver.
 */
#ifndef SBUS_H
#define SBUS_H

#include <stdbool.h>
#include <stdint.h>

#include "rc.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SBUS_CHANNELS		16

/*
 * The nominal ends and centre of a stick, in raw units: Betaflight's fit
 * of 988, 1500 and 2012 us against an FrSky X4R (sbus_channels.c:41-42,
 * :92-94). ArduPilot maps 200..1800 to 1000..2000 us instead
 * (AP_RCProtocol_SBUS.cpp:66-76): the sources disagree by a few percent,
 * and this library takes Betaflight's. Nothing clamps.
 */
#define SBUS_RAW_MIN		173
#define SBUS_RAW_MID		993
#define SBUS_RAW_MAX		1812

struct sbus;

struct sbus_config {
	struct rc_port_config port;
	/*
	 * ESP-IDF, read only: true for a receiver whose SBUS output is not
	 * inverted (some have such a pad). Betaflight's serialrx_inverted,
	 * Bolder Flight's inv.
	 */
	bool not_inverted;
	uint32_t timeout_ms;	/* RC_LINK_TIMEOUT after this; 0: 100 ms */
};

/* The flags byte of the last frame. */
struct sbus_flags {
	bool channel_17;	/* the two digital channels */
	bool channel_18;
	bool frame_lost;	/* the receiver missed a radio packet */
	bool failsafe;		/* the receiver has lost the radio */
};

/*
 * Make the receiver. Read on ESP-IDF: the init installs the UART at
 * 100000 8E2, inverted unless not_inverted, on rx_gpio, and del deletes
 * it. Returns at once. -EINVAL for no config or out, -ENOMEM, -EOPNOTSUPP
 * for read on a HAL build without HAL_UART_MODULE_ENABLED, or the UART's
 * error.
 */
int sbus_init(const struct sbus_config *config, struct sbus **out);

int sbus_del(struct sbus *sbus);

/* The receiver as a struct rc_dev; NULL for NULL. Valid until del. */
struct rc_dev *sbus_rc(struct sbus *sbus);

/* The flags of the last frame. -ENODATA before the first. */
int sbus_get_flags(const struct sbus *sbus, struct sbus_flags *flags);

#ifdef __cplusplus
}
#endif

#endif /* SBUS_H */
