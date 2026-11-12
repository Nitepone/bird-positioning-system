// The trace log's ring (logbuf.c): what it keeps, what it drops and counts,
// what a heartbeat's share of it looks like, and when lines count as sent.
#include <unity.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "logbuf.h"
#include "port.h"

// The ring's lock, as a board has it; `held` stands for another task.
static bool held;
bool port_log_trylock(void)
{
    if (held)
        return false;
    held = true;
    return true;
}
void port_log_unlock(void) { held = false; }

static char out[8192];

// A heartbeat's log part: `{"x":0` + the ring's fields + `}`.
static const char *take(size_t budget, uint64_t now_ms, bps_log_batch *b)
{
    bps_json j;
    bps_json_init(&j, out, sizeof out);
    bps_json_raw(&j, "{\"x\":0");
    bps_log_json(&j, budget, now_ms, b);
    bps_json_raw(&j, "}");
    TEST_ASSERT_FALSE(j.overflow);
    return out;
}

// Lines in `s`: the elements of its "lines" array.
static int count_lines(const char *s)
{
    const char *l = strstr(s, "\"lines\":[");
    if (!l)
        return -1;
    int n = 0, depth = 1;
    bool in_str = false;
    for (const char *p = l + 9; *p && depth > 0; p++) {
        if (in_str) {
            if (*p == '\\')
                p++;
            else if (*p == '"')
                in_str = false;
        } else if (*p == '"') {
            in_str = true;
        } else if (*p == '[') {
            n += depth == 1;
            depth++;
        } else if (*p == ']') {
            depth--;
        }
    }
    return n;
}

static long dropped_of(const char *s)
{
    const char *d = strstr(s, "\"dropped\":");
    return d ? strtol(d + 10, NULL, 10) : -1;
}

void setUp(void)
{
    bps_log_reset();
    held = false;
    bps_log_set_capture(true);
}
void tearDown(void) {}

static void test_nothing_is_kept_while_capture_is_off(void)
{
    bps_log_set_capture(false);
    bps_log_capture(1000, "not kept");
    TEST_ASSERT_FALSE(bps_log_pending());
    bps_log_batch b;
    const char *s = take(1024, 2, &b);
    TEST_ASSERT_TRUE(b.valid);
    TEST_ASSERT_EQUAL_STRING("{\"x\":0,\"dropped\":0,\"lines\":[]}", s);
}

static void test_lines_with_their_times_escaped_and_cleaned(void)
{
    bps_log_capture(1500000, "wifi: \"home\" C:\\x");
    bps_log_capture(2000000, "tab\there, caf\xc3\xa9, bell\a");
    bps_log_batch b;
    const char *s = take(1024, 2500, &b);
    TEST_ASSERT_EQUAL_STRING("{\"x\":0,\"dropped\":0,\"lines\":[[1500,\"wifi: \\\"home\\\" C:\\\\x\"],"
                             "[2000,\"tab here, caf??, bell \"]]}",
                             s);
    TEST_ASSERT_EQUAL_UINT32(2, b.lines);
    TEST_ASSERT_FALSE(b.more);
}

// Line times are kept as their low 32 bits of ms, and given in full.
static void test_times_past_49_days(void)
{
    uint64_t now_ms = (1ull << 32) + 5000; // 49.7 days and 5 s
    bps_log_capture((now_ms - 10000) * 1000, "before the wrap");
    bps_log_capture((now_ms - 1000) * 1000, "after it");
    bps_log_batch b;
    const char *s = take(1024, now_ms, &b);
    char want[160];
    snprintf(want, sizeof want, "[[%llu,\"before the wrap\"],[%llu,\"after it\"]]",
             (unsigned long long)(now_ms - 10000), (unsigned long long)(now_ms - 1000));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(s, want), s);
}

// Until delivered, the same lines (and more) go again.
static void test_lines_count_as_sent_only_once_delivered(void)
{
    bps_log_capture(1000, "one");
    bps_log_batch b;
    take(1024, 2, &b); // that heartbeat failed: not delivered
    bps_log_capture(2000, "two");
    const char *s = take(1024, 3, &b);
    TEST_ASSERT_EQUAL_INT(2, count_lines(s));
    TEST_ASSERT_TRUE(bps_log_pending());
    bps_log_delivered(&b);
    TEST_ASSERT_FALSE(bps_log_pending());
    s = take(1024, 3, &b);
    TEST_ASSERT_EQUAL_INT(0, count_lines(s));
    bps_log_capture(3000, "three");
    s = take(1024, 4, &b);
    TEST_ASSERT_NOT_NULL(strstr(s, "[3,\"three\"]"));
    TEST_ASSERT_EQUAL_INT(1, count_lines(s));
}

// A full ring drops its oldest lines, and the next heartbeat counts them.
static void test_the_ring_wraps_and_counts_what_it_dropped(void)
{
    char line[64];
    const int n = 5000; // 29 bytes each in the ring: many times round it
    for (int i = 0; i < n; i++) {
        snprintf(line, sizeof line, "line %06d of the test", i);
        bps_log_capture((uint64_t)i * 1000, line);
    }
    int got = 0, last = -1;
    long dropped = 0;
    for (int beat = 0; beat < 100 && bps_log_pending(); beat++) {
        bps_log_batch b;
        const char *s = take(2048, n, &b);
        TEST_ASSERT_TRUE(b.valid);
        long d = dropped_of(s);
        dropped += d;
        // In order, with a gap only where lines were dropped, before the first.
        bool first = true;
        for (const char *p = s; (p = strstr(p, "\"line ")); p++) {
            int k = atoi(p + 6);
            TEST_ASSERT_EQUAL_INT(first ? last + 1 + (int)d : last + 1, k);
            first = false;
            last = k;
            got++;
        }
        bps_log_delivered(&b);
    }
    TEST_ASSERT_EQUAL_INT(n - 1, last);          // the newest arrived
    TEST_ASSERT_EQUAL_INT(n, got + (int)dropped); // and every line is accounted for
    TEST_ASSERT_TRUE(got <= BPS_LOG_RING_BYTES / (6 + (int)strlen("line 000000 of the test")));
    TEST_ASSERT_TRUE(dropped > 0);
}

// Lines that leave the ring while their heartbeat is in flight were sent
// with it: once it is delivered they are not counted as dropped.
static void test_lines_overwritten_in_flight_are_not_counted_as_dropped(void)
{
    char line[64];
    for (int i = 0; i < 10; i++) {
        snprintf(line, sizeof line, "early %d", i);
        bps_log_capture(1000, line);
    }
    bps_log_batch b;
    take(4096, 2, &b); // carries the 10
    TEST_ASSERT_EQUAL_UINT32(10, b.lines);
    // Meanwhile, enough to turn the ring over completely.
    int more = BPS_LOG_RING_BYTES / 20 * 3;
    for (int i = 0; i < more; i++) {
        snprintf(line, sizeof line, "later %05d", i);
        bps_log_capture(2000, line);
    }
    bps_log_delivered(&b);
    long dropped = 0;
    int got = 0;
    for (int beat = 0; beat < 50 && bps_log_pending(); beat++) {
        const char *s = take(2048, 3, &b);
        dropped += dropped_of(s);
        got += count_lines(s);
        TEST_ASSERT_NULL(strstr(s, "early"));
        bps_log_delivered(&b);
    }
    TEST_ASSERT_EQUAL_INT(more, got + (int)dropped);
    TEST_ASSERT_TRUE(dropped > 0);
}

// A heartbeat's share never passes its budget; the rest waits.
static void test_the_budget_is_kept_to(void)
{
    char line[BPS_LOG_LINE_MAX + 50];
    memset(line, '"', sizeof line - 1); // escapes to twice as long
    line[sizeof line - 1] = 0;
    bps_log_capture(1000, line); // cut to BPS_LOG_LINE_MAX
    bps_log_capture(2000, "short");
    for (size_t budget = 96; budget < 1200; budget += 37) {
        bps_log_batch b;
        bps_json j;
        bps_json_init(&j, out, sizeof out);
        bps_log_json(&j, budget, 3, &b);
        TEST_ASSERT_TRUE(b.valid);
        TEST_ASSERT_TRUE_MESSAGE(j.len <= budget, out);
        TEST_ASSERT_TRUE(b.lines >= 1); // always progress: a long first line is cut
        TEST_ASSERT_EQUAL_CHAR(']', out[j.len - 1]);
        if (budget < 2 * BPS_LOG_LINE_MAX)
            TEST_ASSERT_TRUE(b.more);
    }
    // The whole first line, escaped, once there is room.
    bps_log_batch b;
    const char *s = take(1024, 3, &b);
    TEST_ASSERT_EQUAL_UINT32(2, b.lines);
    TEST_ASSERT_FALSE(b.more);
    const char *t = strstr(s, "[1,\"");
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_EQUAL_INT(2 * BPS_LOG_LINE_MAX, strstr(t, "\"],") - (t + 4));
    // Too small a budget: nothing written, nothing taken.
    bps_json j;
    bps_json_init(&j, out, sizeof out);
    bps_log_json(&j, 40, 3, &b);
    TEST_ASSERT_FALSE(b.valid);
    TEST_ASSERT_EQUAL_size_t(0, j.len);
}

// The ring is never waited for: a line logged while it is held is dropped
// and counted; a heartbeat made while it is held leaves the lines for later.
static void test_a_held_ring_is_never_waited_for(void)
{
    bps_log_capture(1000, "kept");
    held = true;
    bps_log_capture(2000, "dropped");
    bps_log_capture(3000, "dropped too");
    bps_log_batch b;
    bps_json j;
    bps_json_init(&j, out, sizeof out);
    bps_log_json(&j, 1024, 4, &b);
    TEST_ASSERT_FALSE(b.valid);
    TEST_ASSERT_EQUAL_size_t(0, j.len);
    bps_log_delivered(&b); // nothing to deliver
    held = false;
    const char *s = take(1024, 4, &b);
    TEST_ASSERT_EQUAL_INT(1, count_lines(s));
    TEST_ASSERT_EQUAL_INT(2, dropped_of(s));
    bps_log_delivered(&b);
    s = take(1024, 4, &b);
    TEST_ASSERT_EQUAL_INT(0, dropped_of(s));
    TEST_ASSERT_FALSE(bps_log_pending());
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_nothing_is_kept_while_capture_is_off);
    RUN_TEST(test_lines_with_their_times_escaped_and_cleaned);
    RUN_TEST(test_times_past_49_days);
    RUN_TEST(test_lines_count_as_sent_only_once_delivered);
    RUN_TEST(test_the_ring_wraps_and_counts_what_it_dropped);
    RUN_TEST(test_lines_overwritten_in_flight_are_not_counted_as_dropped);
    RUN_TEST(test_the_budget_is_kept_to);
    RUN_TEST(test_a_held_ring_is_never_waited_for);
    return UNITY_END();
}
