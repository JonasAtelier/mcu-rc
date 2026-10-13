/*
 * The DJI DR16 receiver of the DT7 radio, as RoboMaster robots use it: four
 * sticks, two three-position switches, and the mouse and keyboard of a PC
 * the radio can relay, on DJI's DBUS line.
 *
 * Fed only. A DBUS frame has no header and no checksum; the only thing
 * that says where one starts is the quiet line between two frames, which
 * DJI's own code waits for. The app owns the UART (100000 baud, 8E1,
 * inverted -- see docs/hardware.md), hands every byte to rc_rx_feed() and
 * calls rc_rx_idle() when the line goes idle:
 *
 *   STM32 HAL: HAL_UARTEx_ReceiveToIdle_DMA(); in
 *              HAL_UARTEx_RxEventCallback(), rc_rx_feed() the new bytes,
 *              then rc_rx_idle() when the event is an idle one.
 *   ESP-IDF:   a task on the UART's event queue: for each UART_DATA,
 *              uart_read_bytes() its size into rc_rx_feed(), then
 *              rc_rx_idle() when timeout_flag is set.
 *
 * A burst between two idles counts as a frame only when it is exactly 18
 * bytes and every stick and switch is in range. Anything else is an error,
 * and the first burst after start usually is: the stream was picked up
 * mid-frame.
 *
 * DBUS has no failsafe flag, so a lost radio shows as RC_LINK_TIMEOUT
 * once frames stop.
 *
 * Every field is cited in src/dr16/dr16_proto.h. Never tested against a
 * real receiver.
 */
#ifndef DR16_H
#define DR16_H

#include <stdbool.h>
#include <stdint.h>

#include "rc.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DR16_CHANNELS		4	/* the sticks, in wire order */

/* The sticks' ends and centre, in raw units (RoboMaster Development Board
 * C examples, remote_control.h:28-30). */
#define DR16_RAW_MIN		364
#define DR16_RAW_MID		1024
#define DR16_RAW_MAX		1684

/* Switch positions, as the wire has them (remote_control.h:33-35). */
#define DR16_SWITCH_UP		1
#define DR16_SWITCH_DOWN	2
#define DR16_SWITCH_MIDDLE	3

/* One bit a key in dr16_input.keys (remote_control.h:40-55; RoboRTS
 * dbus.h:74-89 agrees). */
#define DR16_KEY_W		(1u << 0)
#define DR16_KEY_S		(1u << 1)
#define DR16_KEY_A		(1u << 2)
#define DR16_KEY_D		(1u << 3)
#define DR16_KEY_SHIFT		(1u << 4)
#define DR16_KEY_CTRL		(1u << 5)
#define DR16_KEY_Q		(1u << 6)
#define DR16_KEY_E		(1u << 7)
#define DR16_KEY_R		(1u << 8)
#define DR16_KEY_F		(1u << 9)
#define DR16_KEY_G		(1u << 10)
#define DR16_KEY_Z		(1u << 11)
#define DR16_KEY_X		(1u << 12)
#define DR16_KEY_C		(1u << 13)
#define DR16_KEY_V		(1u << 14)
#define DR16_KEY_B		(1u << 15)

struct dr16;

struct dr16_config {
	struct rc_port_config port;	/* fed must be true */
	uint32_t timeout_ms;	/* RC_LINK_TIMEOUT after this; 0: 100 ms */
};

/* What the last frame carried besides the sticks. */
struct dr16_input {
	uint8_t switch_s1;	/* the right switch: DR16_SWITCH_* */
	uint8_t switch_s2;	/* the left switch */
	int16_t mouse_x;	/* as the radio relays them; no unit given */
	int16_t mouse_y;
	int16_t mouse_z;
	bool mouse_left;
	bool mouse_right;
	uint16_t keys;		/* DR16_KEY_* */
	/* The left dial, relative to its rest of 1024. One source names it
	 * (RoboRTS dbus.c:140); DJI's board C example calls the same bytes
	 * "NULL". No range given. */
	int16_t wheel;
};

/*
 * Make the receiver. -EOPNOTSUPP unless config->port.fed: see above.
 * -EINVAL for no config or out, -ENOMEM.
 */
int dr16_init(const struct dr16_config *config, struct dr16 **out);

int dr16_del(struct dr16 *dr16);

/* The receiver as a struct rc_dev; NULL for NULL. Valid until del. */
struct rc_dev *dr16_rc(struct dr16 *dr16);

/* Switches, mouse, keys and wheel of the last frame. -ENODATA before the
 * first. */
int dr16_get_input(const struct dr16 *dr16, struct dr16_input *input);

#ifdef __cplusplus
}
#endif

#endif /* DR16_H */
