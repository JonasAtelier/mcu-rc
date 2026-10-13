/*
 * Host-side check of the SBUS wire format: header and footer, the flag
 * bits, and the 16 channels.
 *
 * The channel check uses an oracle from outside this repo: Bolder Flight
 * Systems' per-channel shift expressions, src/sbus.cpp:112-147 at commit
 * 003c5c33, MIT licensed, transcribed below as they stand. They spell out
 * every channel's bytes and shifts one by one, where this library walks a
 * bit window, so a disagreement on any channel boundary shows.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sbus_proto.h"

/*
 * Bolder Flight's unpack, as it stands; buf_ is the whole 25-byte frame.
 * The expressions below are copied from src/sbus.cpp:112-147 at commit
 * 003c5c33, under this notice:
 *
 * Brian R Taylor
 * brian.taylor@bolderflight.com
 *
 * Copyright (c) 2022 Bolder Flight Systems Inc
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the “Software”), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED “AS IS”, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */
static void oracle(const uint8_t *buf_, uint16_t ch[16])
{
	ch[0]  = (uint16_t)(buf_[1] | ((buf_[2] << 8) & 0x07FF));
	ch[1]  = (uint16_t)((buf_[2] >> 3) | ((buf_[3] << 5) & 0x07FF));
	ch[2]  = (uint16_t)((buf_[3] >> 6) | (buf_[4] << 2) |
			    ((buf_[5] << 10) & 0x07FF));
	ch[3]  = (uint16_t)((buf_[5] >> 1) | ((buf_[6] << 7) & 0x07FF));
	ch[4]  = (uint16_t)((buf_[6] >> 4) | ((buf_[7] << 4) & 0x07FF));
	ch[5]  = (uint16_t)((buf_[7] >> 7) | (buf_[8] << 1) |
			    ((buf_[9] << 9) & 0x07FF));
	ch[6]  = (uint16_t)((buf_[9] >> 2) | ((buf_[10] << 6) & 0x07FF));
	ch[7]  = (uint16_t)((buf_[10] >> 5) | ((buf_[11] << 3) & 0x07FF));
	ch[8]  = (uint16_t)(buf_[12] | ((buf_[13] << 8) & 0x07FF));
	ch[9]  = (uint16_t)((buf_[13] >> 3) | ((buf_[14] << 5) & 0x07FF));
	ch[10] = (uint16_t)((buf_[14] >> 6) | (buf_[15] << 2) |
			    ((buf_[16] << 10) & 0x07FF));
	ch[11] = (uint16_t)((buf_[16] >> 1) | ((buf_[17] << 7) & 0x07FF));
	ch[12] = (uint16_t)((buf_[17] >> 4) | ((buf_[18] << 4) & 0x07FF));
	ch[13] = (uint16_t)((buf_[18] >> 7) | (buf_[19] << 1) |
			    ((buf_[20] << 9) & 0x07FF));
	ch[14] = (uint16_t)((buf_[20] >> 2) | ((buf_[21] << 6) & 0x07FF));
	ch[15] = (uint16_t)((buf_[21] >> 5) | ((buf_[22] << 3) & 0x07FF));
}

static void test_channels_match_the_oracle(void)
{
	uint8_t frame[SBUS_FRAME_LEN];
	uint16_t mine[16], theirs[16];

	srand(1);
	for (int round = 0; round < 10000; round++) {
		for (int i = 0; i < SBUS_FRAME_LEN; i++)
			frame[i] = (uint8_t)rand();
		sbus_unpack_channels(frame, mine);
		oracle(frame, theirs);
		assert(!memcmp(mine, theirs, sizeof(mine)));
	}

	/* The ends: all zero, all ones. */
	memset(frame, 0, sizeof(frame));
	sbus_unpack_channels(frame, mine);
	for (int i = 0; i < 16; i++)
		assert(mine[i] == 0);
	memset(frame, 0xFF, sizeof(frame));
	sbus_unpack_channels(frame, mine);
	for (int i = 0; i < 16; i++)
		assert(mine[i] == 0x7FF);
}

/* The header and the footer: 0x00, or a low nibble of 4 (SBUS2). */
static void test_frame_boundaries(void)
{
	static const uint8_t good[] = { 0x00, 0x04, 0x14, 0x24, 0x34, 0xF4 };
	static const uint8_t bad[] = { 0x01, 0x08, 0x0F, 0x40, 0xFF, 0x03 };
	uint8_t frame[SBUS_FRAME_LEN] = { SBUS_HEADER };

	for (size_t i = 0; i < sizeof(good); i++) {
		frame[24] = good[i];
		assert(sbus_frame_ok(frame));
	}
	for (size_t i = 0; i < sizeof(bad); i++) {
		frame[24] = bad[i];
		assert(!sbus_frame_ok(frame));
	}
	frame[24] = 0x00;
	frame[0] = 0x0E;
	assert(!sbus_frame_ok(frame));
}

/* Each flag on its own bit, and nothing else read from byte 23. */
static void test_flags(void)
{
	uint8_t frame[SBUS_FRAME_LEN] = { SBUS_HEADER };
	struct sbus_flags flags;

	frame[23] = 0x00;
	sbus_unpack_flags(frame, &flags);
	assert(!flags.channel_17 && !flags.channel_18 && !flags.frame_lost &&
	       !flags.failsafe);
	frame[23] = 0x01;
	sbus_unpack_flags(frame, &flags);
	assert(flags.channel_17 && !flags.channel_18 && !flags.frame_lost &&
	       !flags.failsafe);
	frame[23] = 0x02;
	sbus_unpack_flags(frame, &flags);
	assert(!flags.channel_17 && flags.channel_18 && !flags.frame_lost &&
	       !flags.failsafe);
	frame[23] = 0x04;
	sbus_unpack_flags(frame, &flags);
	assert(!flags.channel_17 && !flags.channel_18 && flags.frame_lost &&
	       !flags.failsafe);
	frame[23] = 0x08;
	sbus_unpack_flags(frame, &flags);
	assert(!flags.channel_17 && !flags.channel_18 && !flags.frame_lost &&
	       flags.failsafe);
	frame[23] = 0xF0;
	sbus_unpack_flags(frame, &flags);
	assert(!flags.channel_17 && !flags.channel_18 && !flags.frame_lost &&
	       !flags.failsafe);
}

int main(void)
{
	test_channels_match_the_oracle();
	test_frame_boundaries();
	test_flags();
	printf("test_sbus_proto: all passed\n");
	return 0;
}
