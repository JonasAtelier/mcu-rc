/*
 * The CRSF wire format, split out so it compiles and can be asserted on the
 * host. Nothing here touches a UART. Ported from esp-crsf-rc's
 * src/crsf_parse.h, which keeps its four-space style here.
 *
 * Sources, each pinned (docs/chips.md lists them with their licences; the
 * GPL ones are facts only, no code is copied):
 *
 *   [TBS]   tbs-crsf-spec crsf.md, commit 4e5fe882
 *   [WIKI]  crsf-wg/crsf wiki, commit b1bd5f1f
 *   [ELRS]  ExpressLRS crsf_protocol.h, tags 3.5.3 and 4.1.0
 *   [BF]    Betaflight src/main/rx/crsf.c, crsf_protocol.h, commit cd8e8386
 *
 * A frame is:
 *
 *   +------+-----+------+-----------------+------+
 *   | sync | len | type |     payload     | crc8 |
 *   +------+-----+------+-----------------+------+
 *      0      1     2      3 .. 1+len         1+len
 *
 * len counts the bytes after itself: type, payload and crc ([TBS]
 * crsf.md:170-172; [WIKI] Message-Format.md:10). So the whole frame on the
 * wire is len + 2 bytes, and the payload is len - 2. The crc covers type and
 * payload, not sync and not len ([TBS] crsf.md:187; [WIKI]
 * Message-Format.md:26).
 *
 * Nothing here has been tested against a real receiver on a wire.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "crsf.h"
#include "rc_priv.h"

/*
 * The first byte is a destination address, not a fixed sync pattern, and a
 * receiving device is told to expect more than one value: 0xC8, the
 * broadcast 0x00, or its own address ([TBS] crsf.md:166-169). Testing it
 * against 0xC8 alone drops valid traffic: EdgeTX emits 0xEE, the
 * transmitter module's address ([TBS] crsf.md:253; [BF]
 * crsf_protocol.h:153).
 */
#define CRSF_ADDR_BROADCAST    0x00    /* [TBS] crsf.md:168, :223 */
#define CRSF_ADDR_FC           0xC8    /* [TBS] crsf.md:167, :245 */
#define CRSF_ADDR_TRANSMITTER  0xEE    /* [TBS] crsf.md:253 */

#define CRSF_TYPE_LINK_STATS   0x14    /* [TBS] crsf.md:484; [BF] crsf_protocol.h:55 */
#define CRSF_TYPE_RC_CHANNELS  0x16    /* [TBS] crsf.md:520; [BF] crsf_protocol.h:56 */

/*
 * A frame is at most 64 bytes all in, so len is 2 to 62 ([TBS]
 * crsf.md:148, :173; [ELRS] 4.1.0 crsf_protocol.h:29).
 *
 * DO NOT RELAX THIS TO 64. The bound is not decoration and it is not a
 * protocol nicety: it is the only thing standing between a byte on the wire
 * and a 64-byte buffer. crsf_frame_size returns len + 2, and src/crsf/crsf.c
 * holds that many bytes in a CRSF_MAX_FRAME array, so a len of 63 or 64
 * overruns it by one or two bytes at the direction of whoever is
 * transmitting.
 */
#define CRSF_MAX_FRAME         64
#define CRSF_LEN_MIN           2       /* type + crc, an empty payload */
#define CRSF_LEN_MAX           (CRSF_MAX_FRAME - 2)

/* 16 channels x 11 bits = 176 bits ([TBS] crsf.md:522; [BF]
 * crsf_protocol.h:113) */
#define CRSF_RC_PAYLOAD        22

/*
 * ExpressLRS 4.0 and later may append an arm-status byte to the channel
 * frame, making the payload 23 ([WIKI] CRSF_FRAMETYPE_RC_CHANNELS_PACKED.md:5,
 * :28; [ELRS] 4.1.0 crsf_protocol.h:203-207, which names the bits of "the
 * byte following crsf_channels_t"). The sources disagree on which direction
 * carries it: the wiki says handset to transmitter module. Either way this
 * driver reads only the first 22 bytes, and accepts a longer payload of any
 * length, as [TBS] crsf.md:177 asks of every frame: "Frame receiver should
 * just ignore extra fields."
 */
#define CRSF_RC_PAYLOAD_ARMED  23

/* [TBS] crsf.md:488-499; [BF] crsf_protocol.h:111. ExpressLRS 4.1.0
 * declares a longer variant with a second downlink RSSI ([ELRS] 4.1.0
 * crsf_protocol.h:408-411), and [TBS] crsf.md:177 says to ignore extra
 * fields, so longer is accepted too. */
#define CRSF_LINK_PAYLOAD      10

/* Whether a first byte is an address this driver will listen to. */
static inline bool crsf_addr_ok(uint8_t addr)
{
    return addr == CRSF_ADDR_FC || addr == CRSF_ADDR_BROADCAST ||
           addr == CRSF_ADDR_TRANSMITTER;
}

/*
 * CRC-8 with polynomial 0xD5, initial value 0, most significant bit first, no
 * reflection and no final xor: CRC-8/DVB-S2.
 *
 * The polynomial is the specification's ([TBS] crsf.md:189; [WIKI]
 * Message-Format.md:26; [ELRS] 4.1.0 crsf_protocol.h:9). The initial value
 * of zero is now cited too: the specification's own reference
 * implementation starts its accumulator at 0 ([TBS] crsf.md:212-217), as
 * Betaflight does ([BF] crsf.c:334). esp-crsf-rc, written without the
 * specification at hand, had to call it inferred.
 *
 * test/test_crsf_proto.c pins the first eight table entries a reader can
 * derive by hand from 0xD5 alone, the specification's own table head
 * ([TBS] crsf.md:195), and the CRC RevEng catalogue's check value, 0xBC for
 * "123456789".
 */
static inline uint8_t crsf_crc8(const uint8_t *p, size_t n)
{
    uint8_t crc = 0;

    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];

        for (int bit = 0; bit < 8; bit++)
            crc = (crc & 0x80u) ? (uint8_t)((crc << 1) ^ 0xD5u)
                                : (uint8_t)(crc << 1);
    }

    return crc;
}

/* How many bytes the frame starting at f claims to occupy, or 0 if f cannot
 * start one at all. Needs the first two bytes to be present and no more. */
static inline size_t crsf_frame_size(const uint8_t *f)
{
    /* The length bound here is the buffer guard. See CRSF_LEN_MAX. */
    if (!crsf_addr_ok(f[0]) || f[1] < CRSF_LEN_MIN || f[1] > CRSF_LEN_MAX)
        return 0;

    return (size_t)f[1] + 2;
}

/* Whether the frame at f survives its CRC. Needs crsf_frame_size(f) bytes
 * present, and is meaningless before crsf_frame_size has returned non-zero. */
static inline bool crsf_crc_ok(const uint8_t *f)
{
    size_t covered = (size_t)f[1] - 1;      /* type and payload, not the crc */

    return crsf_crc8(f + 2, covered) == f[2 + covered];
}

static inline uint8_t crsf_frame_type(const uint8_t *f) { return f[2]; }
static inline const uint8_t *crsf_frame_payload(const uint8_t *f) { return f + 3; }
static inline size_t crsf_frame_payload_len(const uint8_t *f)
{
    return (size_t)f[1] - 2;
}

/*
 * Unpack a 22-byte RC_CHANNELS_PACKED payload into 16 raw channel values.
 *
 * The payload is one continuous bit stream with no padding: channel k
 * occupies bits 11k through 11k+10, least significant bit first, so byte 0
 * supplies bits 0 to 7 with its own LSB as bit 0 ([WIKI]
 * CRSF_FRAMETYPE_RC_CHANNELS_PACKED.md:3, "LSB-first, little endian"; the
 * bit-field order of [TBS] crsf.md:530-548 and [ELRS] 4.1.0
 * crsf_protocol.h:183-201). Note the specification's "Big endian" at
 * crsf.md:175 is about multi-byte fields; the channel words are bit fields.
 *
 * test_crsf_proto.c pins this against two payloads packed by hand, with the
 * bit layout drawn out byte by byte so a reader can check the fixture
 * without running anything.
 */
static inline void crsf_unpack_channels(const uint8_t *payload,
                                        uint16_t raw[CRSF_CHANNELS])
{
    rc_unpack_11bit(payload, raw, CRSF_CHANNELS);
}

/*
 * Unpack a LINK_STATISTICS payload ([TBS] crsf.md:488-499; [ELRS] 4.1.0
 * crsf_protocol.h:394-406; [BF] crsf.c:260-270):
 *
 *   0  uplink RSSI, antenna 1, dBm * -1
 *   1  uplink RSSI, antenna 2, likewise
 *   2  uplink link quality, percent
 *   3  uplink SNR, signed dB
 *   4  active antenna
 *   5  RF mode
 *   6  uplink transmit power, an enum index rather than milliwatts
 *   7  downlink RSSI
 *   8  downlink link quality
 *   9  downlink SNR
 *
 * Both antenna readings are kept. A diversity receiver has two, and the one
 * hearing the radio is often not antenna 1, so reducing them to a single
 * number here would throw away the half that matters.
 *
 * Only the uplink is kept. Downlink statistics describe telemetry going back
 * to the radio, which this driver does not send.
 */
static inline void crsf_unpack_link(const uint8_t *payload,
                                    struct crsf_link_stats *out)
{
    /* RSSI is carried as its own magnitude ("dBm * -1"), so a byte of 85
     * means -85 dBm and a larger number is a weaker signal. Passed through
     * in that form, as every radio screen shows it. */
    out->rssi1        = payload[0];
    out->rssi2        = payload[1];
    out->link_quality = payload[2];
    out->snr_db       = (int8_t)payload[3];
    out->antenna      = payload[4];
}
