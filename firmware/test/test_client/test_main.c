// The client (client.c) with its spool (spool.c), their state at hand, on a
// fake board: a storage card as the ESP32 port has it (reads one at a time,
// sharing one answer, worked through by the card's task between loops), a
// server that takes every upload, and a model that hears no bird.
#include <unity.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "card.h"
#include "port.h"

static uint64_t now_us = 1000000;

// ---- The board ----

void port_log(const char *fmt, ...) { (void)fmt; }
uint64_t port_mono_us(void) { return now_us; }
void port_unique_id(uint8_t out[8]) { memset(out, 7, 8); }
size_t port_free_heap(void) { return 8u << 20; }
static bool wifi_up = true;
void port_wifi_begin(const char *s, const char *p, const char *h) { (void)s, (void)p, (void)h; }
bool port_wifi_off(void) { return true; }
bool port_wifi_up(void) { return wifi_up; }
int port_wifi_rssi(void) { return -50; }
void port_wifi_diag(char *out, size_t cap) { snprintf(out, cap, "fake"); }
bool port_beacon_take(port_beacon *out) { (void)out; return false; }
void port_idle_ms(uint32_t ms) { (void)ms; }
bool port_i2c_begin(int sda, int scl, uint32_t hz) { (void)sda, (void)scl, (void)hz; return false; }
bool port_i2c_xfer(uint8_t a, const uint8_t *tx, size_t tn, uint8_t *rx, size_t rn)
{
    (void)a, (void)tx, (void)tn, (void)rx, (void)rn;
    return false;
}
bool port_sd_begin(int sck, int miso, int mosi, int cs) { (void)sck, (void)miso, (void)mosi, (void)cs; return true; }
void port_audio_start(uint32_t rate) { (void)rate; }
void port_led(bool on) { (void)on; }
bool port_button(void) { return false; }
void *port_big_alloc(size_t n) { return malloc(n); }
void port_big_free(void *p) { free(p); }
int port_udp_open(const char *host, uint16_t port) { (void)host, (void)port; return -1; }
int port_udp_send(int h, const void *b, size_t n) { (void)h, (void)b, (void)n; return -1; }
int port_udp_recv(int h, void *b, size_t n) { (void)h, (void)b, (void)n; return 0; }
void port_udp_close(int h) { (void)h; }

// ---- The server: takes `room` bytes a loop; each request's body is checked ----

static bool server_up;
static size_t room_per_loop = SIZE_MAX, room;
static uint8_t request[1 << 17];
static size_t request_len;
static bool answered;
static const char *reply = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
static void (*on_request)(void); // once the request is in, before the reply

int port_tcp_connect(const char *host, uint16_t port)
{
    (void)host, (void)port;
    if (!server_up)
        return -1;
    request_len = 0;
    answered = false;
    return 5;
}
int port_tcp_write(int h, const void *b, size_t n)
{
    (void)h;
    if (n > room)
        n = room;
    room -= n;
    if (request_len + n <= sizeof request) {
        memcpy(request + request_len, b, n);
        request_len += n;
    }
    return (int)n;
}
int port_tcp_read(int h, void *b, size_t n)
{
    (void)h;
    if (answered)
        return -1;
    if (on_request)
        on_request();
    size_t k = strlen(reply) < n ? strlen(reply) : n;
    memcpy(b, reply, k);
    answered = true;
    return (int)k;
}
void port_tcp_close(int h) { (void)h; }

// ---- The storage card ----

typedef struct {
    char path[48];
    uint8_t *data;
    uint32_t len;
} file;
static file files[32];
static uint8_t answer_buf[PORT_SD_READ_MAX]; // the port's one result
static int answer = -1;
static uint32_t answer_total;
static struct {
    bool list;
    char path[48], suffix[8];
    uint32_t off;
    size_t len;
} queued;
static bool has_queued;
static int model_reads; // of models/

static file *find(const char *path)
{
    for (int i = 0; i < 32; i++)
        if (files[i].path[0] && strcmp(files[i].path, path) == 0)
            return &files[i];
    return NULL;
}
static void put(const char *path, const void *data, uint32_t len, bool append)
{
    file *f = find(path);
    for (int i = 0; !f && i < 32; i++)
        if (!files[i].path[0])
            f = &files[i], snprintf(f->path, sizeof f->path, "%s", path), f->len = 0;
    TEST_ASSERT_NOT_NULL_MESSAGE(f, "the card is full");
    uint32_t keep = append ? f->len : 0;
    f->data = realloc(f->data, keep + len + 1);
    memcpy(f->data + keep, data, len);
    f->len = keep + len;
}
// Files in spool/ ending in `suffix`.
static int count(const char *suffix)
{
    int n = 0;
    for (int i = 0; i < 32; i++) {
        size_t k = strlen(files[i].path);
        n += strncmp(files[i].path, "spool/", 6) == 0 && k > strlen(suffix) &&
             strcmp(files[i].path + k - strlen(suffix), suffix) == 0;
    }
    return n;
}

void port_sd_info_get(port_sd_info *o)
{
    memset(o, 0, sizeof *o);
    o->present = true;
    o->total_bytes = o->free_bytes = 8ull << 30;
}
bool port_sd_write(const char *p, const void *b, size_t n, bool a)
{
    put(p, b, (uint32_t)n, a);
    return true;
}
void port_sd_close(void) {}
bool port_sd_remove(const char *p)
{
    file *f = find(p);
    if (f) {
        free(f->data);
        memset(f, 0, sizeof *f);
    }
    return true;
}
static bool ask(bool list, const char *path, uint32_t off, size_t len, const char *suffix)
{
    if (answer == PORT_SD_PENDING)
        return false;
    answer = PORT_SD_PENDING;
    queued.list = list;
    snprintf(queued.path, sizeof queued.path, "%s", path);
    snprintf(queued.suffix, sizeof queued.suffix, "%s", suffix ? suffix : "");
    queued.off = off;
    queued.len = len;
    has_queued = true;
    model_reads += strncmp(path, "models/", 7) == 0;
    return true;
}
bool port_sd_read(const char *p, uint32_t off, size_t len) { return ask(false, p, off, len, NULL); }
bool port_sd_list(const char *dir, const char *suffix) { return ask(true, dir, 0, 0, suffix); }
int port_sd_result(const void **data, uint32_t *total)
{
    *data = answer_buf;
    *total = answer_total;
    return answer;
}

// The card's task: answers what is queued, into the one result.
static void card_task(void)
{
    if (!has_queued)
        return;
    has_queued = false;
    if (!queued.list) {
        file *f = find(queued.path);
        if (!f) {
            answer = -1;
            return;
        }
        uint32_t left = queued.off < f->len ? f->len - queued.off : 0;
        uint32_t n = queued.len < left ? (uint32_t)queued.len : left;
        memcpy(answer_buf, f->data + queued.off, n);
        answer_total = f->len;
        answer = (int)n;
        return;
    }
    size_t len = 0, dl = strlen(queued.path), sl = strlen(queued.suffix);
    answer_total = 0;
    for (int i = 0; i < 32; i++) {
        const char *p = files[i].path;
        if (!p[0] || strncmp(p, queued.path, dl) != 0 || p[dl] != '/')
            continue;
        const char *name = p + dl + 1;
        size_t n = strlen(name);
        if (n < sl || strcmp(name + n - sl, queued.suffix) != 0 || len + n + 1 > sizeof answer_buf)
            continue;
        memcpy(answer_buf + len, name, n);
        len += n;
        answer_buf[len++] = '\n';
        answer_total++;
    }
    answer = (int)len;
}

// ---- The model: loads (unless `model_fails`), and hears no bird ----

static void (*worker)(void);
static bool model_fails;
bool port_worker_start(void (*fn)(void))
{
    worker = fn;
    return true;
}
void port_worker_kick(void) { worker(); }
static uint8_t in_tensor[16 * 184];
static int8_t out_tensor[2] = {-128, -128};
bool port_ml_load(const void *m, size_t b, size_t a, port_ml_tensor *i, port_ml_tensor *o, size_t *u,
                  char *e, size_t c)
{
    (void)m, (void)b, (void)a;
    if (model_fails) {
        snprintf(e, c, "its tensors do not fit");
        return false;
    }
    port_ml_tensor in = {true, in_tensor, sizeof in_tensor, 1.0f, 0};
    port_ml_tensor out = {true, out_tensor, sizeof out_tensor, 1.0f / 256, -128};
    *i = in;
    *o = out;
    *u = 1000;
    return true;
}
bool port_ml_invoke(void) { return true; }
void port_ml_unload(void) {}

#include "client.c"
#include "spool.c"

// ---- Running it ----

#define RATE 48000
#define MS (RATE / 1000)

static const char *MODEL_JSON =
    "{\"model_name\": \"m\", \"model_file\": \"model.tflite\", \"sample_rate\": 16000,"
    " \"window_samples\": 48000, \"frontend\": \"logmel_db_minmax\", \"fft_length\": 1024,"
    " \"hop_length\": 256, \"frames\": 184, \"mel_fmin_hz\": 100.0, \"mel_fmax_hz\": 8000.0,"
    " \"layout\": \"time_freq\", \"output_kind\": \"probability\", \"bird_output_first\": 1,"
    " \"bird_output_count\": 1, \"mel_bands\": 16, \"score_threshold\": 0.35,"
    " \"arena_bytes\": 32768, \"model_bytes\": 6384}";

static uint64_t total; // samples since capture started

static void put_model(void)
{
    static uint8_t tfl[6384];
    memcpy(tfl + 4, "TFL3", 4);
    put("models/m/model.json", MODEL_JSON, (uint32_t)strlen(MODEL_JSON), false);
    put("models/m/model.tflite", tfl, sizeof tfl, false);
}

// Boots the client as far as capturing in ST_RUN (live, as registered), with
// 9 s chunks and validation by model m if `model` (put on the card after).
// The card starts empty.
static void boot(bool model)
{
    for (int i = 0; i < 32; i++) {
        free(files[i].data);
        memset(&files[i], 0, sizeof files[i]);
    }
    answer = -1;
    has_queued = false;
    bps_validation_config v;
    bps_validation_default(&v);
    bps_validate_configure(&v); // a model an earlier test loaded is let go
    bps_validate_poll(now_us);
    bps_card_give(BPS_CARD_MODEL);
    bps_card_give(BPS_CARD_SPOOL);
    memset(&stats, 0, sizeof stats);
    memset(&mode, 0, sizeof mode);
    mode.interval_s = 300;
    mode.radio_on = true;
    memset(&f, 0, sizeof f);
    f.h = -1;
    memset(&st, 0, sizeof st);
    memset(&w, 0, sizeof w);
    on_request = NULL;
    reply = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
    room_per_loop = SIZE_MAX;
    server_up = true;
    wifi_up = true;
    model_fails = false;
    model_reads = 0;
    total = 0;

    static bps_settings s;
    s.wifi_ssid = "x";
    s.wifi_password = "";
    s.server_host = "server";
    s.server_port = 2473;
    s.name = "";
    s.client_id = "";
    s.device_name = "mic";
    s.board = "test";
    s.version = "t";
    s.sample_rate = RATE;
    s.ring_bytes = 1024 * 1024; // 10.9 s
    s.lookback_ms = 750;
    s.buses = (bps_bus_settings){-1, -1, -1, -1, -1, ""};
    bps_client_setup(&s);
    chunk_len = 9 * RATE;
    bps_trigger_config t;
    bps_trigger_default(&t);
    if (model) {
        v.type = BPS_VALIDATION_MODEL;
        strcpy(v.name, "model");
        strcpy(v.model, "m");
    }
    trigger_start(&t, &v);
    start_capture();
    bps_timeline tl = {0, 0, 0};
    bps_ring_timeline(&ring, &tl, true, 0);
    epoch_ns = 1762090000000000000LL; // set by the first clock reply
    epoch_set = true;
    bps_ts_sample smp = {0, 0, 1000000};
    bps_clock_add_sample(&clk, smp);
    hb_next_us = UINT64_MAX / 2; // no heartbeats
    state = ST_RUN;
}

// Captures `samples` (silence), as the capture core does, then runs the loop.
static void loop_once(uint32_t samples)
{
    static int16_t silence[4096];
    for (uint32_t n = samples; n > 0;) {
        uint32_t k = n < 4096 ? n : 4096;
        bps_ring_write(&ring, silence, k);
        total += k;
        n -= k;
        if (total % RATE < k) {
            bps_timeline tl = {(uint32_t)total, (int64_t)(total * BPS_NANOS_PER_SEC / RATE), 0};
            bps_ring_timeline(&ring, &tl, false, 0);
        }
    }
    now_us = 1000000 + total * 1000000 / RATE;
    room = room_per_loop;
    card_task();
    bps_client_loop();
}

// The loop every 10 ms for `ms`.
static void run_ms(uint32_t ms)
{
    for (uint32_t i = 0; i < ms / 10; i++)
        loop_once(10 * MS);
}

static void gate_pass(void)
{
    cap.last_pass_end = bps_ring_written(&ring) - 1000;
    cap.pass_count++;
}

void setUp(void) {}
void tearDown(void) {}

// ---- Tests ----

// Batch mode: a contact's access point does not answer for the 30 s it is
// given. The chunk being saved goes on being saved (uploads were polled only
// once registered, so audio past the ring's 10.9 s was lost, and the whole
// chunk deleted).
static void test_batch_mode_saves_audio_while_a_contact_connects(void)
{
    boot(false);
    mode.run = MODE_BATCH;
    bps_upload_set_spool(true);
    mode.next_contact_us = UINT64_MAX;
    state = ST_OFF;
    run_ms(3000);
    gate_pass();
    run_ms(2000);
    TEST_ASSERT_FALSE(bps_upload_idle()); // 2 s into saving a chunk

    wifi_up = false;
    mode.next_contact_us = now_us;
    for (int i = 0; i < 1000 && state == ST_OFF; i++)
        run_ms(10);
    TEST_ASSERT_EQUAL_INT(ST_WIFI, state);
    for (int i = 0; i < 4000 && state != ST_OFF; i++)
        run_ms(10);
    TEST_ASSERT_EQUAL_INT(ST_OFF, state); // gave up on the contact
    run_ms(10000);
    TEST_ASSERT_TRUE(bps_upload_idle());
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, count(".json"), "the chunk was not saved");
    TEST_ASSERT_EQUAL_INT(1, count(".flac"));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, stats.overruns, "audio was lost");
}

// Validation rejects every chunk: silence still counts once as gated chunks
// (each rejection counted every chunk since the last upload again).
static void test_rejected_chunks_count_once_as_gated(void)
{
    boot(true);
    put_model();
    for (int i = 0; i < 100 && !bps_validate_active(); i++)
        run_ms(10);
    TEST_ASSERT_TRUE(bps_validate_active());
    uint64_t start = total, gated = stats.chunks_gated;
    for (int minute = 1; minute <= 30; minute++) {
        if (minute % 5 == 0)
            gate_pass();
        for (int i = 0; i < 600; i++)
            loop_once(RATE / 10);
    }
    TEST_ASSERT_EQUAL_UINT64(6, stats.chunks_rejected);
    TEST_ASSERT_EQUAL_UINT64(0, stats.chunks_sent);
    uint64_t chunks = (total - start) / chunk_len;
    TEST_ASSERT_UINT64_WITHIN(2, chunks, stats.chunks_gated - gated);
}

// A stored chunk whose body must arrive as on the card.
static uint8_t flac[65536];
static int stored_uploads, damaged;
static void check_body(void)
{
    const uint8_t *body = NULL;
    for (size_t i = 0; i + 3 < request_len; i++)
        if (memcmp(request + i, "\r\n\r\n", 4) == 0) {
            body = request + i + 4;
            break;
        }
    if (!body || !strstr((const char *)request, "audio/flac"))
        return;
    stored_uploads++;
    size_t n = request_len - (size_t)(body - request);
    for (size_t i = 0; i < n; i++)
        if (body[i] != 0xAA) {
            damaged++;
            return;
        }
}

static void put_stored_chunk(const char *name, int64_t start_s)
{
    char path[48], meta[256];
    snprintf(meta, sizeof meta,
             "{\"epoch_ns\":1,\"start\":%lld,\"end\":%lld,\"start_est\":1,\"end_est\":2,"
             "\"bytes\":%u,\"dropped\":0}\n",
             (long long)(start_s * BPS_NANOS_PER_SEC), (long long)((start_s + 9) * BPS_NANOS_PER_SEC),
             (unsigned)sizeof flac);
    memset(flac, 0xAA, sizeof flac);
    snprintf(path, sizeof path, "spool/%s.json", name);
    put(path, meta, (uint32_t)strlen(meta), false);
    snprintf(path, sizeof path, "spool/%s.flac", name);
    put(path, flac, sizeof flac, false);
}

// A model that does not run is tried again every 30 s, while stored chunks go
// up (live mode). Its reads must wait for the spool's pass, as they share the
// card's one answer: the retry read model.json while the spool was sending
// from that answer (model.json went out inside a stored upload), and then
// took the spool's data for model.json.
static void test_stored_uploads_and_a_model_being_loaded_keep_to_their_reads(void)
{
    boot(true);
    put_model();
    put_stored_chunk("1000", 1);
    model_fails = true;
    run_ms(1000);
    const char *note = bps_validate_note();
    TEST_ASSERT_TRUE_MESSAGE(note && strstr(note, "does not run"), note ? note : "no note");

    room_per_loop = 50; // a slow link: the spool sends from the card's answer most of the time
    reply = "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\n\r\n"; // stays on the card
    on_request = check_body;
    stored_uploads = damaged = 0;
    mode.session_synced = true;
    int first_reads = model_reads;
    for (int i = 0; i < 2 * 60 * 100; i++) {
        if (!bps_spool_busy() && i % 50 == 0)
            mode.live_spool_us = 0; // another pass now
        run_ms(10);
        note = bps_validate_note();
        TEST_ASSERT_TRUE_MESSAGE(note && strstr(note, "does not run"), note ? note : "no note");
    }
    TEST_ASSERT_GREATER_THAN_INT(5, stored_uploads);
    /* Twice at least (each retry waits for the pass going on: 13 s here). */
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(first_reads + 6, model_reads, "the model was not tried again");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, damaged, "a stored upload carried bytes not its own");
}

// A pass asked for while a model loads waits until it has: it took the
// card's one answer, which the model then took for its own (a 4096-byte
// listing copied where it asked for less: past the model's buffer).
static void test_a_spool_pass_waits_for_a_model_to_load(void)
{
    boot(true);
    put_model();
    put_stored_chunk("1000", 1);
    for (int i = 0; i < 100 && !(has_queued && strcmp(queued.path, "models/m/model.tflite") == 0 &&
                                 queued.off == 4096);
         i++)
        loop_once(48);
    TEST_ASSERT_EQUAL_UINT32(4096, queued.off);
    card_task(); // answered before the loop collects it
    bps_spool_kick(); // as a contact's flush, or switching to live mode
    TEST_ASSERT_TRUE(bps_spool_busy());
    TEST_ASSERT_EQUAL_INT(F_IDLE, f.state); // not yet: the card is the model's
    for (int i = 0; i < 100 && !bps_validate_active(); i++)
        loop_once(48);
    TEST_ASSERT_TRUE_MESSAGE(bps_validate_active(), bps_validate_note() ? bps_validate_note() : "");
    TEST_ASSERT_EQUAL_UINT32(6384, bps_validate_model()->model_bytes);
    for (int i = 0; i < 100 && bps_spool_busy(); i++)
        loop_once(480);
    TEST_ASSERT_FALSE(bps_spool_busy());
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, count(".json"), "the stored chunk did not go up");
}

// The once-a-boot sweep for .flac files a restart left without their .json
// compares them with the listing taken when the pass began: a chunk saved
// since (batch mode saves while a contact uploads) is not an orphan.
static void test_a_chunk_saved_during_a_pass_is_not_swept_up(void)
{
    boot(false);
    char name[24];
    for (int k = 0; k < 3; k++) {
        bps_spool_name(epoch_ns - (100 - 10 * k) * BPS_NANOS_PER_SEC, name);
        put_stored_chunk(name, 1);
    }
    room_per_loop = 25; // 26 s a chunk: the pass outlasts the sweep's 60 s
    bps_spool_kick();
    run_ms(3000);
    // A chunk saved now (as upload.c does in batch mode), 9 s long.
    int64_t start = local_ns(now_us) - 9 * BPS_NANOS_PER_SEC;
    uint8_t head[42] = "fLaC";
    TEST_ASSERT_TRUE(bps_spool_begin(start, start + 9 * BPS_NANOS_PER_SEC, 1, 2, RATE, 9 * RATE, head,
                                     sizeof head));
    bps_spool_frame(flac, 4000);
    bps_spool_end(9 * RATE, 0);
    char path[48];
    bps_spool_name(start, name);
    snprintf(path, sizeof path, "spool/%s.flac", name);
    for (int i = 0; i < 20000 && bps_spool_busy(); i++)
        run_ms(10);
    TEST_ASSERT_FALSE(bps_spool_busy());
    TEST_ASSERT_TRUE(st.orphans_checked);
    TEST_ASSERT_NOT_NULL_MESSAGE(find(path), "its .flac was swept up");
    room_per_loop = SIZE_MAX;
    bps_spool_kick();
    for (int i = 0; i < 1000 && bps_spool_busy(); i++)
        run_ms(10);
    TEST_ASSERT_EQUAL_INT(0, count(".json"));
    TEST_ASSERT_EQUAL_INT(0, count(".flac"));
}

// The registration says which build the client is and which API version it
// speaks, so the server can tell a unit running old firmware.
static void test_registration_reports_the_version_and_api_version(void)
{
    boot(false);
    cfg.version = "bps-mcu 1a2b3c4-dirty";
    room = SIZE_MAX;
    start_register();
    for (int i = 0; i < 20 && request_len == 0; i++)
        txn_poll(&reg_txn);
    TEST_ASSERT_GREATER_THAN(0, request_len);
    request[request_len < sizeof request ? request_len : sizeof request - 1] = 0;
    const char *body = strstr((const char *)request, "\r\n\r\n");
    TEST_ASSERT_NOT_NULL(body);
    char v[48];
    TEST_ASSERT_TRUE(bps_json_string(body, strlen(body), "version", v, sizeof v));
    TEST_ASSERT_EQUAL_STRING("bps-mcu 1a2b3c4-dirty", v);
    TEST_ASSERT_TRUE(bps_json_string(body, strlen(body), "api_version", v, sizeof v));
    TEST_ASSERT_EQUAL_STRING(BPS_API_VERSION, v);
    txn_reset(&reg_txn);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_registration_reports_the_version_and_api_version);
    RUN_TEST(test_batch_mode_saves_audio_while_a_contact_connects);
    RUN_TEST(test_rejected_chunks_count_once_as_gated);
    RUN_TEST(test_stored_uploads_and_a_model_being_loaded_keep_to_their_reads);
    RUN_TEST(test_a_spool_pass_waits_for_a_model_to_load);
    RUN_TEST(test_a_chunk_saved_during_a_pass_is_not_swept_up);
    return UNITY_END();
}
