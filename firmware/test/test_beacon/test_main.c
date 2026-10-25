// Following an access point's clock (TSF) from its beacons.
#include <unity.h>

#include "beacon.h"

void setUp(void) {}
void tearDown(void) {}

static const uint8_t AP[6] = {4, 0xf4, 0x1c, 7, 0x93, 0x52};
static const uint8_t OTHER_AP[6] = {4, 0xf4, 0x1c, 0xb, 0xf1, 0x3a};

static uint32_t rng = 7;
static int64_t noise_ns(int64_t amplitude)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return (int64_t)(rng % (uint32_t)(2 * amplitude + 1)) - amplitude;
}

// The AP's clock runs 21 ppm fast against the board's; its TSF was 5 s at mono 1 s.
static int64_t ap_tsf_ns(int64_t mono_ns)
{
    return 5 * BPS_NANOS_PER_SEC + (mono_ns - BPS_NANOS_PER_SEC) +
           (mono_ns - BPS_NANOS_PER_SEC) * 21 / 1000000;
}

// Beacons every 102.4 ms of TSF, a third of them missed, heard 0.5 us late at most.
static int64_t feed(bps_beacon *b, const uint8_t *bssid, int64_t from_mono, int64_t secs,
                    int64_t tsf_shift)
{
    int64_t mono = from_mono;
    for (int k = 0; mono < from_mono + secs * BPS_NANOS_PER_SEC; k++) {
        mono += 102400000 - 2150; // 102.4 ms of TSF is that much less on the board
        if (k % 3 == 1)
            continue;
        int64_t tsf = ap_tsf_ns(mono) + tsf_shift;
        int64_t heard = mono + noise_ns(500);
        if (k == 150)
            heard += 300000; // a beacon stamped very late
        bps_beacon_push(b, bssid, (uint64_t)(tsf / 1000), heard);
    }
    return mono;
}

static void test_follows_the_access_points_clock(void)
{
    bps_beacon b;
    bps_beacon_init(&b);
    int64_t start = BPS_NANOS_PER_SEC;
    TEST_ASSERT_FALSE(bps_beacon_ready(&b, start));
    int64_t now = feed(&b, AP, start, 20, 0);
    TEST_ASSERT_TRUE(bps_beacon_ready(&b, now));
    // Within a microsecond (TSF itself is in whole microseconds), 10 s either side.
    for (int64_t t = now - 10 * BPS_NANOS_PER_SEC; t <= now + 10 * BPS_NANOS_PER_SEC;
         t += BPS_NANOS_PER_SEC)
        TEST_ASSERT_INT64_WITHIN(1000, ap_tsf_ns(t), bps_beacon_tsf_at(&b, t));
    TEST_ASSERT_DOUBLE_WITHIN(0.5e-6, 21e-6, b.rate);
    TEST_ASSERT_TRUE(b.scatter_ns < 1000);
    TEST_ASSERT_TRUE(b.fit_points < BPS_BEACON_POINTS); // the late one left out
    TEST_ASSERT_FALSE(bps_beacon_ready(&b, now + 6 * BPS_NANOS_PER_SEC));
    char s[18];
    bps_beacon_bssid(&b, s);
    TEST_ASSERT_EQUAL_STRING("04:f4:1c:07:93:52", s);
}

static void test_starts_again_on_a_new_clock(void)
{
    bps_beacon b;
    bps_beacon_init(&b);
    int64_t now = feed(&b, AP, BPS_NANOS_PER_SEC, 10, 0);
    uint32_t restarts = b.restarts;
    // The AP restarted: its TSF is a minute behind.
    now = feed(&b, AP, now, 10, -60 * BPS_NANOS_PER_SEC);
    TEST_ASSERT_EQUAL_UINT32(restarts + 1, b.restarts);
    TEST_ASSERT_INT64_WITHIN(1000, ap_tsf_ns(now) - 60 * BPS_NANOS_PER_SEC,
                             bps_beacon_tsf_at(&b, now));
    // Roaming to another AP: its own clock.
    feed(&b, OTHER_AP, now, 1, BPS_NANOS_PER_SEC);
    TEST_ASSERT_EQUAL_UINT32(restarts + 2, b.restarts);
    TEST_ASSERT_FALSE(b.has_fit); // not long enough yet
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_follows_the_access_points_clock);
    RUN_TEST(test_starts_again_on_a_new_clock);
    return UNITY_END();
}
