// Timestamper, noise gate (mirrors
// gate.rs tests, continuous) and the audio ring.
#include <unity.h>

#include <math.h>
#include <string.h>

#include "gate.h"
#include "ring.h"
#include "timestamper.h"

void setUp(void) {}
void tearDown(void) {}

// A sample clock 30 ppm slow; each block's estimate 5 us late plus jitter of
// up to 50 us, now and then 2 ms; 50 ms of audio lost 40 s in.
static int64_t truth_at(uint64_t idx, int64_t lost)
{
    return 1000 * BPS_NANOS_PER_SEC + lost +
           (int64_t)llround((double)idx * 1e9 / 48000 * (1 + 30e-6));
}

static int64_t late_by(uint64_t i)
{
    if (i % 997 == 500)
        return 2 * BPS_NANOS_PER_MS;
    return 5000 + (int64_t)((i * 7919) % 50) * 1000;
}

static void test_timestamper_follows_offset_rate_and_jumps(void)
{
    const uint32_t sr = 48000, block = 256;
    bps_timestamper t;
    bps_timestamper_init(&t, sr);
    int news = 0;
    for (uint64_t i = 0; i < 60 * sr / block; i++) {
        uint64_t idx = i * block;
        int64_t lost = idx >= 40 * sr ? 50 * BPS_NANOS_PER_MS : 0;
        int r = bps_timestamper_push(&t, (uint32_t)idx, truth_at(idx, lost) + late_by(i), block);
        news += r == BPS_TS_NEW;
        double secs = (double)idx / sr;
        // Once settled, the timeline is the truth plus the smallest delay (5 us).
        if ((secs > 10 && secs < 40) || secs > 52) {
            int64_t err = bps_timeline_at(bps_timestamper_model(&t), (uint32_t)idx, sr) -
                          truth_at(idx, lost) - 5000;
            TEST_ASSERT_TRUE_MESSAGE(bps_abs64(err) <= 2000, "timeline off");
            TEST_ASSERT_INT32_WITHIN(500, 30000, t.model.ppb);
        }
    }
    TEST_ASSERT_EQUAL_INT(2, news); // the start, and the lost audio
    TEST_ASSERT_TRUE(t.seg_start > 40 * sr && t.seg_start <= 42 * sr);
    TEST_ASSERT_TRUE(t.scatter_ns < 2000);
}

// ---- gate ----

static uint32_t rng = 1;
static float noise(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return ((float)rng / 4294967295.0f - 0.5f) * 0.02f;
}

static int passes;
static void count(void *ctx, size_t end, const bps_gate_frame *f)
{
    (void)ctx;
    (void)end;
    passes += f->pass;
}

static int16_t buf[48000 * 3];

// Feeds `secs` of noise, plus `extra(i)` per sample; returns the frames that passed.
static int feed(bps_gate *g, float (*extra)(size_t), size_t n)
{
    for (size_t i = 0; i < n; i++) {
        float x = noise() + (extra ? extra(i) : 0.0f);
        buf[i] = (int16_t)lroundf(fmaxf(-1.0f, fminf(1.0f, x)) * 32767.0f);
    }
    passes = 0;
    bps_gate_process(g, buf, n, count, NULL);
    return passes;
}

static float tone_burst(size_t i)
{
    // 0.2 s of 3.5 kHz, 1 s into the buffer
    if (i < 48000 || i >= 48000 + 9600)
        return 0.0f;
    return 0.2f * sinf(2.0f * 3.14159265f * 3500.0f * (float)(i - 48000) / 48000.0f);
}

static float rumble(size_t i)
{
    return i < 48000 ? 0.0f : 0.5f * sinf(2.0f * 3.14159265f * 80.0f * (float)i / 48000.0f);
}

static void test_passes_tone_burst_not_noise(void)
{
    bps_gate_config cfg;
    bps_gate_config_default(&cfg);
    bps_gate g;
    bps_gate_init(&g, &cfg, 48000);
    for (int k = 0; k < 5; k++)
        TEST_ASSERT_EQUAL_INT(0, feed(&g, NULL, sizeof buf / sizeof buf[0]));
    TEST_ASSERT_TRUE(g.has_floor);
    TEST_ASSERT_TRUE(feed(&g, tone_burst, sizeof buf / sizeof buf[0]) > 0);
}

static void test_ignores_low_frequency_rumble(void)
{
    bps_gate_config cfg;
    bps_gate_config_default(&cfg);
    bps_gate g;
    bps_gate_init(&g, &cfg, 48000);
    feed(&g, NULL, sizeof buf / sizeof buf[0]);
    feed(&g, NULL, sizeof buf / sizeof buf[0]);
    TEST_ASSERT_EQUAL_INT(0, feed(&g, rumble, sizeof buf / sizeof buf[0]));
}

static void test_reports_frame_ends(void)
{
    bps_gate_config cfg;
    bps_gate_config_default(&cfg);
    bps_gate g;
    bps_gate_init(&g, &cfg, 48000);
    int16_t z[7000] = {0};
    passes = 0;
    bps_gate_process(&g, z, 7000, count, NULL);
    TEST_ASSERT_EQUAL_UINT32(7000 - 4800, g.in_frame);
}

// ---- ring ----

static bps_ring ring;

static void fill(int16_t *p, size_t n, int16_t from)
{
    for (size_t i = 0; i < n; i++)
        p[i] = (int16_t)(from + (int16_t)i);
}

static bps_timeline line(uint32_t idx, int64_t ts, int32_t ppb)
{
    bps_timeline m = {idx, ts, ppb};
    return m;
}

static void test_ring_reads_back_and_detects_overwrite(void)
{
    TEST_ASSERT_EQUAL_UINT32(3 * BPS_RING_BLOCK, bps_ring_alloc(&ring, 48000, 3 * BPS_RING_BLOCK + 5));
    static int16_t in[1000], out[1000];
    int64_t ts = 5 * BPS_NANOS_PER_SEC;
    bps_timeline m = line(0, ts, 0);
    bps_ring_timeline(&ring, &m, true, 0);
    for (int k = 0; k < 40; k++) { // 40000 samples, more than the 24576 kept
        fill(in, 1000, (int16_t)(k * 1000));
        bps_ring_write(&ring, in, 1000);
    }
    TEST_ASSERT_EQUAL_UINT32(40000, bps_ring_written(&ring));
    TEST_ASSERT_TRUE(bps_ring_read(&ring, 39000, out, 1000));
    TEST_ASSERT_EQUAL_INT16(-26536, out[0]); // (int16_t)39000
    TEST_ASSERT_TRUE(bps_ring_read(&ring, 30000, out, 1000));
    TEST_ASSERT_EQUAL_INT16((int16_t)30999, out[999]);
    TEST_ASSERT_FALSE(bps_ring_read(&ring, 10000, out, 1000)); // overwritten
    TEST_ASSERT_FALSE(bps_ring_read(&ring, 39500, out, 1000)); // not written yet
    // One continuous timeline: one segment, which times every sample.
    int64_t t;
    bool has_next;
    uint32_t next;
    TEST_ASSERT_TRUE(bps_ring_locate(&ring, 24000, &t, &has_next, &next));
    TEST_ASSERT_FALSE(has_next);
    TEST_ASSERT_TRUE(t == ts + 500 * BPS_NANOS_PER_MS);
    // Refining the timeline in place retimes the samples already written.
    m = line(36000, ts + 750 * BPS_NANOS_PER_MS + 3000, 20000);
    bps_ring_timeline(&ring, &m, false, 0);
    TEST_ASSERT_TRUE(bps_ring_locate(&ring, 24000, &t, &has_next, &next));
    TEST_ASSERT_FALSE(has_next);
    TEST_ASSERT_TRUE(t == ts + 500 * BPS_NANOS_PER_MS + 3000 - 5000); // 250 ms * 20 ppm
}

static void test_ring_starts_a_segment_at_a_timeline_jump(void)
{
    bps_ring_alloc(&ring, 48000, 2 * BPS_RING_BLOCK);
    static int16_t in[480];
    int64_t ts = BPS_NANOS_PER_SEC;
    bps_timeline m = line(0, ts, 0);
    bps_ring_timeline(&ring, &m, true, 0);
    for (int k = 0; k < 12; k++)
        bps_ring_write(&ring, in, 480);
    // A jump found once 5760 samples are in, which started at 5280.
    m = line(5280, ts + 111 * BPS_NANOS_PER_MS, 0);
    bps_ring_timeline(&ring, &m, true, 5280);
    int64_t t;
    bool has_next;
    uint32_t next;
    TEST_ASSERT_TRUE(bps_ring_locate(&ring, 100, &t, &has_next, &next));
    TEST_ASSERT_TRUE(has_next);
    TEST_ASSERT_EQUAL_UINT32(5280, next);
    TEST_ASSERT_TRUE(bps_ring_locate(&ring, 5300, &t, &has_next, &next));
    TEST_ASSERT_FALSE(has_next);
    TEST_ASSERT_TRUE(t == ts + 111 * BPS_NANOS_PER_MS + 20 * BPS_NANOS_PER_SEC / 48000);
}

static void test_ring_survives_index_wrap(void)
{
    bps_ring_alloc(&ring, 48000, 2 * BPS_RING_BLOCK);
    ring.written = UINT32_MAX - 1500; // as after ~25 hours
    uint32_t first = UINT32_MAX - 1500;
    bps_timeline m = line(first, BPS_NANOS_PER_SEC, 0);
    bps_ring_timeline(&ring, &m, true, first);
    static int16_t in[1000], out[3000];
    for (int k = 0; k < 3; k++) {
        fill(in, 1000, (int16_t)(k * 1000));
        bps_ring_write(&ring, in, 1000);
    }
    TEST_ASSERT_TRUE(bps_ring_read(&ring, first, out, 3000));
    for (int i = 0; i < 3000; i++)
        TEST_ASSERT_EQUAL_INT16((int16_t)i, out[i]);
    int64_t t;
    bool has_next;
    uint32_t next;
    TEST_ASSERT_TRUE(bps_ring_locate(&ring, first + 2400, &t, &has_next, &next));
    TEST_ASSERT_TRUE(t == BPS_NANOS_PER_SEC + 50 * BPS_NANOS_PER_MS);
    TEST_ASSERT_EQUAL_INT32(3000, bps_idx_diff(bps_ring_written(&ring), first));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_timestamper_follows_offset_rate_and_jumps);
    RUN_TEST(test_passes_tone_burst_not_noise);
    RUN_TEST(test_ignores_low_frequency_rumble);
    RUN_TEST(test_reports_frame_ends);
    RUN_TEST(test_ring_reads_back_and_detects_overwrite);
    RUN_TEST(test_ring_starts_a_segment_at_a_timeline_jump);
    RUN_TEST(test_ring_survives_index_wrap);
    return UNITY_END();
}
