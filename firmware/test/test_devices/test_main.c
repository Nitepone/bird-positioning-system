// Peripherals: the device list, the MAX17048 gauge and the storage card's
// report, and the copy of recordings on the card (with the board layer faked).
#include <unity.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "devices.h"
#include "port.h"
#include "record.h"

// ---- Fake board ----

void port_log(const char *fmt, ...)
{
    (void)fmt;
}

// A MAX17048 at 0x36 while `gauge_on`: 16-bit registers, read big-endian.
static bool gauge_on;
static uint16_t regs[256];

bool port_i2c_begin(int sda, int scl, uint32_t hz)
{
    (void)hz;
    return sda >= 0 && scl >= 0;
}

bool port_i2c_xfer(uint8_t addr, const uint8_t *tx, size_t tx_len, uint8_t *rx, size_t rx_len)
{
    if (!gauge_on || addr != 0x36 || tx_len != 1 || rx_len != 2)
        return false;
    rx[0] = (uint8_t)(regs[tx[0]] >> 8);
    rx[1] = (uint8_t)regs[tx[0]];
    return true;
}

// A card that keeps a log of what is written to it.
static port_sd_info card;
static bool sd_started;
static char written[4096];
static size_t writes;

bool port_sd_begin(int sck, int miso, int mosi, int cs)
{
    (void)sck, (void)miso, (void)mosi, (void)cs;
    sd_started = true;
    return true;
}

void port_sd_info_get(port_sd_info *out) { *out = card; }

bool port_sd_write(const char *path, const void *buf, size_t len, bool append)
{
    if (!card.present)
        return false;
    writes++;
    size_t n = strlen(written);
    snprintf(written + n, sizeof written - n, "%s %s %u\n", append ? "append" : "create", path,
             (unsigned)len);
    if (strstr(path, "index.jsonl")) {
        n = strlen(written);
        snprintf(written + n, sizeof written - n, "%.*s", (int)len, (const char *)buf);
    }
    return true;
}

void port_sd_close(void)
{
    size_t n = strlen(written);
    snprintf(written + n, sizeof written - n, "close\n");
}

void setUp(void)
{
    gauge_on = false;
    memset(regs, 0, sizeof regs);
    regs[0x08] = 0x0012; // VERSION
    memset(&card, 0, sizeof card);
    written[0] = 0;
    writes = 0;
}

void tearDown(void) {}

// ---- The device list ----

static void test_parses_device_lists(void)
{
    bps_device d[BPS_MAX_DEVICES];
    char err[64];
    TEST_ASSERT_EQUAL_INT(2, bps_devices_parse("max17048@0x36 sd@2", d, BPS_MAX_DEVICES, err,
                                               sizeof err));
    TEST_ASSERT_EQUAL_STRING("max17048", d[0].drv->kind);
    TEST_ASSERT_EQUAL_INT(0x36, d[0].address);
    TEST_ASSERT_EQUAL_STRING("sd", d[1].drv->kind);
    TEST_ASSERT_EQUAL_INT(2, d[1].address);
    // The usual address when left out; commas work too; SPI may leave it to the board.
    TEST_ASSERT_EQUAL_INT(2, bps_devices_parse(" max17048, sd ", d, BPS_MAX_DEVICES, err,
                                               sizeof err));
    TEST_ASSERT_EQUAL_INT(0x36, d[0].address);
    TEST_ASSERT_EQUAL_INT(-1, d[1].address);
    TEST_ASSERT_EQUAL_INT(0, bps_devices_parse("", d, BPS_MAX_DEVICES, err, sizeof err));
    TEST_ASSERT_EQUAL_INT(0, bps_devices_parse(NULL, d, BPS_MAX_DEVICES, err, sizeof err));
    TEST_ASSERT_EQUAL_INT(-1, bps_devices_parse("bme280@0x76", d, BPS_MAX_DEVICES, err,
                                                sizeof err));
    TEST_ASSERT_EQUAL_STRING("unknown device \"bme280\"", err);
    TEST_ASSERT_EQUAL_INT(-1, bps_devices_parse("max17048@x", d, BPS_MAX_DEVICES, err,
                                                sizeof err));
    TEST_ASSERT_EQUAL_INT(-1, bps_devices_parse("sd@2 sd@3", d, 1, err, sizeof err));
}

// ---- Heartbeats ----

static char hb[1024];

static const char *heartbeat(void)
{
    bps_json j;
    bps_json_init(&j, hb, sizeof hb);
    bps_json_raw(&j, "{");
    bps_devices_heartbeat(&j);
    bps_json_raw(&j, "}");
    TEST_ASSERT_FALSE(j.overflow);
    return hb;
}

static const bps_bus_settings BUSES = {5, 6, 7, 8, 9, "max17048@0x36 sd@2"};

static void test_reports_the_battery_and_card(void)
{
    gauge_on = true;
    regs[0x02] = 51200;  // VCELL: 51200 * 78.125 uV = 4.000 V
    regs[0x04] = 0x5780; // SOC: 87.5 %
    regs[0x16] = 0xfffb; // CRATE: -5 * 0.208 = -1.04 %/h
    card = (port_sd_info){.present = true, .total_bytes = 32ull << 30, .free_bytes = 1ull << 30};
    bps_devices_setup(&BUSES);
    TEST_ASSERT_TRUE(sd_started);
    bps_devices_poll(0);
    TEST_ASSERT_EQUAL_STRING(
        "{\"battery\":{\"percent\":87.50,\"voltage_v\":4.000,\"rate_pct_per_h\":-1.04},"
        "\"storage\":{\"present\":true,\"total_bytes\":34359738368,\"free_bytes\":1073741824,"
        "\"recordings\":0,\"recorded_bytes\":0,\"errors\":0,\"dropped_bytes\":0},"
        "\"devices\":[{\"kind\":\"max17048\",\"bus\":\"i2c\",\"address\":54,\"present\":true},"
        "{\"kind\":\"sd\",\"bus\":\"spi\",\"address\":2,\"present\":true}]}",
        heartbeat());
}

static void test_reports_no_battery_and_an_empty_slot(void)
{
    bps_devices_setup(&BUSES);
    bps_devices_poll(0);
    TEST_ASSERT_EQUAL_STRING(
        "{\"storage\":{\"present\":false,\"recordings\":0,\"recorded_bytes\":0,\"errors\":0,"
        "\"dropped_bytes\":0},"
        "\"devices\":[{\"kind\":\"max17048\",\"bus\":\"i2c\",\"address\":54,\"present\":false},"
        "{\"kind\":\"sd\",\"bus\":\"spi\",\"address\":2,\"present\":false}]}",
        heartbeat());
    // A battery connected later is found at the next poll.
    gauge_on = true;
    regs[0x04] = 50 << 8;
    bps_devices_poll(10000000);
    TEST_ASSERT_NOT_NULL(strstr(heartbeat(), "\"battery\":{\"percent\":50.00,"));
}

static void test_leaves_out_i2c_devices_without_a_bus(void)
{
    gauge_on = true;
    bps_bus_settings b = BUSES;
    b.i2c_sda = -1;
    bps_devices_setup(&b);
    bps_devices_poll(0);
    TEST_ASSERT_NULL(strstr(heartbeat(), "\"battery\""));
    TEST_ASSERT_NOT_NULL(strstr(hb, "\"kind\":\"max17048\",\"bus\":\"i2c\",\"address\":54,\"present\":false"));
}

static void test_no_devices_no_report(void)
{
    bps_bus_settings b = {-1, -1, -1, -1, -1, ""};
    bps_devices_setup(&b);
    TEST_ASSERT_EQUAL_STRING("{}", heartbeat());
}

// ---- Recordings ----

static void test_names_recordings_by_their_utc_start(void)
{
    char day[11], time[11];
    bps_record_names(1792912530123000000LL, day, time); // 2026-10-25T07:15:30.123Z
    TEST_ASSERT_EQUAL_STRING("2026-10-25", day);
    TEST_ASSERT_EQUAL_STRING("071530.123", time);
    bps_record_names(1709251199999000000LL, day, time); // a leap day's last millisecond
    TEST_ASSERT_EQUAL_STRING("2024-02-29", day);
    TEST_ASSERT_EQUAL_STRING("235959.999", time);
    bps_record_names(-1000000000LL, day, time);
    TEST_ASSERT_EQUAL_STRING("1969-12-31", day);
    TEST_ASSERT_EQUAL_STRING("235959.000", time);
}

static void test_saves_a_copy_of_each_chunk(void)
{
    card = (port_sd_info){.present = true, .total_bytes = 1ull << 30, .free_bytes = 1ull << 30};
    const uint8_t head[42] = {'f', 'L', 'a', 'C'}, frame[100] = {0};
    bps_record_begin(1792912530123000000LL, 1792912539123000000LL,
                     "04:f4:1c:07:93:52 5000 9005000", 48000, 432000, head, sizeof head);
    bps_record_frame(frame, sizeof frame);
    bps_record_frame(frame, 60);
    bps_record_end(432000, 480, "sent");
    TEST_ASSERT_EQUAL_STRING(
        "create recordings/2026-10-25/071530.123.flac 42\n"
        "append recordings/2026-10-25/071530.123.flac 100\n"
        "append recordings/2026-10-25/071530.123.flac 60\n"
        "close\n"
        "append recordings/2026-10-25/index.jsonl 210\n"
        "{\"file\":\"071530.123.flac\",\"start_ns\":1792912530123000000,"
        "\"end_ns\":1792912539123000000,\"sample_rate\":48000,\"samples\":432000,"
        "\"lost_samples\":480,\"bytes\":202,\"tsf\":\"04:f4:1c:07:93:52 5000 9005000\","
        "\"upload\":\"sent\"}\n"
        "close\n",
        written);
    bps_record_stats st;
    bps_record_stats_get(&st);
    TEST_ASSERT_EQUAL_UINT32(1, st.files);
    TEST_ASSERT_EQUAL_UINT64(202, st.bytes);
}

static void test_skips_chunks_without_a_card_or_room(void)
{
    const uint8_t head[42] = {0};
    bps_record_stats before, after;
    bps_record_stats_get(&before);
    bps_record_begin(0, 1, NULL, 48000, 48000, head, sizeof head);
    bps_record_frame(head, 10);
    bps_record_end(48000, 0, "sent");
    card = (port_sd_info){.present = true, .total_bytes = 1ull << 30,
                          .free_bytes = BPS_RECORD_MIN_FREE - 1};
    bps_record_begin(0, 1, NULL, 48000, 48000, head, sizeof head);
    bps_record_end(48000, 0, "sent");
    bps_record_stats_get(&after);
    TEST_ASSERT_EQUAL_size_t(0, writes);
    TEST_ASSERT_EQUAL_UINT32(before.files, after.files);
    TEST_ASSERT_EQUAL_UINT32(before.skipped + 2, after.skipped);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_parses_device_lists);
    RUN_TEST(test_reports_the_battery_and_card);
    RUN_TEST(test_reports_no_battery_and_an_empty_slot);
    RUN_TEST(test_leaves_out_i2c_devices_without_a_bus);
    RUN_TEST(test_no_devices_no_report);
    RUN_TEST(test_names_recordings_by_their_utc_start);
    RUN_TEST(test_saves_a_copy_of_each_chunk);
    RUN_TEST(test_skips_chunks_without_a_card_or_room);
    return UNITY_END();
}
