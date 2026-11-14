// Which sounds upload.c uploads, and when: a chunk goes once it is complete
// in the store (or, with a store too small for one, as it is encoded), and
// never if validation rejects it. After the network was down for 13 hours, or
// the gate was shut that long, the ring indices it keeps between chunks must
// still compare once 2^31 samples (12.4 h at 48 kHz) went by, or every upload
// is held back for as long again (or bogus ones start).
#include <unity.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "port.h"
#include "upload.h"

// ---- The port, as far as upload.c and what it calls reach it ----

// A card holding one model (`card_*`), but no room for recordings.
static const char *card_json;
static const uint8_t *card_model;
static uint32_t card_model_len;
static char read_path[96];
static uint32_t read_off;
static size_t read_len;
static bool read_queued;

void port_log(const char *fmt, ...) { (void)fmt; }
static uint64_t mono_us;
uint64_t port_mono_us(void) { return mono_us; }
void *port_big_alloc(size_t n) { return malloc(n); }
void port_big_free(void *p) { free(p); }
void port_tcp_close(int h) { (void)h; }
bool port_sd_write(const char *p, const void *b, size_t n, bool a) { (void)p, (void)b, (void)n, (void)a; return false; }
void port_sd_close(void) {}
bool port_sd_remove(const char *p) { (void)p; return false; }
bool port_sd_list(const char *d, const char *s) { (void)d, (void)s; return false; }
bool port_sd_read(const char *p, uint32_t o, size_t l)
{
    if (read_queued)
        return false;
    snprintf(read_path, sizeof read_path, "%s", p);
    read_off = o;
    read_len = l;
    read_queued = true;
    return true;
}
int port_sd_result(const void **d, uint32_t *t)
{
    if (!read_queued)
        return -1;
    read_queued = false;
    const uint8_t *data = NULL;
    uint32_t len = 0;
    if (card_json && strcmp(read_path, "models/m/model.json") == 0)
        data = (const uint8_t *)card_json, len = (uint32_t)strlen(card_json);
    else if (card_model && strcmp(read_path, "models/m/model.tflite") == 0)
        data = card_model, len = card_model_len;
    else
        return -1;
    uint32_t left = read_off < len ? len - read_off : 0;
    *d = data + read_off;
    *t = len;
    return (int)(left < read_len ? left : read_len);
}
void port_sd_info_get(port_sd_info *o)
{
    memset(o, 0, sizeof *o);
    o->present = card_json != NULL;
}

static void (*worker)(void);
bool port_worker_start(void (*fn)(void))
{
    worker = fn;
    return true;
}
void port_worker_kick(void) { worker(); } // jobs run at once

// The model scores every window `score` (0: validation rejects them all).
static uint8_t in_tensor[16 * 184];
static int8_t out_tensor[2] = {-128, -128};
static int invokes; // checks run, and set-up runs
bool port_ml_load(const void *m, size_t b, size_t a, port_ml_tensor *i, port_ml_tensor *o, size_t *u,
                  char *e, size_t c)
{
    (void)m, (void)b, (void)a, (void)e, (void)c;
    port_ml_tensor in = {true, in_tensor, sizeof in_tensor, 1.0f, 0};
    port_ml_tensor out = {true, out_tensor, sizeof out_tensor, 1.0f / 256, -128};
    *i = in;
    *o = out;
    *u = 1000;
    return true;
}
bool port_ml_invoke(void)
{
    invokes++;
    return true;
}
void port_ml_unload(void) {}

// ---- The client, as far as upload.c uses it ----

#define RATE 48000
#define CHUNK 4800 // 0.1 s
#define LOOKBACK 480
#define HOUR_SAMPLES (3600ull * RATE)
#define STORE_BYTES (2u << 20)

// For the next set_up().
static uint32_t chunk_len = CHUNK, lookback = LOOKBACK, store_bytes = STORE_BYTES;

static bps_settings cfg;
static bps_ring ring;
static bps_capture cap;
static bps_clock clk;
static bps_beacon beacon;
static bps_store store;
static uint8_t *store_buf;
static int64_t epoch_ns;
static bool epoch_set = true, need_register;
static uint64_t chunks_sent, chunks_rejected, total; // total: samples since boot
static uint32_t overruns;
static int connects;   // upload attempts
static bool server_up; // else none gets through
static bool noisy;     // capture noise (FLAC hardly shrinks it), else silence
static int writes_before_end; // bytes written of an upload before the chunk was complete

int port_tcp_write(int h, const void *b, size_t n)
{
    (void)h, (void)b;
    if (server_up && bps_upload_encoding())
        writes_before_end++;
    return server_up ? (int)n : -1;
}

static int connect_server(const char *what)
{
    (void)what;
    connects++;
    return server_up ? 3 : -1;
}
static void path_for(char *out, size_t n, const char *what) { snprintf(out, n, "/c/%s", what); }
static int read_response(int h, char *b, size_t n, size_t *len, bps_http_response *r)
{
    (void)h, (void)b, (void)n, (void)len;
    r->status = 200;
    return server_up ? 1 : -1;
}
static const char *centi(char *b, size_t n, int32_t v)
{
    snprintf(b, n, "%ld", (long)v);
    return b;
}

// The timestamper refines the timeline every second; once per call here.
static void refine_timeline(void)
{
    bps_timeline tl = {(uint32_t)total, (int64_t)(total * BPS_NANOS_PER_SEC / RATE), 0};
    bps_ring_timeline(&ring, &tl, false, 0);
}

static uint64_t now_us(void) { return total * 1000000 / RATE + 1000000; }

static void capture(uint32_t n)
{
    static int16_t pcm[4800];
    static uint32_t seed = 1;
    while (n > 0) {
        uint32_t k = n < 4800 ? n : 4800;
        for (uint32_t i = 0; i < k; i++) {
            seed = seed * 1103515245u + 12345u;
            pcm[i] = noisy ? (int16_t)(seed >> 16) : 0;
        }
        bps_ring_write(&ring, pcm, k);
        total += k;
        n -= k;
    }
    refine_timeline();
    mono_us = now_us();
}

// The gate passes at the newest sample but `back`.
static void pass(uint32_t back)
{
    cap.last_pass_end = (uint32_t)total - back;
    cap.pass_count++;
}

// The network is down for `hours`: uploads are not polled, but the client's
// loop goes on (as often as it likes; once an hour here).
static void outage(int hours)
{
    for (int h = 0; h < hours; h++) {
        ring.written += (uint32_t)HOUR_SAMPLES;
        total += HOUR_SAMPLES;
        refine_timeline();
        bps_upload_keep_recent();
    }
    capture(RATE);
}

static void set_up(uint32_t blocks, bool validation)
{
    memset(&cap, 0, sizeof cap);
    total = 0;
    connects = 0;
    writes_before_end = 0;
    server_up = false;
    chunks_sent = chunks_rejected = 0;
    overruns = 0;
    cfg.server_host = "server";
    cfg.server_port = 8443;
    cfg.sample_rate = RATE;
    bps_ring_alloc(&ring, RATE, blocks * BPS_RING_BLOCK);
    free(store_buf);
    store_buf = malloc(store_bytes);
    bps_store_init(&store, store_buf, store_bytes);
    bps_gate_config g;
    bps_gate_config_default(&g);
    bps_capture_init(&cap, &ring, RATE, 0, &g);
    bps_clock_init(&clk, true);
    bps_ts_sample s = {0, 0, 1000000};
    bps_clock_add_sample(&clk, s);
    bps_beacon_init(&beacon);
    bps_timeline tl = {0, 0, 0};
    bps_ring_timeline(&ring, &tl, true, 0);
    bps_upload_ctx u = {
        .cfg = &cfg, .ring = &ring, .cap = &cap, .clk = &clk, .beacon = &beacon,
        .epoch_ns = &epoch_ns, .epoch_set = &epoch_set, .chunk_len = chunk_len, .lookback = lookback,
        .store = &store, .chunks_sent = &chunks_sent, .overruns = &overruns,
        .need_register = &need_register, .connect_server = connect_server, .path_for = path_for,
        .read_response = read_response, .centi = centi, .on_validated = NULL,
        .chunks_rejected = &chunks_rejected,
    };
    bps_upload_init(&u);
    bps_upload_set_spool(false);
    bps_validation_config want;
    bps_validation_default(&want);
    bps_validate_configure(&want); // a model an earlier test loaded is let go
    bps_validate_poll(0);
    bps_validate_init(&ring, RATE);
    card_json = NULL;
    if (validation) {
        static const char *json =
            "{\"model_name\": \"m\", \"model_file\": \"model.tflite\", \"sample_rate\": 16000,"
            " \"window_samples\": 48000, \"frontend\": \"logmel_db_minmax\", \"fft_length\": 1024,"
            " \"hop_length\": 256, \"frames\": 184, \"mel_fmin_hz\": 100.0, \"mel_fmax_hz\": 8000.0,"
            " \"layout\": \"time_freq\", \"output_kind\": \"probability\", \"bird_output_first\": 1,"
            " \"bird_output_count\": 1, \"mel_bands\": 16, \"score_threshold\": 0.35,"
            " \"arena_bytes\": 32768, \"model_bytes\": 6384}";
        static uint8_t model[6384];
        memcpy(model + 4, "TFL3", 4);
        card_json = json;
        card_model = model;
        card_model_len = sizeof model;
        want.type = BPS_VALIDATION_MODEL;
        want.threshold = -1;
        strcpy(want.model, "m");
    }
    bps_validate_configure(&want);
    for (int i = 0; i < 100 && validation && !bps_validate_active(); i++)
        bps_validate_poll(0);
    TEST_ASSERT_EQUAL(validation, bps_validate_active());
    capture(RATE);
}

void setUp(void)
{
    chunk_len = CHUNK;
    lookback = LOOKBACK;
    store_bytes = STORE_BYTES;
    noisy = false;
    out_tensor[1] = -128;
}
void tearDown(void) {}

// The client's loop for `seconds`: capturing, and polling every 0.1 s.
static void run(int seconds)
{
    for (int n = seconds * 10; n > 0; n--) {
        capture(RATE / 10);
        bps_upload_keep_recent();
        bps_upload_poll(now_us());
        bps_upload_send_poll(now_us());
    }
}

static void test_uploads_resume_after_13_hours_without_network(void)
{
    set_up(4, false);
    // A chunk handled before the network went (saved on a card with no room:
    // lost, as an upload that went through, it leaves its end behind).
    bps_upload_set_spool(true);
    pass(1000);
    bps_upload_poll(now_us());
    bps_upload_set_spool(false);
    TEST_ASSERT_EQUAL_INT(0, connects);
    TEST_ASSERT_TRUE(bps_upload_idle());

    outage(13);
    pass(1000);
    run(1);
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(1, connects, "the first sound after the network came back is not uploaded");
}

// Runs validation on the chunk the gate opened: polls while capturing.
static void validate_chunk(void)
{
    for (int i = 0; i < 60 && !bps_upload_idle(); i++) {
        capture(RATE / 10);
        bps_upload_poll(now_us());
        bps_upload_send_poll(now_us());
    }
}

static void test_a_rejected_window_does_not_hold_back_uploads_after_13_hours(void)
{
    chunk_len = 9 * RATE; // the model's window (3 s) fits
    set_up(32, true);
    capture(RATE);
    pass(1000);
    bps_upload_poll(now_us());
    TEST_ASSERT_FALSE(bps_upload_idle()); // validating
    validate_chunk();
    TEST_ASSERT_TRUE(bps_upload_idle());
    TEST_ASSERT_EQUAL_UINT64(1, chunks_rejected);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, connects, "a rejected chunk was sent");

    outage(13);
    pass(1000);
    bps_upload_poll(now_us());
    TEST_ASSERT_FALSE_MESSAGE(bps_upload_idle(), "the first sound after the network came back is not validated");
    validate_chunk();
    TEST_ASSERT_EQUAL_UINT64(2, chunks_rejected);
}

static void test_a_chunk_awaiting_validation_through_13_hours_is_dropped(void)
{
    chunk_len = 9 * RATE;
    set_up(32, true);
    capture(RATE);
    pass(1000);
    bps_upload_poll(now_us());
    TEST_ASSERT_FALSE(bps_upload_idle()); // encoding, collecting the model's window

    outage(13);
    TEST_ASSERT_TRUE_MESSAGE(bps_upload_idle(), "still encoding audio that is gone");
    pass(1000);
    bps_upload_poll(now_us());
    TEST_ASSERT_FALSE(bps_upload_idle());
    validate_chunk();
    TEST_ASSERT_EQUAL_UINT64(1, chunks_rejected);
}

// The gate stays shut for `hours`, the network up: the loop goes on (every
// 10 minutes here).
static void quiet(int hours)
{
    for (int k = 0; k < hours * 6; k++) {
        ring.written += (uint32_t)(HOUR_SAMPLES / 6);
        total += HOUR_SAMPLES / 6;
        refine_timeline();
        bps_upload_keep_recent();
        bps_upload_poll(now_us());
        bps_upload_send_poll(now_us());
    }
    capture(RATE);
}

// The last sound before is not taken for a new one once its ring index
// compares as the future (2^31 samples on), which started a bogus upload
// every few seconds and held back real sounds for 12 h more.
static void test_a_gate_shut_for_13_hours_starts_no_upload(void)
{
    chunk_len = 9 * RATE;
    lookback = 750 * RATE / 1000;
    set_up(16, false);
    server_up = true;
    pass(1000);
    run(10);
    TEST_ASSERT_EQUAL_INT(1, connects);
    TEST_ASSERT_EQUAL_UINT64(1, chunks_sent);

    quiet(13);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, connects, "an upload started with the gate shut");
    pass(1000);
    run(10);
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, connects, "the first sound after 13 quiet hours is not uploaded");
    TEST_ASSERT_EQUAL_UINT64(2, chunks_sent);
}

// A sound passing the gate near the end of a chunk (within the look-back of
// it) may still be going: the next chunk follows. One well before the end
// is in the chunk already.
static void test_a_sound_near_the_end_of_a_chunk_continues(void)
{
    chunk_len = 9 * RATE;
    lookback = 750 * RATE / 1000;
    set_up(16, false);
    server_up = true;
    pass(1000); // its chunk ends 8.13 s on
    run(7);
    pass(1000); // 1.13 s before that
    run(10);
    TEST_ASSERT_EQUAL_UINT64(1, chunks_sent);

    pass(1000);
    run(8);
    pass(1000); // 0.13 s before the end of its chunk
    run(12);
    TEST_ASSERT_EQUAL_UINT64(3, chunks_sent);
}

// With a look-back over half a chunk (5 s of 9 s chunks: 4 s left after the
// sound), the sound that started a chunk was taken as still going at its
// end, so every sound took two chunks.
static void test_a_long_look_back_uploads_one_chunk_per_sound(void)
{
    chunk_len = 9 * RATE;
    lookback = 5 * RATE;
    set_up(40, false);
    server_up = true;
    capture(5 * RATE);
    pass(1000);
    run(30);
    TEST_ASSERT_EQUAL_UINT64(1, chunks_sent);
    TEST_ASSERT_EQUAL_INT(1, connects);
}

// Nothing goes while the chunk is recorded: the radio is needed only for a
// burst once it is complete.
static void test_a_chunk_goes_only_once_complete(void)
{
    chunk_len = 9 * RATE;
    set_up(16, false);
    server_up = true;
    pass(1000);
    run(5);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, connects, "an upload started before the chunk was complete");
    TEST_ASSERT_TRUE(bps_upload_encoding());
    TEST_ASSERT_FALSE(bps_upload_ready(now_us()));
    run(6);
    TEST_ASSERT_EQUAL_INT(1, connects);
    TEST_ASSERT_EQUAL_UINT64(1, chunks_sent);
    TEST_ASSERT_EQUAL_INT(0, writes_before_end);
    TEST_ASSERT_TRUE(bps_upload_idle());
}

// A chunk validation rejects never leaves the device.
static void test_a_rejected_chunk_is_never_sent(void)
{
    chunk_len = 9 * RATE;
    set_up(32, true);
    server_up = true;
    pass(1000);
    run(15);
    TEST_ASSERT_EQUAL_UINT64(1, chunks_rejected);
    TEST_ASSERT_EQUAL_INT(0, connects);
    TEST_ASSERT_TRUE(bps_upload_idle());
}

// Validation lets a chunk through, but the server cannot be reached: the
// chunk stays in the store, and goes once the server is back (the model ran
// once for it).
static void test_a_chunk_validation_let_through_waits_for_the_server(void)
{
    chunk_len = 9 * RATE;
    set_up(32, true);
    out_tensor[1] = 127; // a bird in every window
    server_up = true;
    pass(1000);
    run(12);
    TEST_ASSERT_EQUAL_UINT64(1, chunks_sent);

    server_up = false;
    connects = 0;
    int checks = invokes;
    pass(1000);
    run(14);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, invokes - checks, "the model ran again");
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(2, connects, "connecting is not tried again");
    server_up = true;
    run(40);
    TEST_ASSERT_EQUAL_UINT64(2, chunks_sent);
    TEST_ASSERT_EQUAL_INT(1, invokes - checks);
    TEST_ASSERT_TRUE(bps_upload_idle());
}

// While the server is away the store fills; the oldest chunk gives way to the
// newest, and the rest go once it is back.
static void test_a_full_store_drops_the_oldest_chunk(void)
{
    chunk_len = 9 * RATE;
    noisy = true; // about 880 KB a chunk: the 2 MB store holds two
    set_up(16, false);
    for (int i = 0; i < 3; i++) {
        pass(1000);
        run(10);
    }
    bps_upload_status us;
    bps_upload_status_get(&us);
    TEST_ASSERT_FALSE(us.streaming);
    TEST_ASSERT_EQUAL_UINT32(2, us.queued);
    TEST_ASSERT_EQUAL_UINT32(1, us.dropped);
    TEST_ASSERT_EQUAL_UINT32(0, overruns);
    server_up = true;
    run(40);
    TEST_ASSERT_EQUAL_UINT64(2, chunks_sent);
    TEST_ASSERT_TRUE(bps_upload_idle());
}

// A store too small for a whole chunk (a board without PSRAM): chunks stream
// as they are encoded, as before there was a store.
static void test_a_small_store_streams_chunks(void)
{
    chunk_len = 9 * RATE;
    store_bytes = 32 * 1024;
    noisy = true;
    set_up(16, false);
    server_up = true;
    pass(1000);
    run(2);
    TEST_ASSERT_EQUAL_INT(1, connects);
    TEST_ASSERT_GREATER_THAN_INT(0, writes_before_end);
    run(10);
    TEST_ASSERT_EQUAL_UINT64(1, chunks_sent);
    TEST_ASSERT_EQUAL_UINT32(0, overruns);
    bps_upload_status us;
    bps_upload_status_get(&us);
    TEST_ASSERT_TRUE(us.streaming);
    TEST_ASSERT_TRUE(bps_upload_idle());
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_uploads_resume_after_13_hours_without_network);
    RUN_TEST(test_a_rejected_window_does_not_hold_back_uploads_after_13_hours);
    RUN_TEST(test_a_chunk_awaiting_validation_through_13_hours_is_dropped);
    RUN_TEST(test_a_gate_shut_for_13_hours_starts_no_upload);
    RUN_TEST(test_a_sound_near_the_end_of_a_chunk_continues);
    RUN_TEST(test_a_long_look_back_uploads_one_chunk_per_sound);
    RUN_TEST(test_a_chunk_goes_only_once_complete);
    RUN_TEST(test_a_rejected_chunk_is_never_sent);
    RUN_TEST(test_a_chunk_validation_let_through_waits_for_the_server);
    RUN_TEST(test_a_full_store_drops_the_oldest_chunk);
    RUN_TEST(test_a_small_store_streams_chunks);
    return UNITY_END();
}
