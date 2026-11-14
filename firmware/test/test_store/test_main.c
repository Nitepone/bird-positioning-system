// The store's items keep their bytes and order as the buffer wraps around,
// and the ring keeps its newest audio when it moves to other memory.
#include <unity.h>

#include <stdlib.h>
#include <string.h>

#include "ring.h"
#include "store.h"

void setUp(void) {}
void tearDown(void) {}

static uint8_t buf[100];

// Item `k`'s bytes: k, k+1, ... (mod 256).
static void fill(uint8_t *out, uint32_t n, uint8_t k)
{
    for (uint32_t i = 0; i < n; i++)
        out[i] = (uint8_t)(k + i);
}

static void add(bps_store *s, uint32_t n, uint8_t k)
{
    uint8_t b[100];
    fill(b, n, k);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, bps_store_open(s));
    TEST_ASSERT_TRUE(bps_store_append(s, b, n / 2));
    TEST_ASSERT_TRUE(bps_store_append(s, b + n / 2, n - n / 2));
    bps_store_close(s);
}

static void expect(const bps_store *s, uint32_t k, uint32_t n, uint8_t first)
{
    uint8_t want[100], got[100];
    fill(want, n, first);
    int slot = bps_store_at(s, k);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, slot);
    TEST_ASSERT_EQUAL_UINT32(n, bps_store_len(s, slot));
    TEST_ASSERT_EQUAL_UINT32(n, bps_store_read(s, slot, 0, got, n));
    TEST_ASSERT_EQUAL_MEMORY(want, got, n);
}

static void test_items_wrap_around_the_buffer(void)
{
    bps_store s;
    bps_store_init(&s, buf, sizeof buf);
    add(&s, 40, 1);
    add(&s, 40, 50);
    TEST_ASSERT_EQUAL_UINT32(20, bps_store_free(&s));
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, bps_store_open(&s));
    TEST_ASSERT_FALSE(bps_store_append(&s, buf, 30)); // full
    bps_store_cancel(&s);
    bps_store_pop(&s);
    add(&s, 50, 100); // from byte 80, around the end
    TEST_ASSERT_EQUAL_UINT32(2, bps_store_count(&s));
    expect(&s, 0, 40, 50);
    expect(&s, 1, 50, 100);
    bps_store_pop(&s);
    expect(&s, 0, 50, 100);
    TEST_ASSERT_EQUAL_UINT32(50, bps_store_free(&s));
}

static void test_released_bytes_make_room_for_an_item_being_written(void)
{
    bps_store s;
    bps_store_init(&s, buf, sizeof buf);
    uint8_t b[100];
    fill(b, 100, 7);
    int slot = bps_store_open(&s);
    TEST_ASSERT_TRUE(bps_store_append(&s, b, 90));
    TEST_ASSERT_FALSE(bps_store_append(&s, b + 90, 20));
    bps_store_release(&s, 60); // sent: streaming
    TEST_ASSERT_TRUE(bps_store_append(&s, b + 90, 10));
    TEST_ASSERT_TRUE(bps_store_is_open(&s, slot));
    uint8_t got[40];
    TEST_ASSERT_EQUAL_UINT32(0, bps_store_read(&s, slot, 0, got, 10)); // released
    TEST_ASSERT_EQUAL_UINT32(40, bps_store_read(&s, slot, 60, got, 40));
    TEST_ASSERT_EQUAL_MEMORY(b + 60, got, 40);
    TEST_ASSERT_EQUAL_UINT32(100, bps_store_len(&s, slot));
}

static void test_a_cancelled_item_frees_its_bytes(void)
{
    bps_store s;
    bps_store_init(&s, buf, sizeof buf);
    add(&s, 30, 1);
    bps_store_open(&s);
    TEST_ASSERT_TRUE(bps_store_append(&s, buf, 50));
    TEST_ASSERT_EQUAL_INT(-1, bps_store_open(&s)); // one at a time
    bps_store_cancel(&s);
    TEST_ASSERT_EQUAL_UINT32(1, bps_store_count(&s));
    TEST_ASSERT_EQUAL_UINT32(70, bps_store_free(&s));
    add(&s, 60, 9);
    expect(&s, 1, 60, 9);
}

static void test_items_are_limited_in_number(void)
{
    static uint8_t big[1000];
    bps_store s;
    bps_store_init(&s, big, sizeof big);
    for (int i = 0; i < BPS_STORE_ITEMS; i++)
        add(&s, 4, (uint8_t)i);
    TEST_ASSERT_EQUAL_INT(-1, bps_store_open(&s));
    bps_store_pop(&s);
    add(&s, 4, 200);
    expect(&s, BPS_STORE_ITEMS - 1, 4, 200);
}

static void *fail_realloc(void *p, size_t n)
{
    (void)p;
    (void)n;
    return NULL;
}

// Making room for a model: a store whose items wrap around its end shrinks,
// keeping them (and one still being written), and grows again.
static void test_the_store_shrinks_and_grows_keeping_its_items(void)
{
    bps_store s;
    uint8_t *b = malloc(100);
    bps_store_init(&s, b, 100);
    add(&s, 40, 1);
    add(&s, 40, 50);
    bps_store_pop(&s);
    add(&s, 30, 100); // bytes 80-109: wraps
    uint8_t part[10];
    fill(part, 10, 7);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, bps_store_open(&s));
    TEST_ASSERT_TRUE(bps_store_append(&s, part, 10));
    TEST_ASSERT_FALSE(bps_store_resize(&s, 70, realloc)); // 80 held
    TEST_ASSERT_TRUE(bps_store_resize(&s, 80, realloc));
    TEST_ASSERT_EQUAL_UINT32(80, s.cap);
    TEST_ASSERT_EQUAL_UINT32(0, bps_store_free(&s));
    expect(&s, 0, 40, 50);
    expect(&s, 1, 30, 100);
    TEST_ASSERT_TRUE(bps_store_resize(&s, 200, realloc));
    fill(part, 10, 17);
    TEST_ASSERT_TRUE(bps_store_append(&s, part, 10)); // the open item goes on
    bps_store_close(&s);
    expect(&s, 2, 20, 7);
    bps_store_pop(&s);
    add(&s, 100, 3);
    expect(&s, 0, 30, 100);
    expect(&s, 2, 100, 3);
    TEST_ASSERT_FALSE(bps_store_resize(&s, 150, fail_realloc)); // kept as it was
    TEST_ASSERT_EQUAL_UINT32(200, s.cap);
    expect(&s, 1, 20, 7);
    free(s.buf);
}

static int16_t sample_at(uint32_t i) { return (int16_t)(i * 7); }

static void write_n(bps_ring *r, uint32_t from, uint32_t n)
{
    int16_t b[1000];
    while (n) {
        uint32_t k = n < 1000 ? n : 1000;
        for (uint32_t i = 0; i < k; i++)
            b[i] = sample_at(from + i);
        bps_ring_write(r, b, k);
        from += k;
        n -= k;
    }
}

static int allocs;
static void *counting_alloc(size_t n)
{
    allocs++;
    return malloc(n);
}

// Low-power mode moves the ring to internal RAM and back: the newest audio
// stays readable at the same indices, and writing goes on.
static void test_the_ring_moves_keeping_its_newest_audio(void)
{
    bps_ring r;
    bps_ring_alloc(&r, 48000, 8 * BPS_RING_BLOCK);
    uint32_t n = 8 * BPS_RING_BLOCK + 12345; // wrapped once
    write_n(&r, 0, n);
    bps_ring_mem mem = {counting_alloc, free};
    TEST_ASSERT_TRUE(bps_ring_move(&r, 3 * BPS_RING_BLOCK, &mem));
    TEST_ASSERT_EQUAL_INT(3, allocs);
    TEST_ASSERT_EQUAL_UINT32(3 * BPS_RING_BLOCK, r.capacity);
    TEST_ASSERT_EQUAL_UINT32(n, bps_ring_written(&r));
    static int16_t got[40000];
    uint32_t keep = 3 * BPS_RING_BLOCK - 4096;
    TEST_ASSERT_TRUE(bps_ring_read(&r, n - keep, got, keep));
    for (uint32_t i = 0; i < keep; i++)
        TEST_ASSERT_EQUAL_INT16(sample_at(n - keep + i), got[i]);
    TEST_ASSERT_FALSE(bps_ring_read(&r, n - 3 * BPS_RING_BLOCK, got, 10)); // gone

    write_n(&r, n, 5000);
    TEST_ASSERT_TRUE(bps_ring_move(&r, 8 * BPS_RING_BLOCK, NULL));
    TEST_ASSERT_EQUAL_UINT32(8 * BPS_RING_BLOCK, r.capacity);
    write_n(&r, n + 5000, 30000);
    uint32_t w = n + 35000;
    TEST_ASSERT_TRUE(bps_ring_read(&r, w - 40000, got, 40000));
    for (uint32_t i = 0; i < 40000; i++)
        TEST_ASSERT_EQUAL_INT16(sample_at(w - 40000 + i), got[i]);
}

static void *no_alloc(size_t n)
{
    (void)n;
    return NULL;
}

static void test_a_ring_that_cannot_move_stays(void)
{
    bps_ring r;
    bps_ring_alloc(&r, 48000, 2 * BPS_RING_BLOCK);
    write_n(&r, 0, 1000);
    bps_ring_mem mem = {no_alloc, free};
    TEST_ASSERT_FALSE(bps_ring_move(&r, 2 * BPS_RING_BLOCK, &mem));
    TEST_ASSERT_EQUAL_UINT32(2 * BPS_RING_BLOCK, r.capacity);
    int16_t got[1000];
    TEST_ASSERT_TRUE(bps_ring_read(&r, 0, got, 1000));
    TEST_ASSERT_EQUAL_INT16(sample_at(999), got[999]);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_items_wrap_around_the_buffer);
    RUN_TEST(test_released_bytes_make_room_for_an_item_being_written);
    RUN_TEST(test_a_cancelled_item_frees_its_bytes);
    RUN_TEST(test_items_are_limited_in_number);
    RUN_TEST(test_the_store_shrinks_and_grows_keeping_its_items);
    RUN_TEST(test_the_ring_moves_keeping_its_newest_audio);
    RUN_TEST(test_a_ring_that_cannot_move_stays);
    return UNITY_END();
}
