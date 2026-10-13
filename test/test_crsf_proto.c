/*
 * Host-side check of the CRSF wire format: the CRC, the frame envelope, the
 * 11-bit channel packing and the raw-to-microseconds map. No hardware, no ESP
 * toolchain -- see test/Makefile. Ported from esp-crsf-rc's test/test_parse.c,
 * in its four-space style; what changed is marked "mcu-rc:".
 *
 * WHAT THIS FILE CAN AND CANNOT PROVE. There was no CRSF specification
 * available when esp-crsf-rc was written, so a fixture built from the same
 * belief about the wire format that the decoder holds would only prove the two
 * agree with each other. Every fixture below is therefore either derived by
 * hand from something outside the code -- the polynomial, the bit layout drawn
 * out byte by byte -- or is a corruption of one that is. None of it proves the
 * belief itself is right. Only a real receiver can do that, and no real
 * receiver has ever been attached to this code.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "crsf_proto.h"

/* ------------------------------------------------------------- the CRC ---- */

/*
 * The first eight entries of the CRC-8 table for polynomial 0xD5, which is to
 * say the CRC of each single byte 0 through 7. These are arithmetic, not
 * recollection: a reader can rebuild the whole column from 0xD5 alone.
 *
 * t[0] is 0, because zero shifted is still zero.
 * t[1] is the polynomial itself, 0xD5: the single set bit walks left through
 *      seven shifts to reach 0x80, and the eighth shift pushes it out and
 *      folds 0xD5 back in.
 * t[2] = (t[1] << 1) ^ 0xD5, since t[1] has its top bit set:
 *        0xD5 << 1 = 0xAA, and 0xAA ^ 0xD5 = 0x7F.
 * t[4] = t[2] << 1 = 0xFE, since 0x7F does not have its top bit set.
 * t[8] = (t[4] << 1) ^ 0xD5 = 0xFC ^ 0xD5 = 0x29.
 *
 * The rest follow from the table being linear over GF(2), t[a ^ b] = t[a] ^
 * t[b], which the next test checks directly:
 *   t[3] = t[1] ^ t[2] = 0xD5 ^ 0x7F = 0xAA
 *   t[5] = t[1] ^ t[4] = 0xD5 ^ 0xFE = 0x2B
 *   t[6] = t[2] ^ t[4] = 0x7F ^ 0xFE = 0x81
 *   t[7] = t[1] ^ t[2] ^ t[4] = 0x2B ^ 0x7F = 0x54
 */
static const uint8_t crc_table_head[8] = {
    0x00, 0xD5, 0x7F, 0xAA, 0xFE, 0x2B, 0x81, 0x54
};

static void test_crc_is_the_polynomial_it_claims(void)
{
    for (uint8_t i = 0; i < 8; i++)
        assert(crsf_crc8(&i, 1) == crc_table_head[i]);

    /* Empty input is the initial value, which is zero. */
    assert(crsf_crc8(NULL, 0) == 0);

    /*
     * A multi-byte case worked by hand rather than by this function. The CRC
     * of {0x16} is t[0x16], and 0x16 = 0x10 ^ 0x04 ^ 0x02, so by linearity
     * t[0x16] = t[0x10] ^ t[0x04] ^ t[0x02]. Continuing the doubling above,
     * t[0x10] = t[0x08] << 1 = 0x29 << 1 = 0x52 (0x29's top bit is clear), so
     * t[0x16] = 0x52 ^ 0xFE ^ 0x7F = 0xAC ^ 0x7F = 0xD3.
     */
    { uint8_t b = 0x16; assert(crsf_crc8(&b, 1) == 0xD3); }

    /*
     * The published check value, and the one constant here that ties this
     * function to a named standard from outside this repo. The CRC RevEng
     * catalogue gives, for CRC-8/DVB-S2:
     *
     *   width=8 poly=0xd5 init=0x00 refin=false refout=false xorout=0x00
     *   check=0xbc residue=0x00 name="CRC-8/DVB-S2"
     *
     * https://reveng.sourceforge.io/crc-catalogue/1-15.htm
     *
     * "123456789" is the catalogue's standard input. An earlier draft of this
     * file recalled the check value as 0xF4 and declined to assert anything;
     * the recollection was simply wrong. Unlike every other assertion here,
     * this one is not the file agreeing with itself, so if only one CRC check
     * survives, keep this one.
     */
    assert(crsf_crc8((const uint8_t *)"123456789", 9) == 0xBC);
}

/*
 * mcu-rc: the specification's own table, its first row, as printed at
 * tbs-crsf-spec crsf.md:195 (commit 4e5fe882). The hand derivation above
 * agrees with its first eight entries; this checks the function against the
 * document rather than against arithmetic.
 */
static void test_crc_matches_the_specification_table(void)
{
    static const uint8_t spec_row0[16] = {
        0x00, 0xD5, 0x7F, 0xAA, 0xFE, 0x2B, 0x81, 0x54,
        0x29, 0xFC, 0x56, 0x83, 0xD7, 0x02, 0xA8, 0x7D
    };

    for (uint8_t i = 0; i < 16; i++)
        assert(crsf_crc8(&i, 1) == spec_row0[i]);
}

static void test_crc_is_linear_over_gf2(void)
{
    /* t[a ^ b] = t[a] ^ t[b] holds for every table of this shape, and is
     * independent of the polynomial, so it catches a transcription slip in
     * the table above without assuming the table is right. */
    for (unsigned a = 0; a < 256; a++) {
        for (unsigned b = 0; b < 256; b++) {
            uint8_t ba = (uint8_t)a, bb = (uint8_t)b, bx = (uint8_t)(a ^ b);

            assert(crsf_crc8(&bx, 1) ==
                   (uint8_t)(crsf_crc8(&ba, 1) ^ crsf_crc8(&bb, 1)));
        }
    }
}

/* ------------------------------------------------- the channel packing ---- */

/*
 * THE BIT LAYOUT, drawn out so the two fixtures below can be checked by eye.
 *
 * The 22-byte payload is one continuous bit stream. Bit 0 is the least
 * significant bit of byte 0, bit 8 is the least significant bit of byte 1, and
 * so on. Channel k takes bits 11k through 11k+10, with bit 11k as its least
 * significant. Sixteen channels come to 176 bits, exactly 22 bytes, with no
 * padding anywhere.
 *
 * Eight channels come to 88 bits, exactly 11 bytes, so any payload whose
 * channels repeat with period 8 has a byte pattern that repeats with period
 * 11. Both fixtures below use that: an 11-byte group written twice.
 *
 * Fixture A alternates channel values 0x7FF and 0x000. Every bit of an odd
 * channel is 0 and every bit of an even channel is 1, so each byte of the
 * group is read straight off which channel owns each of its bits. Bytes are
 * written most significant bit on the left, so bit 0 of a byte is the
 * rightmost digit and the fields run right to left:
 *
 *   byte  stream bits  fields, low bit first        binary     hex
 *   ----  -----------  ---------------------------  ---------  ----
 *    0      0 ..  7    ch0[0..7]                    1111 1111  0xFF
 *    1      8 .. 15    ch0[8..10], ch1[0..4]        0000 0111  0x07
 *    2     16 .. 23    ch1[5..10], ch2[0..1]        1100 0000  0xC0
 *    3     24 .. 31    ch2[2..9]                    1111 1111  0xFF
 *    4     32 .. 39    ch2[10],    ch3[0..6]        0000 0001  0x01
 *    5     40 .. 47    ch3[7..10], ch4[0..3]        1111 0000  0xF0
 *    6     48 .. 55    ch4[4..10], ch5[0]           0111 1111  0x7F
 *    7     56 .. 63    ch5[1..8]                    0000 0000  0x00
 *    8     64 .. 71    ch5[9..10], ch6[0..5]        1111 1100  0xFC
 *    9     72 .. 79    ch6[6..10], ch7[0..2]        0001 1111  0x1F
 *   10     80 .. 87    ch7[3..10]                   0000 0000  0x00
 *
 * This fixture pins the bit order and the channel order at once. Reading the
 * stream most significant bit first, or numbering the channels from the other
 * end, both give a different answer than 2047, 0, 2047, 0.
 */
static const uint8_t fixture_alternating[11] = {
    0xFF, 0x07, 0xC0, 0xFF, 0x01, 0xF0, 0x7F, 0x00, 0xFC, 0x1F, 0x00
};

/*
 * Fixture B sets every channel to 992, the centre stick position. As 11 bits
 * that is 0x3E0, or 011 1110 0000: bits 0 to 4 clear, bits 5 to 9 set, bit 10
 * clear. Same table, now filled in from that pattern instead:
 *
 *   byte  stream bits  fields, low bit first        binary     hex
 *   ----  -----------  ---------------------------  ---------  ----
 *    0      0 ..  7    ch0[0..7]                    1110 0000  0xE0
 *    1      8 .. 15    ch0[8..10], ch1[0..4]        0000 0011  0x03
 *    2     16 .. 23    ch1[5..10], ch2[0..1]        0001 1111  0x1F
 *    3     24 .. 31    ch2[2..9]                    1111 1000  0xF8
 *    4     32 .. 39    ch2[10],    ch3[0..6]        1100 0000  0xC0
 *    5     40 .. 47    ch3[7..10], ch4[0..3]        0000 0111  0x07
 *    6     48 .. 55    ch4[4..10], ch5[0]           0011 1110  0x3E
 *    7     56 .. 63    ch5[1..8]                    1111 0000  0xF0
 *    8     64 .. 71    ch5[9..10], ch6[0..5]        1000 0001  0x81
 *    9     72 .. 79    ch6[6..10], ch7[0..2]        0000 1111  0x0F
 *   10     80 .. 87    ch7[3..10]                   0111 1100  0x7C
 *
 * Where fixture A pins the ordering, this one pins the offsets: a channel
 * boundary out by a bit turns 992 into 496, 1984 or 0x3E0 shifted into a
 * neighbour, none of which survive the check below.
 */
static const uint8_t fixture_centred[11] = {
    0xE0, 0x03, 0x1F, 0xF8, 0xC0, 0x07, 0x3E, 0xF0, 0x81, 0x0F, 0x7C
};

static void payload_from_group(const uint8_t group[11], uint8_t out[CRSF_RC_PAYLOAD])
{
    memcpy(out, group, 11);
    memcpy(out + 11, group, 11);
}

static void test_unpack_alternating_extremes(void)
{
    uint8_t payload[CRSF_RC_PAYLOAD];
    uint16_t raw[CRSF_CHANNELS];

    payload_from_group(fixture_alternating, payload);
    crsf_unpack_channels(payload, raw);

    for (int i = 0; i < CRSF_CHANNELS; i++)
        assert(raw[i] == (i % 2 == 0 ? 2047 : 0));
}

static void test_unpack_every_channel_centred(void)
{
    uint8_t payload[CRSF_RC_PAYLOAD];
    uint16_t raw[CRSF_CHANNELS];

    payload_from_group(fixture_centred, payload);
    crsf_unpack_channels(payload, raw);

    for (int i = 0; i < CRSF_CHANNELS; i++)
        assert(raw[i] == 992);
}

/* An all-zero and an all-ones payload are the two cases where the bit order
 * cannot matter, so they check the range and the channel count rather than the
 * layout: no unpacked value may exceed 11 bits. */
static void test_unpack_stays_inside_eleven_bits(void)
{
    uint8_t payload[CRSF_RC_PAYLOAD];
    uint16_t raw[CRSF_CHANNELS];

    memset(payload, 0x00, sizeof payload);
    crsf_unpack_channels(payload, raw);
    for (int i = 0; i < CRSF_CHANNELS; i++)
        assert(raw[i] == 0);

    memset(payload, 0xFF, sizeof payload);
    crsf_unpack_channels(payload, raw);
    for (int i = 0; i < CRSF_CHANNELS; i++)
        assert(raw[i] == 0x7FF);
}

/* --------------------------------------------------- the frame envelope ---- */

/* Wrap a payload in sync, length and CRC. The CRC comes from crsf_crc8, which
 * the tests above pin against the polynomial, so this seals frames with a
 * function already checked from outside rather than with itself. */
static size_t build_frame(uint8_t *f, uint8_t type, const uint8_t *payload,
                          size_t n)
{
    f[0] = CRSF_ADDR_FC;
    f[1] = (uint8_t)(n + 2);
    f[2] = type;
    memcpy(f + 3, payload, n);
    f[3 + n] = crsf_crc8(f + 2, n + 1);

    return n + 4;
}

static void test_frame_size_rejects_what_cannot_start_a_frame(void)
{
    uint8_t f[CRSF_MAX_FRAME];
    uint8_t payload[CRSF_RC_PAYLOAD];

    payload_from_group(fixture_centred, payload);
    assert(build_frame(f, CRSF_TYPE_RC_CHANNELS, payload, sizeof payload) == 26);
    assert(crsf_frame_size(f) == 26);
    assert(crsf_crc_ok(f));
    assert(crsf_frame_type(f) == CRSF_TYPE_RC_CHANNELS);
    assert(crsf_frame_payload_len(f) == CRSF_RC_PAYLOAD);
    assert(crsf_frame_payload(f) == f + 3);

    /* The first byte is an address, not one fixed sync pattern. A receiving
     * device is told to expect the flight controller's address, a broadcast,
     * or its own; EdgeTX puts the transmitter's address on the wire. Testing
     * against 0xC8 alone would drop valid traffic. */
    f[0] = CRSF_ADDR_BROADCAST;
    assert(crsf_frame_size(f) == 26);
    f[0] = CRSF_ADDR_TRANSMITTER;
    assert(crsf_frame_size(f) == 26);

    f[0] = 0x99;                       /* not an address this driver answers */
    assert(crsf_frame_size(f) == 0);

    build_frame(f, CRSF_TYPE_RC_CHANNELS, payload, sizeof payload);
    f[1] = 1;                          /* shorter than a type plus a crc */
    assert(crsf_frame_size(f) == 0);

    /*
     * Past the top of the range. This is the buffer guard, not a protocol
     * nicety: crsf_frame_size returns len + 2 and the driver reads that many
     * bytes into a CRSF_MAX_FRAME array, so accepting 63 or 64 here would let
     * a byte on the wire overrun it.
     */
    build_frame(f, CRSF_TYPE_RC_CHANNELS, payload, sizeof payload);
    for (unsigned len = CRSF_LEN_MAX + 1; len <= 0xFF; len++) {
        f[1] = (uint8_t)len;
        assert(crsf_frame_size(f) == 0);
    }

    /* The two ends of the legal range are lengths, not errors, and neither
     * exceeds the buffer. */
    build_frame(f, CRSF_TYPE_RC_CHANNELS, payload, sizeof payload);
    f[1] = CRSF_LEN_MIN;
    assert(crsf_frame_size(f) == CRSF_LEN_MIN + 2);
    f[1] = CRSF_LEN_MAX;
    assert(crsf_frame_size(f) == CRSF_MAX_FRAME);
    assert(crsf_frame_size(f) <= CRSF_MAX_FRAME && "never past the buffer");
}

static void test_a_corrupted_frame_fails_its_crc(void)
{
    uint8_t f[CRSF_MAX_FRAME];
    uint8_t payload[CRSF_RC_PAYLOAD];

    payload_from_group(fixture_centred, payload);

    /* Every byte the CRC covers, one bit at a time. Flipping any of them, or
     * the CRC byte itself, must be caught. */
    for (size_t i = 2; i < 26; i++) {
        for (int bit = 0; bit < 8; bit++) {
            build_frame(f, CRSF_TYPE_RC_CHANNELS, payload, sizeof payload);
            f[i] ^= (uint8_t)(1u << bit);
            assert(!crsf_crc_ok(f));
        }
    }
}

/* ------------------------------------------------------ link statistics ---- */

static void test_link_statistics_fields(void)
{
    /* rssi ant1, rssi ant2, lq, snr, active antenna, rf mode, tx power,
     * then the three downlink figures this driver drops. */
    uint8_t payload[CRSF_LINK_PAYLOAD] = { 85, 61, 100, 9, 0, 2, 3, 40, 99, 7 };
    struct crsf_link_stats link;

    crsf_unpack_link(payload, &link);

    /*
     * BOTH antennas, not one. A diversity receiver has two, and the one
     * hearing the radio is often not antenna 1: here antenna 1 is at -85 dBm
     * while antenna 2 is at -61, a difference of 24 dB. A driver that reported
     * only byte 0 would call this link far weaker than it is.
     */
    assert(link.rssi1 == 85);
    assert(link.rssi2 == 61);
    assert(link.antenna == 0);
    assert(link.link_quality == 100);
    assert(link.snr_db == 9);

    payload[4] = 1;                     /* antenna 2 is the active one */
    crsf_unpack_link(payload, &link);
    assert(link.antenna == 1);
    assert(link.rssi1 == 85 && link.rssi2 == 61 && "both survive either way");

    /* RSSI is the positive magnitude the wire carries, not a negated dBm
     * value: 85 means -85 dBm, and the larger number is the weaker signal. */
    assert(link.rssi1 > link.rssi2 && "antenna 1 is the weaker of the two");

    /* SNR is signed on the wire: 0xF7 is -9 dB, not 247. */
    payload[3] = 0xF7;
    crsf_unpack_link(payload, &link);
    assert(link.snr_db == -9);

    /* A dead link reads as zero quality, not as an absent frame. */
    payload[2] = 0;
    crsf_unpack_link(payload, &link);
    assert(link.link_quality == 0);
}

/* ------------------------------------------------ raw to microseconds ---- */

static void test_microsecond_map(void)
{
    /* The three fixed points, exact by construction: 1811 - 172 is 1639 and
     * 2012 - 988 is 1024, so the scale is 1024/1639 with no rounding slack at
     * either end. */
    assert(crsf_us(CRSF_RAW_MIN) == 988);
    assert(crsf_us(CRSF_RAW_MID) == 1500);
    assert(crsf_us(CRSF_RAW_MAX) == 2012);

    /* Monotonic across the whole 11-bit range, and never wrapping. An
     * ExpressLRS receiver can and does send outside 172..1811. */
    for (uint16_t raw = 1; raw <= 0x7FF; raw++)
        assert(crsf_us(raw) >= crsf_us((uint16_t)(raw - 1)));

    assert(crsf_us(0) == 881);
    assert(crsf_us(0x7FF) == 2159);
}

int main(void)
{
    test_crc_is_the_polynomial_it_claims();
    test_crc_matches_the_specification_table();
    test_crc_is_linear_over_gf2();
    test_unpack_alternating_extremes();
    test_unpack_every_channel_centred();
    test_unpack_stays_inside_eleven_bits();
    test_frame_size_rejects_what_cannot_start_a_frame();
    test_a_corrupted_frame_fails_its_crc();
    test_link_statistics_fields();
    test_microsecond_map();
    printf("test_crsf_proto: all passed\n");
    return 0;
}
