/*
 * The SBUS wire format, host-testable; nothing here touches a UART.
 *
 * Futaba publishes no SBUS specification. Every fact below is the
 * agreement of three open implementations, each pinned; the GPL ones are
 * facts only, no code is copied (docs/chips.md):
 *
 *   [BF]   Betaflight src/main/rx/sbus.c, sbus_channels.c, sbus_channels.h,
 *          commit cd8e8386 (GPL-3.0)
 *   [AP]   ArduPilot libraries/AP_RCProtocol/AP_RCProtocol_SBUS.cpp,
 *          commit 3297d9f1 (GPL-3.0)
 *   [BFS]  Bolder Flight Systems sbus src/sbus.h, src/sbus.cpp,
 *          commit 003c5c33 (MIT)
 *
 * The line: 100000 baud, 8 data bits, even parity, 2 stop bits, inverted
 * ([BF] sbus.c:59, :82, :205-207; [BFS] sbus.h:75, sbus.cpp:59-63).
 *
 * A frame is 25 bytes ([BF] sbus.c:69 with sbus_channels.h:30-51; [AP]
 * :60; [BFS] sbus.h:77-79):
 *
 *   byte  0      header, 0x0F
 *   bytes 1-22   16 channels x 11 bits, least significant bit first
 *   byte  23     flags: bit 0 channel 17, bit 1 channel 18,
 *                bit 2 frame lost, bit 3 failsafe
 *   byte  24     footer
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "rc_priv.h"
#include "sbus.h"

#define SBUS_BAUD		100000	/* [BF] sbus.c:59; [BFS] sbus.h:75 */

#define SBUS_FRAME_LEN		25	/* [AP] :60; [BFS] sbus.h:77-79 */
#define SBUS_HEADER		0x0F	/* [BF] sbus.c:71; [AP] :94; [BFS] sbus.h:82 */
#define SBUS_FLAGS_BYTE		23	/* [AP] :62; [BFS] sbus.cpp:149-155 */

/* Flag bits: [BF] sbus_channels.c:38-39 and sbus_channels.h:27-28; [AP]
 * :63-64, :108-110; [BFS] sbus.h:85-88. All three agree. */
#define SBUS_FLAG_CHANNEL_17	(1u << 0)
#define SBUS_FLAG_CHANNEL_18	(1u << 1)
#define SBUS_FLAG_FRAME_LOST	(1u << 2)
#define SBUS_FLAG_FAILSAFE	(1u << 3)

/*
 * The footer. The sources disagree here. [BFS] accepts 0x00, or a low
 * nibble of 0x04 -- 0x04, 0x14, 0x24, 0x34 -- (sbus.h:83-84,
 * sbus.cpp:110); [BF] says 0x00 on FrSky and some Futaba receivers, and on
 * some SBUS2 receivers a byte saying which telemetry slot follows
 * (sbus.c:97-104), and neither it nor [AP] checks the byte at all: they
 * frame by the gap between frames (sbus.c:130-135; [AP] :79, :172-187).
 * This library has no clock to measure a gap with, so it checks the footer
 * as [BFS] does, to find the frame boundary in a stream that has none.
 */
#define SBUS_FOOTER		0x00
#define SBUS_FOOTER_SBUS2	0x04	/* low nibble */

static inline bool sbus_footer_ok(uint8_t footer)
{
	return footer == SBUS_FOOTER ||
	       (footer & 0x0F) == SBUS_FOOTER_SBUS2;
}

/* Whether 25 bytes at frame are one frame: the header and the footer. */
static inline bool sbus_frame_ok(const uint8_t *frame)
{
	return frame[0] == SBUS_HEADER &&
	       sbus_footer_ok(frame[SBUS_FRAME_LEN - 1]);
}

/*
 * The 16 channels of a frame that passed sbus_frame_ok(). Bytes 1-22 are
 * one bit stream, channel k at bits 11k to 11k+10, least significant bit
 * first: [BF] sbus_channels.h:30-49 declares them as consecutive 11-bit
 * fields after the header, and [BFS] sbus.cpp:112-147 spells out the same
 * shifts, channel 0 from buf[1] and the low 3 bits of buf[2].
 */
static inline void sbus_unpack_channels(const uint8_t *frame,
					uint16_t raw[SBUS_CHANNELS])
{
	rc_unpack_11bit(frame + 1, raw, SBUS_CHANNELS);
}

static inline void sbus_unpack_flags(const uint8_t *frame,
				     struct sbus_flags *flags)
{
	uint8_t bits = frame[SBUS_FLAGS_BYTE];

	flags->channel_17 = bits & SBUS_FLAG_CHANNEL_17;
	flags->channel_18 = bits & SBUS_FLAG_CHANNEL_18;
	flags->frame_lost = bits & SBUS_FLAG_FRAME_LOST;
	flags->failsafe = bits & SBUS_FLAG_FAILSAFE;
}
