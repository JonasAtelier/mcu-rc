/*
 * The DJI DBUS wire format of the DR16 receiver (DT7 radio, RoboMaster),
 * host-testable; nothing here touches a UART.
 *
 * The DT7 & DR16 user manual shows the DBUS port and nothing of the
 * protocol; the frame table DJI cites is in an attachment we do not have
 * (Type A manual p.5). The facts below come from DJI's own RoboMaster
 * code and teaching material, each pinned; the code is GPL-3.0, so facts
 * only, nothing copied (docs/chips.md):
 *
 *   [TUT]  RoboMaster 开发板 C 型嵌入式软件教程文档 (Development Board C
 *          tutorial), DJI 2020, in RoboMaster/Development-Board-C-Examples
 *          commit 59d12b1a
 *   [BC]   the same repo, 9.remote_control_dma/: applications/
 *          remote_control.c and .h, Src/usart.c
 *   [RTS]  RoboMaster/RoboRTS-Firmware components/devices/dbus.c and .h,
 *          commit 8cffd936
 *   [A]    RoboMaster 竞赛机器人 2020 自组装版 A 型 用户手册 (Type A robot
 *          user manual), in the [TUT] repo, doc/
 *   [DT7]  DT7 & DR16 2.4 GHz RC System User Manual v2.0, 2015
 *
 * The line: 100 kbit/s, 8 data bits, even parity, 1 stop bit, and the
 * levels inverted against a UART's, so a UART needs an inverter ([TUT]
 * p.116, section 9.3.2; [BC] usart.c:36-40 sets 100000, even parity, RX
 * only). [TUT] p.118, step 4, says "no parity" for the same USART3, which
 * contradicts both p.116 and its own usart.c; we follow p.116.
 *
 * A frame is 18 bytes, 144 bits ([TUT] p.117), and has no header, footer
 * or checksum. DJI's code frames it by the idle line: it takes the bytes
 * that arrived between two idle interrupts, and decodes them only when
 * there are exactly 18 ([BC] remote_control.c:81-152, the length test at
 * :117 and :145). So does this driver: see rc_rx_idle().
 *
 *   bits    field                        source
 *   0-10    channel 0, 11 bits           [TUT] p.124; [BC] .c:174; [RTS] .c:93
 *   11-21   channel 1                    [BC] .c:175; [RTS] .c:95
 *   22-32   channel 2                    [BC] .c:176-177; [RTS] .c:97
 *   33-43   channel 3                    [BC] .c:178; [RTS] .c:99
 *   44-45   switch S2, the left one      [BC] .c:179; [RTS] .c:121
 *   46-47   switch S1, the right one     [BC] .c:180; [RTS] .c:120
 *   48-63   mouse x, int16 little end.   [BC] .c:181; [RTS] .c:132
 *   64-79   mouse y                      [BC] .c:182; [RTS] .c:133
 *   80-95   mouse z                      [BC] .c:183; [RTS] .c:134
 *   96-103  mouse left button            [BC] .c:184; [RTS] .c:136
 *   104-111 mouse right button           [BC] .c:185; [RTS] .c:137
 *   112-127 keyboard, one bit a key      [BC] .c:186; [RTS] .c:139
 *   128-143 wheel, 1024 at rest          [RTS] .c:140; [BC] .c:187, :193
 *                                        calls it "NULL"
 *
 * Which switch is which: [BC] names bits 44-45 "Switch left" and 46-47
 * "Switch right" (.c:179-180); [RTS] names 46-47 sw1 and 44-45 sw2
 * (.c:120-121); [A] p.12 puts S1 at the radio's top right. So S1 is the
 * right switch and S2 the left. [DT7] p.2 labels both on the radio.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "dr16.h"
#include "rc_priv.h"

#define DR16_BAUD		100000	/* [TUT] p.116; [BC] usart.c:36 */
#define DR16_FRAME_LEN		18	/* [TUT] p.117; [BC] remote_control.h:26 */

/*
 * The stick range. [BC] remote_control.h:28-30 gives 364..1684, centre
 * 1024; [RTS] .c:123-130 throws a frame away when any stick is more than
 * 660 from 1024 -- the same range. A frame outside it is not a frame.
 */
static inline bool dr16_stick_ok(uint16_t raw)
{
	return raw >= DR16_RAW_MIN && raw <= DR16_RAW_MAX;
}

/* Switch values: [BC] remote_control.h:33-35; [RTS] .c:146-200 knows only
 * these three. */
static inline bool dr16_switch_ok(uint8_t position)
{
	return position == DR16_SWITCH_UP || position == DR16_SWITCH_MIDDLE ||
	       position == DR16_SWITCH_DOWN;
}

static inline int16_t dr16_int16(const uint8_t *bytes)
{
	return (int16_t)(uint16_t)(bytes[0] | bytes[1] << 8);
}

/*
 * Decode 18 bytes: the sticks into raw, the rest into input. false, and
 * nothing written, when a stick or a switch is out of range.
 */
static inline bool dr16_unpack(const uint8_t *frame,
			       uint16_t raw[DR16_CHANNELS],
			       struct dr16_input *input)
{
	uint16_t sticks[DR16_CHANNELS];
	uint8_t switch_s1 = (frame[5] >> 6) & 0x03;
	uint8_t switch_s2 = (frame[5] >> 4) & 0x03;

	rc_unpack_11bit(frame, sticks, DR16_CHANNELS);
	for (int i = 0; i < DR16_CHANNELS; i++)
		if (!dr16_stick_ok(sticks[i]))
			return false;
	if (!dr16_switch_ok(switch_s1) || !dr16_switch_ok(switch_s2))
		return false;

	for (int i = 0; i < DR16_CHANNELS; i++)
		raw[i] = sticks[i];
	input->switch_s1 = switch_s1;
	input->switch_s2 = switch_s2;
	input->mouse_x = dr16_int16(frame + 6);
	input->mouse_y = dr16_int16(frame + 8);
	input->mouse_z = dr16_int16(frame + 10);
	input->mouse_left = frame[12] != 0;
	input->mouse_right = frame[13] != 0;
	input->keys = (uint16_t)(frame[14] | frame[15] << 8);
	input->wheel = (int16_t)((frame[16] | frame[17] << 8) - DR16_RAW_MID);
	return true;
}
