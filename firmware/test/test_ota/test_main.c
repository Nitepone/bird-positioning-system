// Over-the-air updates, the portable part (ota.c): the build's identity in the
// registration, the offer in a heartbeat reply, the battery gate, and the
// status a heartbeat reports.
#include <unity.h>

#include <stdio.h>
#include <string.h>

#include "ota.h"

void setUp(void) {}
void tearDown(void) {}

// A heartbeat reply as the server sends it (API 1.2.0), with an offer.
static const char *REPLY_WITH_OFFER =
    "{\"mode\":{\"mode\":\"live\",\"interval_s\":300},\"commands\":[],"
    "\"firmware_offer\":{\"firmware_build_id\":42,"
    "\"firmware_url\":\"/api/v1/client/firmware/42.bin\",\"firmware_size\":1283456,"
    "\"firmware_md5\":\"0123456789abcdef0123456789abcdef\","
    "\"firmware_describe\":\"v1.3.0-2-g3c48fb8\"}}";

static void test_an_offer_is_parsed(void)
{
    bps_ota_offer o;
    TEST_ASSERT_TRUE(bps_ota_offer_parse(REPLY_WITH_OFFER, strlen(REPLY_WITH_OFFER), &o));
    TEST_ASSERT_EQUAL_INT64(42, o.build_id);
    TEST_ASSERT_EQUAL_STRING("/api/v1/client/firmware/42.bin", o.url);
    TEST_ASSERT_EQUAL_INT64(1283456, o.size);
    TEST_ASSERT_EQUAL_STRING("0123456789abcdef0123456789abcdef", o.md5);
    TEST_ASSERT_EQUAL_STRING("v1.3.0-2-g3c48fb8", o.describe);
}

static void test_an_offer_needs_an_id_and_a_path(void)
{
    bps_ota_offer o;
    const char *none = "{\"mode\":{\"mode\":\"live\",\"interval_s\":300}}";
    TEST_ASSERT_FALSE(bps_ota_offer_parse(none, strlen(none), &o));
    const char *null_offer = "{\"firmware_offer\":null}";
    TEST_ASSERT_FALSE(bps_ota_offer_parse(null_offer, strlen(null_offer), &o));
    const char *no_url = "{\"firmware_offer\":{\"firmware_build_id\":42}}";
    TEST_ASSERT_FALSE(bps_ota_offer_parse(no_url, strlen(no_url), &o));
    const char *no_id = "{\"firmware_offer\":{\"firmware_url\":\"/x.bin\"}}";
    TEST_ASSERT_FALSE(bps_ota_offer_parse(no_id, strlen(no_id), &o));
    const char *bad_id =
        "{\"firmware_offer\":{\"firmware_build_id\":0,\"firmware_url\":\"/x.bin\"}}";
    TEST_ASSERT_FALSE(bps_ota_offer_parse(bad_id, strlen(bad_id), &o));
    const char *not_a_path = "{\"firmware_offer\":{\"firmware_build_id\":7,"
                             "\"firmware_url\":\"http://elsewhere/x.bin\"}}";
    TEST_ASSERT_FALSE(bps_ota_offer_parse(not_a_path, strlen(not_a_path), &o));
    // Only the id and url are needed; the rest may be missing.
    const char *bare = "{\"firmware_offer\": {\"firmware_build_id\": 7, "
                       "\"firmware_url\": \"/api/v1/client/firmware/7.bin\"}}";
    TEST_ASSERT_TRUE(bps_ota_offer_parse(bare, strlen(bare), &o));
    TEST_ASSERT_EQUAL_INT64(7, o.build_id);
    TEST_ASSERT_EQUAL_INT64(0, o.size);
    TEST_ASSERT_EQUAL_STRING("", o.md5);
    TEST_ASSERT_EQUAL_STRING("", o.describe);
    // A reply cut short in the offer.
    TEST_ASSERT_FALSE(bps_ota_offer_parse(REPLY_WITH_OFFER, 120, &o));
}

static void test_the_battery_gate(void)
{
    char d[32];
    bps_battery_reading b = {true, 3400, false};
    TEST_ASSERT_FALSE(bps_ota_battery_ok(&b, 50, d, sizeof d));
    TEST_ASSERT_EQUAL_STRING("battery 34%", d);
    // Just under is under; the percent is not rounded up.
    b.centi_percent = 4999;
    TEST_ASSERT_FALSE(bps_ota_battery_ok(&b, 50, d, sizeof d));
    TEST_ASSERT_EQUAL_STRING("battery 49%", d);
    b.centi_percent = 5000;
    TEST_ASSERT_TRUE(bps_ota_battery_ok(&b, 50, d, sizeof d));
    TEST_ASSERT_EQUAL_STRING("", d);
    // Charging: any charge will do.
    b.centi_percent = 500;
    b.charging = true;
    TEST_ASSERT_TRUE(bps_ota_battery_ok(&b, 50, d, sizeof d));
    // No gauge reading (no gauge, or external power): update.
    bps_battery_reading none = {false, 0, false};
    TEST_ASSERT_TRUE(bps_ota_battery_ok(&none, 50, d, sizeof d));
    TEST_ASSERT_TRUE(bps_ota_battery_ok(NULL, 50, d, sizeof d));
    // A threshold of 0 never defers; 100 needs a full battery.
    b.charging = false;
    b.centi_percent = 0;
    TEST_ASSERT_TRUE(bps_ota_battery_ok(&b, 0, d, sizeof d));
    b.centi_percent = 9990;
    TEST_ASSERT_FALSE(bps_ota_battery_ok(&b, 100, d, sizeof d));
    TEST_ASSERT_EQUAL_STRING("battery 99%", d);
}

static void test_the_status_json(void)
{
    char buf[512];
    bps_json j;
    bps_json_init(&j, buf, sizeof buf);
    bps_json_raw(&j, "{");
    bps_ota_status_json(&j, 42, "deferred", "battery 34%");
    bps_json_raw(&j, "}");
    TEST_ASSERT_EQUAL_STRING(
        "{\"firmware_status\":{\"build_id\":42,\"state\":\"deferred\",\"detail\":\"battery 34%\"}}",
        buf);

    // After other members; no detail.
    bps_json_init(&j, buf, sizeof buf);
    bps_json_raw(&j, "{");
    bps_json_i64(&j, "uptime_s", 5);
    bps_ota_status_json(&j, 9, "rolled_back", NULL);
    bps_json_raw(&j, "}");
    TEST_ASSERT_EQUAL_STRING(
        "{\"uptime_s\":5,\"firmware_status\":{\"build_id\":9,\"state\":\"rolled_back\"}}", buf);

    // HTTPUpdate's error text, cut to 120 characters (and escaped).
    char err[200];
    memset(err, 'x', sizeof err - 1);
    err[sizeof err - 1] = 0;
    err[0] = '"';
    bps_json_init(&j, buf, sizeof buf);
    bps_json_raw(&j, "{");
    bps_ota_status_json(&j, 42, "failed", err);
    bps_json_raw(&j, "}");
    TEST_ASSERT_FALSE(j.overflow);
    const char *d = strstr(buf, "\"detail\":\"");
    TEST_ASSERT_NOT_NULL(d);
    d += strlen("\"detail\":\"");
    TEST_ASSERT_EQUAL_STRING_LEN("\\\"", d, 2);
    const char *end = strstr(d + 2, "\"}");
    TEST_ASSERT_NOT_NULL(end);
    TEST_ASSERT_EQUAL_INT(120 + 1, (int)(end - d)); // the quote is escaped
}

static void test_the_build_json(void)
{
    char buf[512];
    bps_json j;
    bps_build b = {"esp32-s3 on xiao-proto-rev0.1", NULL, "3c48fb8", true, "3c48fb8-dirty",
                   "2026-11-12T23:10:00Z", NULL};
    bps_json_init(&j, buf, sizeof buf);
    bps_json_raw(&j, "{");
    bps_json_str(&j, "api_version", "1.2.0");
    bps_build_json(&j, &b);
    bps_json_raw(&j, "}");
    TEST_ASSERT_EQUAL_STRING(
        "{\"api_version\":\"1.2.0\",\"build\":{\"hardware_id\":\"esp32-s3 on xiao-proto-rev0.1\","
        "\"tag\":null,\"commit\":\"3c48fb8\",\"dirty\":true,\"describe\":\"3c48fb8-dirty\","
        "\"built_at\":\"2026-11-12T23:10:00Z\"}}",
        buf);
    // A tag; "" is no tag.
    b.tag = "v1.3.0";
    b.dirty = false;
    b.describe = "v1.3.0";
    bps_json_init(&j, buf, sizeof buf);
    bps_json_raw(&j, "{");
    bps_build_json(&j, &b);
    bps_json_raw(&j, "}");
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"tag\":\"v1.3.0\",\"commit\":\"3c48fb8\",\"dirty\":false"));
    b.tag = "";
    bps_json_init(&j, buf, sizeof buf);
    bps_json_raw(&j, "{");
    bps_build_json(&j, &b);
    bps_json_raw(&j, "}");
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"tag\":null"));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_an_offer_is_parsed);
    RUN_TEST(test_an_offer_needs_an_id_and_a_path);
    RUN_TEST(test_the_battery_gate);
    RUN_TEST(test_the_status_json);
    RUN_TEST(test_the_build_json);
    return UNITY_END();
}
