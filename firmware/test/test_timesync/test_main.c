// Mirrors the tests in bsp-proto's timesync.rs and bsp-client's timesync.rs.
#include <unity.h>

#include <string.h>

#include "timesync.h"

void setUp(void) {}
void tearDown(void) {}

static void test_roundtrip(void)
{
    bps_ts_request req = {.seq = 7, .t1 = 123456789};
    for (int i = 0; i < 16; i++)
        req.client_id[i] = (uint8_t)(i * 17);
    uint8_t b[BPS_TS_REQUEST_LEN];
    bps_ts_request_encode(&req, b);
    bps_ts_request back;
    TEST_ASSERT_TRUE(bps_ts_request_decode(b, sizeof b, &back));
    TEST_ASSERT_EQUAL_UINT32(7, back.seq);
    TEST_ASSERT_EQUAL_MEMORY(req.client_id, back.client_id, 16);
    TEST_ASSERT_TRUE(back.t1 == req.t1);

    bps_ts_response resp = {.seq = 7, .t1 = 1, .t2 = 2, .t3 = 3}, r2;
    uint8_t c[BPS_TS_RESPONSE_LEN];
    bps_ts_response_encode(&resp, c);
    TEST_ASSERT_TRUE(bps_ts_response_decode(c, sizeof c, &r2));
    TEST_ASSERT_TRUE(r2.t1 == 1 && r2.t2 == 2 && r2.t3 == 3 && r2.seq == 7);
    TEST_ASSERT_FALSE(bps_ts_response_decode(b, sizeof b, &r2));
    TEST_ASSERT_FALSE(bps_ts_request_decode(b, sizeof b - 1, &back));
}

static void test_offset_math(void)
{
    // Server is 5 ms ahead of client; one-way delay 1 ms each way; 0.2 ms processing.
    int64_t off = 5000000, t1 = 1000000000;
    int64_t t2 = t1 + 1000000 + off, t3 = t2 + 200000, t4 = t3 - off + 1000000;
    bps_ts_response r = {0, t1, t2, t3};
    int64_t o, rtt;
    bps_ts_offset_rtt(&r, t4, &o, &rtt);
    TEST_ASSERT_TRUE(o == off);
    TEST_ASSERT_TRUE(rtt == 2000000);
}

static void test_rejects_impossible_replies(void)
{
    bps_ts_response r = {0, 1000, 5000, 5100};
    bps_ts_sample s;
    TEST_ASSERT_TRUE(bps_ts_sample_of(&r, 2000, &s));
    TEST_ASSERT_FALSE(bps_ts_sample_of(&r, 500, &s)); // client clock stepped back
    r.t3 = 4000;
    TEST_ASSERT_FALSE(bps_ts_sample_of(&r, 2000, &s)); // server clock stepped back
}

// Every fourth measurement has a fast symmetric round trip; the rest are
// delayed (asymmetrically) on the way back.
static bps_ts_sample measure(int64_t i, int64_t offset_ns, double drift_ppm)
{
    int64_t at = 1000 * BPS_NANOS_PER_SEC + i * BPS_NANOS_PER_SEC / 4;
    int64_t truth = offset_ns + (int64_t)(drift_ppm * 1e-6 * ((double)at - 1e12));
    int64_t rtt = 200000, extra_back = 0;
    if (i % 4 != 0) {
        rtt = 200000 + (i % 7) * 1000000;
        extra_back = (i % 7) * 1000000;
    }
    bps_ts_sample s = {at, truth - extra_back / 2, rtt};
    return s;
}

static bps_estimator est;

static void test_fits_offset_and_drift_through_fast_round_trips(void)
{
    bps_est_init(&est, 64 * BPS_NANOS_PER_SEC, 16);
    const bps_ts_fit *f = NULL;
    for (int i = 0; i < 400; i++)
        f = bps_est_push(&est, measure(i, 3000000, 20.0));
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_DOUBLE_WITHIN(0.5, 20.0, bps_fit_drift_ppm(f));
    int64_t at = 1000 * BPS_NANOS_PER_SEC + 399 * BPS_NANOS_PER_SEC / 4;
    int64_t truth = 3000000 + (int64_t)(20e-6 * ((double)at - 1e12));
    TEST_ASSERT_TRUE(bps_abs64(bps_fit_offset_at(f, at) - truth) < 20000);
    TEST_ASSERT_TRUE(f->rtt_ns == 200000);
    TEST_ASSERT_TRUE(f->error_ns >= 100000 && f->error_ns < 150000);
}

static void test_starts_afresh_after_a_clock_step(void)
{
    bps_est_init(&est, 64 * BPS_NANOS_PER_SEC, 16);
    for (int i = 0; i < 200; i++)
        bps_est_push(&est, measure(i, 0, 0.0));
    const bps_ts_fit *f = NULL;
    for (int i = 200; i < 260; i++) // the client clock steps 5 ms back
        f = bps_est_push(&est, measure(i, 5000000, 0.0));
    TEST_ASSERT_DOUBLE_WITHIN(20000.0, 5e6, f->offset_ns);
    TEST_ASSERT_DOUBLE_WITHIN(1.0, 0.0, bps_fit_drift_ppm(f));
}

static void test_a_single_outlier_is_not_a_step(void)
{
    bps_est_init(&est, 64 * BPS_NANOS_PER_SEC, 16);
    for (int i = 0; i < 200; i++)
        bps_est_push(&est, measure(i, 0, 0.0));
    bps_ts_sample odd = measure(200, 0, 0.0);
    odd.offset_ns += 50000000;
    bps_est_push(&est, odd);
    const bps_ts_fit *f = bps_est_push(&est, measure(201, 0, 0.0));
    TEST_ASSERT_DOUBLE_WITHIN(20000.0, 0.0, f->offset_ns);
}

static bps_clock clk;
static const int64_t NOW = 1700000000LL * BPS_NANOS_PER_SEC;

static void feed(bps_clock *c, int64_t offset_ns)
{
    for (int i = 0; i < 40; i++) {
        bps_ts_sample s = {NOW - (40 - i) * BPS_NANOS_PER_SEC / 2, offset_ns, 400000};
        bps_clock_add_sample(c, s);
    }
}

static void test_corrects_chunks_and_reports_what_is_left(void)
{
    bps_clock_init(&clk, true);
    // No estimate yet: the chunk goes out uncorrected and says so.
    TEST_ASSERT_TRUE(bps_clock_correct(&clk, 1000, BPS_NANOS_PER_SEC) == 1000);
    bps_clock_status st;
    TEST_ASSERT_FALSE(bps_clock_status_take(&clk, NOW, &st));
    feed(&clk, 3000000);
    TEST_ASSERT_TRUE(bps_clock_status_take(&clk, NOW, &st));
    TEST_ASSERT_TRUE(st.offset_ns == 3000000);
    TEST_ASSERT_TRUE(st.error_ns >= 3000000);
    TEST_ASSERT_EQUAL_UINT32(40, st.replies);

    TEST_ASSERT_TRUE(bps_clock_correct(&clk, NOW, 9 * BPS_NANOS_PER_SEC) == NOW + 3000000);
    TEST_ASSERT_TRUE(bps_clock_status_take(&clk, NOW, &st));
    TEST_ASSERT_TRUE(st.offset_ns == 0);
    TEST_ASSERT_TRUE(st.error_ns == 200000);
    TEST_ASSERT_TRUE(st.clock_offset_ns == 3000000);
    TEST_ASSERT_EQUAL_UINT32(0, st.replies); // counts restart each heartbeat
    TEST_ASSERT_FALSE(st.has_rtts);
}

static void test_trusting_the_os_clock_reports_the_raw_offset(void)
{
    bps_clock_init(&clk, false);
    feed(&clk, -1500000);
    TEST_ASSERT_TRUE(bps_clock_correct(&clk, NOW, BPS_NANOS_PER_SEC) == NOW);
    bps_clock_status st;
    TEST_ASSERT_TRUE(bps_clock_status_take(&clk, NOW, &st));
    TEST_ASSERT_TRUE(st.offset_ns == -1500000);
    TEST_ASSERT_TRUE(st.error_ns == 1700000);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_roundtrip);
    RUN_TEST(test_offset_math);
    RUN_TEST(test_rejects_impossible_replies);
    RUN_TEST(test_fits_offset_and_drift_through_fast_round_trips);
    RUN_TEST(test_starts_afresh_after_a_clock_step);
    RUN_TEST(test_a_single_outlier_is_not_a_step);
    RUN_TEST(test_corrects_chunks_and_reports_what_is_left);
    RUN_TEST(test_trusting_the_os_clock_reports_the_raw_offset);
    return UNITY_END();
}
