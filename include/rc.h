/*
 * rc -- a small Linux-IIO-style subsystem for RC receivers on ESP-IDF and
 * the STM32 HAL: receivers that stream stick positions over one serial
 * wire, with or without a word on the state of the radio link.
 *
 * One struct rc_dev per receiver. It holds what every protocol here shares:
 * the channels as the wire carried them (raw) and normalised to -1..1, the
 * receiver's own failsafe flag where the protocol has one, and the age of
 * the last channel frame. What a protocol carries beyond that -- CRSF's link
 * statistics, SBUS's digital channels, the DR16's switches, mouse and keys
 * -- stays in its own header (crsf.h, sbus.h, dr16.h), next to its init.
 *
 * Bytes reach a receiver one of two ways, chosen at init:
 *
 *   - Read: the library reads the UART itself, through the bus layer, each
 *     time rc_update() runs. On ESP-IDF the init installs the UART driver,
 *     which buffers in the background. On the STM32 HAL a blocking read
 *     listens only while it runs, so bytes that arrive between two calls
 *     are lost: use fed there.
 *   - Fed: the app owns the UART and hands every byte it receives to
 *     rc_rx_feed(), from an interrupt or DMA callback or a task, and tells
 *     rc_rx_idle() when the line goes quiet. The bytes wait in a ring until
 *     the next rc_update().
 *
 * No clock is read anywhere in this library. The app passes its time to
 * rc_update(), which stamps every new channel frame with it, and to
 * rc_get_link(), which measures the age from that stamp. Read the time just
 * before the call: a stamp taken earlier than the frame arrived makes the
 * age read longer, never shorter.
 *
 * The failsafe is the app's to write. This library reports; only the
 * machine knows whether a lost link means coast, brake or cut.
 *
 * Not thread-safe: call everything for one receiver from one task, except
 * rc_rx_feed() and rc_rx_idle(), which may run in an interrupt -- one
 * writer, so both from the same one. Neither is in IRAM: on ESP-IDF, call
 * them from a task or an ISR that is not ESP_INTR_FLAG_IRAM.
 *
 * Every int-returning function returns 0 or a negative errno.
 */
#ifndef RC_H
#define RC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef ESP_PLATFORM
#include "driver/uart.h"
#elif !defined(CMSIS_device_header)
/*
 * Undefined, the include fails with only
 * `#include expects "FILENAME" or <FILENAME>`. Every library source includes
 * this header first, rc_priv.h too, so the check is here.
 */
#error "rc: CMSIS_device_header is not defined; define it as the STM32 family header, \"stm32f4xx.h\" (CMake: set(MCU_DEVICE_HEADER stm32f4xx.h))"
#else
#include CMSIS_device_header
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Idle marks kept between two rc_update() calls. */
#define RC_IDLE_MARKS		8

/* The most channels any protocol here carries in its channel words. */
#define RC_CHANNELS_MAX		16

/*
 * A link with no channel frame for this long is RC_LINK_TIMEOUT, when the
 * protocol's config says 0. Our policy, not a protocol's: the TBS CRSF
 * specification suggests waiting a whole second before a flight controller
 * starts its failsafe; a ground robot usually wants to stop sooner.
 */
#define RC_DEFAULT_TIMEOUT_MS	100

enum rc_link_state {
	RC_LINK_NO_FRAME,	/* no channel frame since init */
	RC_LINK_UP,		/* frames arriving, no failsafe flag */
	RC_LINK_FAILSAFE,	/* frames arriving, but the receiver says it
				 * has lost the radio: the channels are its
				 * failsafe values, not the sticks */
	RC_LINK_TIMEOUT,	/* no channel frame within timeout_ms */
};

struct rc_link {
	enum rc_link_state state;
	uint32_t age_ms;	/* since the last channel frame; UINT32_MAX
				 * before the first */
	uint32_t frames;	/* channel frames decoded since init */
	uint32_t errors;	/* frames thrown away: bad CRC, bad header or
				 * footer, out of range, wrong length */
	uint32_t dropped;	/* fed bytes that found the ring full */
};

/* Where the bytes come from. The line settings are the protocol's. */
#ifdef ESP_PLATFORM
struct rc_port_config {
	bool fed;		/* true: through rc_rx_feed(); the UART
				 * fields below are not used */
	uart_port_t uart;	/* the init installs and configures it, and
				 * del deletes it: the port is the library's */
	int rx_gpio;		/* the receiver's signal wire */
	int rx_buffer_len;	/* UART driver RX buffer, bytes; 0: 2048 */
};
#else
struct rc_port_config {
	bool fed;		/* true: through rc_rx_feed() */
#ifdef HAL_UART_MODULE_ENABLED
	/* fed false: HAL_UART_Init()ed by the app at the protocol's line
	 * settings; only its RX is used */
	UART_HandleTypeDef *uart;
#endif
};
#endif

struct rc_dev;
struct rc_input;	/* the protocol's byte source; private */

struct rc_ops {
	/*
	 * Take len bytes of the stream, in order. For each whole frame,
	 * write raw[] and call rc_frame_done(); for each bad one,
	 * rc_frame_error(). Keep a partial frame for the next call.
	 */
	void (*parse)(struct rc_dev *rc, const uint8_t *data, size_t len);
	/* optional: the line went quiet after the bytes parsed so far */
	void (*idle)(struct rc_dev *rc);
};

struct rc_dev {
	const char *name;		/* "crsf"; log prefix */
	const struct rc_ops *ops;
	void *priv;			/* the protocol's handle */
	struct rc_input *input;
	uint8_t channel_count;		/* channels in raw[] */
	/* The nominal ends and centre of a stick, in raw units; the
	 * normalised value is -1, 0 and 1 there, and linear beyond. */
	uint16_t raw_min;
	uint16_t raw_mid;
	uint16_t raw_max;
	uint32_t timeout_ms;

	/* The last channel frame, written by the protocol's parse. */
	uint16_t raw[RC_CHANNELS_MAX];
	bool failsafe;
	bool new_frame;

	/* The core's. */
	bool seen;			/* a channel frame since init */
	uint32_t frame_ms;		/* the now_ms it arrived at */
	bool expired;			/* an rc_update() found it older than
					 * timeout_ms; held until a new frame,
					 * so the age wrapping cannot revive it */
	uint32_t frames;
	uint32_t errors;
};

/*
 * Take every byte waiting -- in the ring when fed, on the UART when read --
 * and decode what is complete. Never waits. A new channel frame is stamped
 * now_ms, and when several arrived since the last call, the newest wins.
 * Without one, a last frame older than timeout_ms is latched as timed out
 * until the next frame arrives. 0 when there was a new channel frame. Else
 * -EAGAIN, or on the read path a UART error other than an empty line, as it
 * came.
 */
int rc_update(struct rc_dev *rc, uint32_t now_ms);

/*
 * The last channel frame, count channels from the first, normalised: -1,
 * 0 and 1 at the protocol's nominal ends and centre, and linear beyond,
 * not clamped -- receivers do send past the ends. -EINVAL for count over
 * channel_count, -ENODATA before the first frame.
 */
int rc_get_channels(const struct rc_dev *rc, float *channels, size_t count);

/* The same channels in the wire's own units. */
int rc_get_raw(const struct rc_dev *rc, uint16_t *raw, size_t count);

/*
 * The link's state and counters, the age measured to now_ms: now_ms minus
 * the stamp, modulo 2^32. A now_ms from before the stamp reads as a huge
 * age, so a timeout. Past 2^32 ms the age wraps; the latch rc_update()
 * sets keeps the state RC_LINK_TIMEOUT all the same, as long as
 * rc_update() runs at least once every 49 days.
 */
int rc_get_link(const struct rc_dev *rc, uint32_t now_ms,
		struct rc_link *link);

/*
 * Hand over bytes the app's UART received. ISR-safe and never blocks.
 * Returns how many were taken: fewer when the ring is full. 0 for a
 * receiver not made with fed.
 */
size_t rc_rx_feed(struct rc_dev *rc, const uint8_t *data, size_t len);

/*
 * Say the line went quiet after the bytes fed so far: on the STM32 HAL,
 * from HAL_UARTEx_RxEventCallback(); on ESP-IDF, for a UART_DATA event
 * with timeout_flag set. ISR-safe. A frame boundary for protocols with no
 * header of their own; the DR16 needs it. The last RC_IDLE_MARKS marks
 * before an rc_update() are kept; older ones are lost, and the frames
 * they ended run together and are thrown away, never misread.
 */
void rc_rx_idle(struct rc_dev *rc);

#ifdef __cplusplus
}
#endif

#endif /* RC_H */
