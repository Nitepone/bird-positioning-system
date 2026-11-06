// Trigger and validation settings from server replies, and handing a new
// gate to the capture side.
#include <unity.h>

#include <string.h>

#include "capture.h"
#include "trigger.h"

void setUp(void) {}
void tearDown(void) {}

#define PARSE(fn, s, out) fn(s, strlen(s), out)

static void test_parses_energy_trigger_and_no_validation(void)
{
    const char *reply =
        "{\"mode\":{\"mode\":\"live\",\"interval_s\":300},"
        "\"trigger\":{\"trigger_type\":\"energy\",\"band_low_hz\":1500.0,"
        "\"band_high_hz\":9000.0,\"threshold_db\":6.5,\"frame_ms\":50},"
        "\"validation\":{\"validation_type\":\"none\"}}";
    bps_trigger_config t;
    bps_validation_config v;
    TEST_ASSERT_TRUE(PARSE(bps_trigger_parse, reply, &t));
    TEST_ASSERT_EQUAL_INT(BPS_TRIGGER_ENERGY, t.type);
    TEST_ASSERT_EQUAL_FLOAT(1500.0f, t.gate.band_low_hz);
    TEST_ASSERT_EQUAL_FLOAT(9000.0f, t.gate.band_high_hz);
    TEST_ASSERT_EQUAL_FLOAT(6.5f, t.gate.threshold_db);
    TEST_ASSERT_EQUAL_UINT32(50, t.gate.frame_ms);
    TEST_ASSERT_TRUE(PARSE(bps_validation_parse, reply, &v));
    TEST_ASSERT_EQUAL_INT(BPS_VALIDATION_NONE, v.type);
}

static void test_older_server_registration_gives_the_gate(void)
{
    const char *reply = "{\"udp_timesync_port\":2473,\"heartbeat_interval_s\":5,\"chunk_secs\":9.0,"
                        "\"gate\":{\"band_low_hz\":2000.0,\"band_high_hz\":8000.0,"
                        "\"threshold_db\":12.0,\"frame_ms\":100},\"flac_uploads\":true}";
    bps_trigger_config t;
    bps_validation_config v;
    bps_validation_default(&v);
    TEST_ASSERT_TRUE(PARSE(bps_trigger_parse, reply, &t));
    TEST_ASSERT_EQUAL_INT(BPS_TRIGGER_ENERGY, t.type);
    TEST_ASSERT_EQUAL_FLOAT(12.0f, t.gate.threshold_db);
    TEST_ASSERT_FALSE(PARSE(bps_validation_parse, reply, &v));
    TEST_ASSERT_EQUAL_INT(BPS_VALIDATION_NONE, v.type); // untouched
}

static void test_older_server_heartbeat_reply_has_neither(void)
{
    const char *reply = "{\"mode\":{\"mode\":\"batch\",\"interval_s\":300}}";
    bps_trigger_config t;
    bps_trigger_default(&t);
    t.gate.threshold_db = 3.0f;
    TEST_ASSERT_FALSE(PARSE(bps_trigger_parse, reply, &t));
    TEST_ASSERT_EQUAL_FLOAT(3.0f, t.gate.threshold_db); // untouched
}

static void test_unknown_types_are_kept_apart(void)
{
    const char *reply = "{\"trigger\":{\"trigger_type\":\"spectral\"},"
                        "\"validation\":{\"validation_type\":\"model\",\"threshold\":0.5}}";
    bps_trigger_config t;
    bps_validation_config v;
    TEST_ASSERT_TRUE(PARSE(bps_trigger_parse, reply, &t));
    TEST_ASSERT_EQUAL_INT(BPS_TRIGGER_UNKNOWN, t.type);
    TEST_ASSERT_EQUAL_STRING("spectral", t.name);
    TEST_ASSERT_TRUE(PARSE(bps_validation_parse, reply, &v));
    TEST_ASSERT_EQUAL_INT(BPS_VALIDATION_UNKNOWN, v.type);
    TEST_ASSERT_EQUAL_STRING("model", v.name);
}

static void test_rejects_unusable_energy_settings(void)
{
    bps_trigger_config t;
    TEST_ASSERT_FALSE(PARSE(bps_trigger_parse,
                            "{\"trigger_type\":\"energy\",\"band_low_hz\":9000,"
                            "\"band_high_hz\":8000,\"threshold_db\":6,\"frame_ms\":100}",
                            &t));
    TEST_ASSERT_FALSE(PARSE(bps_trigger_parse, "{\"trigger_type\":\"energy\"}", &t));
}

static void test_heartbeat_json_round_trips(void)
{
    bps_trigger_config t, back;
    bps_validation_config v, vback;
    bps_trigger_default(&t);
    t.gate.threshold_db = 7.25f;
    bps_validation_default(&v);
    char buf[256];
    bps_json j;
    bps_json_init(&j, buf, sizeof buf);
    bps_json_raw(&j, "{");
    bps_trigger_json(&j, &t);
    bps_validation_json(&j, &v);
    bps_json_raw(&j, "}");
    TEST_ASSERT_FALSE(j.overflow);
    TEST_ASSERT_TRUE(bps_trigger_parse(buf, j.len, &back));
    TEST_ASSERT_TRUE(bps_trigger_equal(&t, &back));
    TEST_ASSERT_TRUE(bps_validation_parse(buf, j.len, &vback));
    TEST_ASSERT_TRUE(bps_validation_equal(&v, &vback));
    char d[96];
    bps_trigger_describe(&t, d, sizeof d);
    TEST_ASSERT_EQUAL_STRING("energy, 7.3 dB above the floor, 1000-10000 Hz, 100 ms frames", d);
}

// ---- the capture side takes a new gate ----

static bps_ring ring;
static bps_capture cap;
static int32_t frames[2 * BPS_CAPTURE_MAX_FRAMES];

static void feed(int blocks, uint64_t *t_us)
{
    for (int b = 0; b < blocks; b++) {
        for (int i = 0; i < BPS_CAPTURE_MAX_FRAMES; i++)
            frames[2 * i] = frames[2 * i + 1] = (int32_t)((i * 7919) % 2001 - 1000) << 16;
        *t_us += 5333;
        bps_capture_frames(frames, BPS_CAPTURE_MAX_FRAMES, *t_us);
    }
}

static void test_capture_takes_a_new_gate_at_the_next_block(void)
{
    bps_ring_alloc(&ring, 48000, 64 * BPS_RING_BLOCK);
    bps_gate_config g;
    bps_gate_config_default(&g);
    bps_capture_init(&cap, &ring, 48000, 0, &g);
    uint64_t t = 1000000;
    feed(800, &t); // past the floor's warm-up (30 frames of 100 ms)
    TEST_ASSERT_TRUE(cap.gate.has_floor);
    float floor = cap.gate.floor_db;

    // A new threshold keeps the floor and the frame length.
    bps_gate_config more = g;
    more.threshold_db = 4.0f;
    more.frame_ms = 50;
    TEST_ASSERT_TRUE(bps_capture_set_gate(&cap, &more, true));
    TEST_ASSERT_FALSE(bps_capture_set_gate(&cap, &more, true)); // not taken yet
    TEST_ASSERT_EQUAL_FLOAT(10.0f, cap.gate.threshold_db);
    feed(1, &t);
    TEST_ASSERT_EQUAL_FLOAT(4.0f, cap.gate.threshold_db);
    TEST_ASSERT_EQUAL_UINT32(4800, cap.gate.frame_len);
    TEST_ASSERT_TRUE(cap.gate.has_floor);
    TEST_ASSERT_FLOAT_WITHIN(1.0f, floor, cap.gate.floor_db);

    // A new band starts the floor afresh.
    bps_gate_config band = more;
    band.band_low_hz = 2000.0f;
    TEST_ASSERT_TRUE(bps_capture_set_gate(&cap, &band, false));
    feed(1, &t);
    TEST_ASSERT_FALSE(cap.gate.has_floor);
    TEST_ASSERT_EQUAL_UINT32(4800, cap.gate.frame_len);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_parses_energy_trigger_and_no_validation);
    RUN_TEST(test_older_server_registration_gives_the_gate);
    RUN_TEST(test_older_server_heartbeat_reply_has_neither);
    RUN_TEST(test_unknown_types_are_kept_apart);
    RUN_TEST(test_rejects_unusable_energy_settings);
    RUN_TEST(test_heartbeat_json_round_trips);
    RUN_TEST(test_capture_takes_a_new_gate_at_the_next_block);
    return UNITY_END();
}
