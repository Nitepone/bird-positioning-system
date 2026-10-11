// FLAC encoder: exact bytes of a frame checked against libFLAC once, then
// round trips through a small decoder for the subset the encoder writes.
#include <unity.h>

#include <math.h>
#include <string.h>

#include "flac.h"

void setUp(void) {}
void tearDown(void) {}

/* ---- A decoder for what the encoder writes ---- */

typedef struct {
    const uint8_t *p;
    size_t len, bit;
} bitr;

static uint32_t get(bitr *r, unsigned n)
{
    uint32_t v = 0;
    for (unsigned i = 0; i < n; i++, r->bit++) {
        TEST_ASSERT_TRUE_MESSAGE(r->bit / 8 < r->len, "read past the frame");
        v = v << 1 | ((r->p[r->bit / 8] >> (7 - r->bit % 8)) & 1);
    }
    return v;
}

static int32_t sget(bitr *r, unsigned n)
{
    uint32_t v = get(r, n);
    return (int32_t)(v ^ (1u << (n - 1))) - (int32_t)(1u << (n - 1));
}

static uint8_t crc8(const uint8_t *b, size_t n)
{
    uint8_t c = 0;
    while (n--) {
        c ^= *b++;
        for (int k = 0; k < 8; k++)
            c = (uint8_t)(c & 0x80 ? (c << 1) ^ 0x07 : c << 1);
    }
    return c;
}

static uint16_t crc16(const uint8_t *b, size_t n)
{
    uint16_t c = 0;
    while (n--) {
        c ^= (uint16_t)(*b++ << 8);
        for (int k = 0; k < 8; k++)
            c = (uint16_t)(c & 0x8000 ? (c << 1) ^ 0x8005 : c << 1);
    }
    return c;
}

/* Decodes one frame; checks its header, both checksums and its length. */
static void decode_frame(const uint8_t *f, size_t len, uint32_t number, uint32_t n, int16_t *out)
{
    bitr r = {f, len, 0};
    TEST_ASSERT_EQUAL_HEX32(0xfff8, get(&r, 16));
    unsigned bs = get(&r, 4);
    get(&r, 4); /* sample rate code */
    TEST_ASSERT_EQUAL_HEX32(0x08, get(&r, 8));
    uint32_t first = get(&r, 8), num = first;
    if (first >= 0xc0) {
        int extra = first >= 0xfc ? 5 : first >= 0xf8 ? 4 : first >= 0xf0 ? 3 : first >= 0xe0 ? 2 : 1;
        num = first & (0x3f >> extra);
        for (int i = 0; i < extra; i++)
            num = num << 6 | (get(&r, 8) & 0x3f);
    }
    TEST_ASSERT_EQUAL_UINT32(number, num);
    uint32_t size = bs == 10 ? BPS_FLAC_BLOCK : bs == 6 ? get(&r, 8) + 1 : get(&r, 16) + 1;
    TEST_ASSERT_EQUAL_UINT32(n, size);
    uint8_t head_crc = crc8(f, r.bit / 8); /* before reading on: argument order is unspecified */
    TEST_ASSERT_EQUAL_HEX8(head_crc, get(&r, 8));

    TEST_ASSERT_EQUAL_UINT32(0, get(&r, 1));
    unsigned type = get(&r, 6);
    TEST_ASSERT_EQUAL_UINT32(0, get(&r, 1)); /* no wasted bits */
    if (type == 0) {
        int32_t v = sget(&r, 16);
        for (uint32_t i = 0; i < n; i++)
            out[i] = (int16_t)v;
    } else if (type == 1) {
        for (uint32_t i = 0; i < n; i++)
            out[i] = (int16_t)sget(&r, 16);
    } else {
        TEST_ASSERT_EQUAL_UINT32(8, type & 0x38);
        unsigned order = type & 7;
        TEST_ASSERT_LESS_OR_EQUAL_UINT32(4, order);
        int32_t x[BPS_FLAC_BLOCK];
        for (unsigned i = 0; i < order; i++)
            x[i] = sget(&r, 16);
        TEST_ASSERT_EQUAL_UINT32(0, get(&r, 2)); /* 4-bit Rice parameters */
        unsigned porder = get(&r, 4);
        uint32_t psize = n >> porder, i = order;
        for (uint32_t j = 0; j < (1u << porder); j++) {
            unsigned k = get(&r, 4);
            TEST_ASSERT_NOT_EQUAL(15, k); /* never escapes */
            for (; i < (j + 1) * psize; i++) {
                uint32_t q = 0;
                while (!get(&r, 1))
                    q++;
                uint32_t u = q << k | (k ? get(&r, k) : 0);
                int32_t res = (int32_t)(u >> 1) ^ -(int32_t)(u & 1);
                int32_t p = order == 0   ? 0
                            : order == 1 ? x[i - 1]
                            : order == 2 ? 2 * x[i - 1] - x[i - 2]
                            : order == 3 ? 3 * x[i - 1] - 3 * x[i - 2] + x[i - 3]
                                         : 4 * x[i - 1] - 6 * x[i - 2] + 4 * x[i - 3] - x[i - 4];
                x[i] = p + res;
            }
        }
        TEST_ASSERT_EQUAL_UINT32(n, i);
        for (i = 0; i < n; i++)
            out[i] = (int16_t)x[i];
    }
    r.bit = (r.bit + 7) / 8 * 8;
    uint16_t frame_crc = crc16(f, r.bit / 8);
    TEST_ASSERT_EQUAL_HEX16(frame_crc, get(&r, 16));
    TEST_ASSERT_EQUAL_size_t(len, r.bit / 8);
}

static void round_trip(const int16_t *x, uint32_t total, const char *what)
{
    static uint8_t f[BPS_FLAC_MAX_FRAME];
    static int16_t back[BPS_FLAC_BLOCK];
    for (uint32_t i = 0, number = 0; i < total; i += BPS_FLAC_BLOCK, number++) {
        uint32_t n = total - i < BPS_FLAC_BLOCK ? total - i : BPS_FLAC_BLOCK;
        size_t len = bps_flac_frame(f, x + i, n, number, 48000);
        TEST_ASSERT_TRUE_MESSAGE(len > 0 && len <= BPS_FLAC_MAX_FRAME, what);
        decode_frame(f, len, number, n, back);
        TEST_ASSERT_EQUAL_INT16_ARRAY_MESSAGE(x + i, back, n, what);
    }
}

/* ---- Tests ---- */

static uint32_t rng = 1;
static int32_t rnd(void)
{
    rng = rng * 1664525u + 1013904223u;
    return (int32_t)(rng >> 16) - 32768;
}

// Bytes from this encoder that libFLAC and claxon both decoded exactly: a
// 40-sample last frame (8-bit block size) using a second-order predictor.
static void test_matches_bytes_libflac_accepts(void)
{
    static const uint8_t header[BPS_FLAC_HEADER_LEN] = {
        0x66, 0x4c, 0x61, 0x43, 0x80, 0x00, 0x00, 0x22, 0x04, 0x00, 0x04, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x0b, 0xb8, 0x00, 0xf0, 0x00, 0x00, 0x00, 0x28, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    static const uint8_t frame[] = {0xff, 0xf8, 0x6a, 0x08, 0x00, 0x27, 0xd2, 0x14,
                                    0x00, 0x00, 0x00, 0x64, 0x08, 0x3f, 0xc3, 0xff,
                                    0x4a, 0x00, 0x00, 0x33, 0x68, 0x56, 0xd0, 0xad,
                                    0xa0, 0x26, 0x28, 0xc5, 0x18, 0xa3, 0x8c, 0xca};
    int16_t x[40];
    for (int i = 0; i < 40; i++)
        x[i] = (int16_t)(i < 20 ? 100 * i : 2000 - 37 * (i - 20) + (i % 3));
    uint8_t h[BPS_FLAC_HEADER_LEN], f[BPS_FLAC_MAX_FRAME];
    bps_flac_header(h, 48000, 40);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(header, h, sizeof header);
    TEST_ASSERT_EQUAL_size_t(sizeof frame, bps_flac_frame(f, x, 40, 0, 48000));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(frame, f, sizeof frame);
}

static void test_round_trips(void)
{
    static int16_t x[5 * BPS_FLAC_BLOCK + 100]; /* 5 full frames and a short one */
    const uint32_t n = sizeof x / sizeof x[0];
    for (uint32_t i = 0; i < n; i++)
        x[i] = (int16_t)(3000 * sin(i * 0.05) + rnd() / 512);
    round_trip(x, n, "tone");
    for (uint32_t i = 0; i < n; i++)
        x[i] = (int16_t)(rnd() / 2048);
    round_trip(x, n, "quiet noise");
    for (uint32_t i = 0; i < n; i++)
        x[i] = (int16_t)rnd();
    round_trip(x, n, "full-scale noise");
    for (uint32_t i = 0; i < n; i++)
        x[i] = i % 2 ? 32767 : -32768;
    round_trip(x, n, "extremes");
    for (uint32_t i = 0; i < n; i++)
        x[i] = (int16_t)(rnd() / 8192 + (i % 61 == 0 ? 30000 : 0));
    round_trip(x, n, "spikes");
    memset(x, 0, sizeof x);
    round_trip(x, n, "silence");
    round_trip(x, 1, "one sample");
    round_trip(x, 3000, "short last frame");
}

static void test_compresses_quiet_audio(void)
{
    static int16_t x[BPS_FLAC_BLOCK];
    static uint8_t f[BPS_FLAC_MAX_FRAME];
    for (uint32_t i = 0; i < BPS_FLAC_BLOCK; i++)
        x[i] = (int16_t)(rnd() / 2048); /* about 5 bits of noise */
    TEST_ASSERT_LESS_THAN_size_t(BPS_FLAC_BLOCK, bps_flac_frame(f, x, BPS_FLAC_BLOCK, 0, 48000));
    memset(x, 0, sizeof x);
    TEST_ASSERT_LESS_THAN_size_t(16, bps_flac_frame(f, x, BPS_FLAC_BLOCK, 0, 48000));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_matches_bytes_libflac_accepts);
    RUN_TEST(test_round_trips);
    RUN_TEST(test_compresses_quiet_audio);
    return UNITY_END();
}
