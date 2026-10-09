// HTTP framing, JSON, WAV header and UUIDs.
#include <unity.h>

#include <string.h>

#include "proto.h"

void setUp(void) {}
void tearDown(void) {}

static void test_http_head(void)
{
    char b[256];
    size_t n = bps_http_head(b, sizeof b, "POST", "10.0.0.2", 2473, "/api/v1/client/x/audio",
                             "audio/wav", 1234, "x-bsp-start-ns: 42\r\n");
    TEST_ASSERT_EQUAL_STRING("POST /api/v1/client/x/audio HTTP/1.1\r\nHost: 10.0.0.2:2473\r\n"
                             "Connection: close\r\nContent-Type: audio/wav\r\n"
                             "Content-Length: 1234\r\nx-bsp-start-ns: 42\r\n\r\n",
                             b);
    TEST_ASSERT_EQUAL_size_t(strlen(b), n);
    n = bps_http_head(b, sizeof b, "POST", "h", 1, "/p", NULL, 0, NULL);
    TEST_ASSERT_EQUAL_STRING("POST /p HTTP/1.1\r\nHost: h:1\r\nConnection: close\r\n"
                             "Content-Length: 0\r\n\r\n",
                             b);
    TEST_ASSERT_EQUAL_size_t(0, bps_http_head(b, 20, "POST", "h", 1, "/p", NULL, 0, NULL));
}

static void test_http_parse(void)
{
    const char *r = "HTTP/1.1 200 OK\r\ncontent-type: application/json\r\nCONTENT-LENGTH: 7\r\n\r\n{\"a\":1}";
    bps_http_response resp;
    TEST_ASSERT_FALSE(bps_http_parse(r, 30, &resp));
    TEST_ASSERT_TRUE(bps_http_parse(r, strlen(r), &resp));
    TEST_ASSERT_EQUAL_INT(200, resp.status);
    TEST_ASSERT_EQUAL_INT(7, resp.content_length);
    TEST_ASSERT_EQUAL_STRING("{\"a\":1}", r + resp.body_off);
    const char *nc = "HTTP/1.1 204 No Content\r\ndate: x\r\n\r\n";
    TEST_ASSERT_TRUE(bps_http_parse(nc, strlen(nc), &resp));
    TEST_ASSERT_EQUAL_INT(204, resp.status);
    TEST_ASSERT_EQUAL_INT(-1, resp.content_length);
}

static void test_json_writer(void)
{
    char b[200];
    bps_json j;
    bps_json_init(&j, b, sizeof b);
    bps_json_raw(&j, "{");
    bps_json_key(&j, "s");
    bps_json_str(&j, "a\"b\\c\n");
    bps_json_key(&j, "min");
    bps_json_i64(&j, INT64_MIN);
    bps_json_key(&j, "o");
    bps_json_raw(&j, "{");
    bps_json_key(&j, "f");
    bps_json_fixed(&j, -0.00004, 4);
    bps_json_key(&j, "g");
    bps_json_fixed(&j, -12.34567, 4);
    bps_json_raw(&j, "}");
    bps_json_key(&j, "t");
    bps_json_bool(&j, true);
    bps_json_raw(&j, "}");
    TEST_ASSERT_FALSE(j.overflow);
    TEST_ASSERT_EQUAL_STRING("{\"s\":\"a\\\"b\\\\c\\u000a\",\"min\":-9223372036854775808,"
                             "\"o\":{\"f\":0.0000,\"g\":-12.3457},\"t\":true}",
                             b);
    bps_json_init(&j, b, 4);
    bps_json_raw(&j, "toolong");
    TEST_ASSERT_TRUE(j.overflow);
}

static void test_json_number(void)
{
    const char *r = "{\"udp_timesync_port\":2473,\"heartbeat_interval_s\":5,\"chunk_secs\":9.0,"
                    "\"gate\":{\"band_low_hz\":1000.0,\"band_high_hz\":10000.0,\"threshold_db\":10.0,"
                    "\"frame_ms\":100}}";
    double v;
    TEST_ASSERT_TRUE(bps_json_number(r, strlen(r), "udp_timesync_port", &v));
    TEST_ASSERT_EQUAL_DOUBLE(2473, v);
    TEST_ASSERT_TRUE(bps_json_number(r, strlen(r), "chunk_secs", &v));
    TEST_ASSERT_EQUAL_DOUBLE(9.0, v);
    TEST_ASSERT_TRUE(bps_json_number(r, strlen(r), "frame_ms", &v));
    TEST_ASSERT_EQUAL_DOUBLE(100, v);
    TEST_ASSERT_FALSE(bps_json_number(r, strlen(r), "gate", &v));
    TEST_ASSERT_FALSE(bps_json_number(r, strlen(r), "missing", &v));
}

static void test_wav_header(void)
{
    uint8_t h[BPS_WAV_HEADER_LEN];
    bps_wav_header(h, 48000, 432000);
    TEST_ASSERT_EQUAL_MEMORY("RIFF", h, 4);
    TEST_ASSERT_EQUAL_MEMORY("WAVEfmt ", h + 8, 8);
    TEST_ASSERT_EQUAL_MEMORY("data", h + 36, 4);
    uint32_t data = h[40] | h[41] << 8 | h[42] << 16 | (uint32_t)h[43] << 24;
    TEST_ASSERT_EQUAL_UINT32(864000, data);
    uint32_t rate = h[24] | h[25] << 8 | h[26] << 16 | (uint32_t)h[27] << 24;
    TEST_ASSERT_EQUAL_UINT32(48000, rate);
}

static void test_uuids(void)
{
    const uint8_t chip[8] = {0xe6, 0x61, 0x38, 0x52, 0x83, 0x2b, 0x4c, 0x2d};
    uint8_t id[16], back[16];
    char s[37];
    bps_uuid_from_chip(chip, id);
    bps_uuid_format(id, s);
    TEST_ASSERT_EQUAL_STRING("e6613852-832b-84c2-8d62-70732d6d6375", s);
    TEST_ASSERT_EQUAL_HEX8(0x80, id[6] & 0xf0); // version 8
    TEST_ASSERT_EQUAL_HEX8(0x80, id[8] & 0xc0); // variant
    TEST_ASSERT_TRUE(bps_uuid_parse(s, back));
    TEST_ASSERT_EQUAL_MEMORY(id, back, 16);
    TEST_ASSERT_FALSE(bps_uuid_parse("not-a-uuid", back));
    // All 64 chip bits survive: a different last nibble gives a different id.
    uint8_t chip2[8];
    memcpy(chip2, chip, 8);
    chip2[7] ^= 1;
    bps_uuid_from_chip(chip2, back);
    TEST_ASSERT_TRUE(memcmp(id, back, 16) != 0);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_http_head);
    RUN_TEST(test_http_parse);
    RUN_TEST(test_json_writer);
    RUN_TEST(test_json_number);
    RUN_TEST(test_wav_header);
    RUN_TEST(test_uuids);
    return UNITY_END();
}
