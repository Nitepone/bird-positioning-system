// Timestamps for audio recorded with the radio off, between anchors.
#include <unity.h>

#include <string.h>

#include "anchor.h"

void setUp(void) {}
void tearDown(void) {}

static const uint8_t AP[6] = {4, 0xf4, 0x1c, 7, 0x93, 0x52};
static const uint8_t OTHER_AP[6] = {4, 0xf4, 0x1c, 0xb, 0xf1, 0x3a};

#define S BPS_NANOS_PER_SEC
#define EPOCH (1762000000LL * S)

// The board's crystal runs 20 ppm slow at first, warming to 18 ppm slow
// over the hour: its drift changes. The server's clock is the truth.
static double drift(int64_t local) { return -20e-6 + 2e-6 * (double)(local - EPOCH) / (3600.0 * S); }

static int64_t true_server(int64_t local)
{
    // The offset is the integral of the drift: -20e-6 t + 1e-6 t^2 / 3600 s.
    double t = (double)(local - EPOCH);
    return local + 5000000 - (int64_t)(20e-6 * t) + (int64_t)(1e-6 * t * t / (3600.0 * S));
}

// Interpolating linearly between anchors `gap_s` apart misses that curve by
// at most k gap^2 / 4 (k = 1e-6 / 3600 s, the t^2 term), in ns, plus rounding.
static int64_t bound_ns(double gap_s) { return (int64_t)(1e-6 / 3600.0 * gap_s * gap_s / 4 * 1e9) + 50; }

// The AP's clock: 7 ppm fast against the server's, and counting from 2023.
static int64_t true_tsf(int64_t local) { return true_server(local) * 1000007 / 1000000 - EPOCH / 2; }

static bps_anchor anchor_at(int64_t local, uint32_t session, const uint8_t *bssid)
{
    bps_anchor a;
    memset(&a, 0, sizeof a);
    a.local_ns = local;
    a.server_ns = true_server(local);
    a.rate = drift(local);
    if (bssid) {
        a.has_tsf = true;
        memcpy(a.bssid, bssid, 6);
        a.tsf_ns = true_tsf(local);
        a.tsf_rate = (1 + drift(local)) * 1.000007 - 1;
    }
    a.session = session;
    return a;
}

static void test_interpolates_across_the_gap(void)
{
    bps_anchors t;
    bps_anchors_init(&t);
    // A contact of 20 s, then the radio off for 280 s, then the next.
    int64_t a = EPOCH + 600 * S;
    bps_anchor x = anchor_at(a, 1, AP), y = anchor_at(a + 20 * S, 1, AP),
               z = anchor_at(a + 300 * S, 2, AP);
    bps_anchors_push(&t, &x);
    bps_anchors_push(&t, &y);
    int64_t out;
    // Recorded after the radio went off: no anchor after it yet.
    TEST_ASSERT_FALSE(bps_anchors_server(&t, a + 100 * S, &out));
    bps_anchors_push(&t, &z);
    int64_t worst = 0, worst_extrapolated = 0;
    for (int64_t at = a + 20 * S; at <= a + 300 * S; at += 7 * S) {
        TEST_ASSERT_TRUE(bps_anchors_server(&t, at, &out));
        // 20 ppm of drift over 280 s is 5.6 ms; what is left is how far the
        // drift changed (here a steep 2 ppm an hour): microseconds.
        TEST_ASSERT_INT64_WITHIN(bound_ns(280), true_server(at), out);
        worst = bps_abs64(out - true_server(at)) > worst ? bps_abs64(out - true_server(at)) : worst;
        uint8_t b[6];
        TEST_ASSERT_TRUE(bps_anchors_tsf(&t, at, b, &out));
        TEST_ASSERT_EQUAL_MEMORY(AP, b, 6);
        TEST_ASSERT_INT64_WITHIN(bound_ns(280) * 2, true_tsf(at), out);
        // Carrying on with the drift measured before the gap does worse.
        int64_t d = at - y.local_ns;
        int64_t ex = y.server_ns + d + (int64_t)(y.rate * (double)d);
        if (bps_abs64(ex - true_server(at)) > worst_extrapolated)
            worst_extrapolated = bps_abs64(ex - true_server(at));
    }
    TEST_ASSERT_TRUE(worst < bound_ns(280));
    TEST_ASSERT_TRUE(worst_extrapolated > 3 * worst);
    // Within a contact too, and exactly at the anchors.
    TEST_ASSERT_TRUE(bps_anchors_server(&t, a + 5 * S, &out));
    TEST_ASSERT_INT64_WITHIN(bound_ns(20), true_server(a + 5 * S), out);
    TEST_ASSERT_TRUE(bps_anchors_server(&t, a, &out));
    TEST_ASSERT_EQUAL_INT64(x.server_ns, out);
    // Before the first anchor, or after the last: not known.
    TEST_ASSERT_FALSE(bps_anchors_server(&t, a - 1, &out));
    TEST_ASSERT_FALSE(bps_anchors_server(&t, a + 300 * S + 1, &out));
    // Extrapolating the drift from before the gap instead would be off by
    // its change: the gap reports that.
    int64_t gap = bps_anchor_gap_ns(&y, &z);
    TEST_ASSERT_INT64_WITHIN(1000, true_server(z.local_ns) - y.server_ns - 280 * S -
                                       (int64_t)(y.rate * 280.0 * S),
                             gap);
    TEST_ASSERT_TRUE(gap != 0);
    TEST_ASSERT_TRUE(bps_anchor_gap_tsf_ns(&y, &z, &out));
}

static void test_keeps_a_contacts_first_and_latest(void)
{
    bps_anchors t;
    bps_anchors_init(&t);
    int64_t a = EPOCH;
    for (int i = 0; i < 6; i++) { // every 10 s of a contact
        bps_anchor x = anchor_at(a + i * 10 * S, 1, NULL);
        bps_anchors_push(&t, &x);
    }
    TEST_ASSERT_EQUAL_UINT32(2, t.n);
    TEST_ASSERT_EQUAL_INT64(a, bps_anchors_first(&t)->local_ns);
    TEST_ASSERT_EQUAL_INT64(a + 50 * S, bps_anchors_last(&t)->local_ns);
    bps_anchor y = anchor_at(a + 300 * S, 2, NULL);
    bps_anchors_push(&t, &y);
    TEST_ASSERT_EQUAL_UINT32(3, t.n);
    // Not later than the latest: ignored.
    bps_anchor old = anchor_at(a + 100 * S, 3, NULL);
    bps_anchors_push(&t, &old);
    TEST_ASSERT_EQUAL_UINT32(3, t.n);
    // Many contacts: the oldest go.
    for (int i = 0; i < 2 * BPS_ANCHORS; i++) {
        bps_anchor x = anchor_at(a + (400 + i) * S, 10 + (uint32_t)i, NULL);
        bps_anchors_push(&t, &x);
    }
    TEST_ASSERT_EQUAL_UINT32(BPS_ANCHORS, t.n);
    TEST_ASSERT_EQUAL_INT64(a + (400 + BPS_ANCHORS) * S, bps_anchors_first(&t)->local_ns);
}

static void test_tsf_needs_the_same_ap_either_side(void)
{
    bps_anchors t;
    bps_anchors_init(&t);
    bps_anchor x = anchor_at(EPOCH, 1, AP), y = anchor_at(EPOCH + 300 * S, 2, OTHER_AP),
               z = anchor_at(EPOCH + 600 * S, 3, NULL), w = anchor_at(EPOCH + 900 * S, 4, OTHER_AP);
    bps_anchors_push(&t, &x);
    bps_anchors_push(&t, &y);
    bps_anchors_push(&t, &z);
    bps_anchors_push(&t, &w);
    uint8_t b[6];
    int64_t out;
    TEST_ASSERT_FALSE(bps_anchors_tsf(&t, EPOCH + 100 * S, b, &out)); // roamed meanwhile
    TEST_ASSERT_FALSE(bps_anchors_tsf(&t, EPOCH + 400 * S, b, &out)); // no beacons at one end
    TEST_ASSERT_FALSE(bps_anchors_tsf(&t, EPOCH + 700 * S, b, &out));
    TEST_ASSERT_FALSE(bps_anchor_gap_tsf_ns(&x, &y, &out));
    // The server's clock is still known throughout.
    TEST_ASSERT_TRUE(bps_anchors_server(&t, EPOCH + 400 * S, &out));
    TEST_ASSERT_INT64_WITHIN(bound_ns(300), true_server(EPOCH + 400 * S), out);
}

static void test_not_across_an_access_point_restart(void)
{
    bps_anchors t;
    bps_anchors_init(&t);
    bps_anchor x = anchor_at(EPOCH, 1, AP), y = anchor_at(EPOCH + 300 * S, 2, AP);
    y.tsf_ns = 123456789; // the AP restarted while the radio was off: its clock began again
    bps_anchors_push(&t, &x);
    bps_anchors_push(&t, &y);
    uint8_t b[6];
    int64_t out;
    TEST_ASSERT_FALSE(bps_anchors_tsf(&t, EPOCH + 100 * S, b, &out));
    TEST_ASSERT_TRUE(bps_anchors_server(&t, EPOCH + 100 * S, &out));
    // A hundred ppm apart is still a clock running, not a jump.
    bps_anchors_init(&t);
    y = anchor_at(EPOCH + 300 * S, 2, AP);
    y.tsf_ns += 300 * 100 * 1000;
    bps_anchors_push(&t, &x);
    bps_anchors_push(&t, &y);
    TEST_ASSERT_TRUE(bps_anchors_tsf(&t, EPOCH + 100 * S, b, &out));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_interpolates_across_the_gap);
    RUN_TEST(test_keeps_a_contacts_first_and_latest);
    RUN_TEST(test_tsf_needs_the_same_ap_either_side);
    RUN_TEST(test_not_across_an_access_point_restart);
    return UNITY_END();
}
