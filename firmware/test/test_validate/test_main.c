// model.json, as trigger-models/ writes it, read by validate.c.
#include <unity.h>

#include <stdio.h>
#include <string.h>

#include "card.h"
#include "port.h"
#include "validate.h"

// The port, as far as validate.c reaches it: a card holding the files in
// `card`, as the ESP32 port has it (one read at a time, refused while one is
// pending, and its answer kept until the next), whose reads finish when
// polled unless `hold_reads`; a worker that runs each job when kicked; an
// interpreter that loads anything.
#include <stdlib.h>
void port_log(const char *fmt, ...) { (void)fmt; }
uint64_t port_mono_us(void) { return 0; }
static int live_allocs; // big buffers not freed
void *port_big_alloc(size_t n)
{
    void *p = malloc(n);
    live_allocs += p != NULL;
    return p;
}
void port_big_free(void *p)
{
    live_allocs -= p != NULL;
    free(p);
}

typedef struct {
    const char *path;
    const void *data;
    uint32_t len;
} card_file;
static card_file card[4];
static bool card_present, hold_reads, pending;
static int reads;           // asked for
static int answer_len = -1; // else every answer is this long (another read's)
static char read_path[96];
static uint32_t read_off;
static size_t read_len;
static int result = -1;
static const void *result_data;
static uint32_t result_total;

bool port_sd_read(const char *p, uint32_t o, size_t l)
{
    if (!card_present || pending)
        return false;
    snprintf(read_path, sizeof read_path, "%s", p);
    read_off = o;
    read_len = l;
    pending = true;
    reads++;
    return true;
}

// The card's task.
static void answer(void)
{
    static uint8_t other[PORT_SD_READ_MAX];
    pending = false;
    result = -1;
    if (answer_len >= 0) {
        result_data = other;
        result = answer_len;
        return;
    }
    for (int i = 0; i < 4; i++) {
        const card_file *f = &card[i];
        if (!f->path || strcmp(f->path, read_path) != 0)
            continue;
        uint32_t left = read_off < f->len ? f->len - read_off : 0;
        result_data = (const uint8_t *)f->data + read_off;
        result_total = f->len;
        result = (int)(left < read_len ? left : read_len);
    }
}

int port_sd_result(const void **d, uint32_t *t)
{
    if (pending && !hold_reads)
        answer();
    if (pending)
        return PORT_SD_PENDING;
    *d = result_data;
    *t = result_total;
    return result;
}
void port_sd_info_get(port_sd_info *o)
{
    memset(o, 0, sizeof *o);
    o->present = card_present;
}

static void (*worker)(void);
bool port_worker_start(void (*fn)(void))
{
    worker = fn;
    return fn != NULL;
}
void port_worker_kick(void)
{
    if (worker)
        worker();
}

static size_t loaded_bytes;
static uint8_t loaded_tag, in_tensor[16 * 184];
static int8_t out_tensor[2];
bool port_ml_load(const void *m, size_t b, size_t a, port_ml_tensor *i, port_ml_tensor *o, size_t *u,
                  char *e, size_t c)
{
    (void)a, (void)e, (void)c;
    loaded_bytes = b;
    loaded_tag = ((const uint8_t *)m)[8];
    port_ml_tensor in = {true, in_tensor, sizeof in_tensor, 1.0f, 0};
    port_ml_tensor out = {true, out_tensor, sizeof out_tensor, 1.0f / 256, -128};
    *i = in;
    *o = out;
    *u = 1000;
    return true;
}
static void (*on_invoke)(void); // while the model runs
bool port_ml_invoke(void)
{
    if (on_invoke)
        on_invoke();
    return true;
}
void port_ml_unload(void) {}

void setUp(void) {}
void tearDown(void) {}

static const char *MICRO =
    "{\n"
    "  \"model_name\": \"drongonet-micro\",\n"
    "  \"model_file\": \"model.tflite\",\n"
    "  \"sample_rate\": 16000,\n"
    "  \"window_samples\": 48000,\n"
    "  \"frontend\": \"logmel_db_minmax\",\n"
    "  \"fft_length\": 1024,\n"
    "  \"hop_length\": 256,\n"
    "  \"frames\": 184,\n"
    "  \"mel_fmin_hz\": 100.0,\n"
    "  \"mel_fmax_hz\": 8000.0,\n"
    "  \"layout\": \"time_freq\",\n"
    "  \"output_kind\": \"probability\",\n"
    "  \"bird_output_first\": 1,\n"
    "  \"bird_output_count\": 1,\n"
    "  \"license\": \"CC-BY-4.0\",\n"
    "  \"source\": \"https://github.com/mun3im/drongonet (deploy/, commit 93f67b7)\",\n"
    "  \"mel_bands\": 16,\n"
    "  \"score_threshold\": 0.35,\n"
    "  \"arena_bytes\": 32768,\n"
    "  \"measured_arena_used_bytes\": 25948,\n"
    "  \"measured_invoke_ms_esp32s3_160mhz\": 47.4,\n"
    "  \"model_bytes\": 6384\n"
    "}\n"
    "";

static const char *BIRDNET =
    "{\n"
    "  \"model_name\": \"birdnet-tiny-2.0-hybrid\",\n"
    "  \"model_file\": \"model.tflite\",\n"
    "  \"sample_rate\": 24000,\n"
    "  \"window_samples\": 60000,\n"
    "  \"frontend\": \"stft_sqrt_minmax\",\n"
    "  \"fft_length\": 512,\n"
    "  \"frames\": 384,\n"
    "  \"layout\": \"freq_time\",\n"
    "  \"output_kind\": \"logit\",\n"
    "  \"bird_output_first\": 0,\n"
    "  \"bird_output_count\": 1007,\n"
    "  \"score_threshold\": 0.25,\n"
    "  \"arena_bytes\": 1048576,\n"
    "  \"license\": \"Apache-2.0\",\n"
    "  \"source\": \"https://github.com/birdnet-team/birdnet-stm32/releases/tag/v2.0 (BirdNET_Tiny_N6_GLOBAL_1K_V2.0_Hybrid_INT8.tflite)\",\n"
    "  \"measured_arena_used_bytes\": 932984,\n"
    "  \"measured_invoke_ms_esp32s3_160mhz\": 1584.3,\n"
    "  \"model_bytes\": 2063624\n"
    "}\n"
    "";

static void test_reads_a_logmel_model(void)
{
    bps_model_config m;
    const char *why;
    TEST_ASSERT_TRUE_MESSAGE(bps_model_config_parse(MICRO, strlen(MICRO), &m, &why), why ? why : "");
    TEST_ASSERT_EQUAL_STRING("drongonet-micro", m.name);
    TEST_ASSERT_EQUAL_STRING("model.tflite", m.file);
    TEST_ASSERT_EQUAL_INT(BPS_FE_LOGMEL_DB_MINMAX, m.fe.kind);
    TEST_ASSERT_EQUAL_UINT32(16000, m.fe.sample_rate);
    TEST_ASSERT_EQUAL_UINT32(48000, m.fe.window_samples);
    TEST_ASSERT_EQUAL_UINT32(1024, m.fe.fft_length);
    TEST_ASSERT_EQUAL_UINT32(256, m.fe.hop_length);
    TEST_ASSERT_EQUAL_UINT32(184, m.fe.frames);
    TEST_ASSERT_EQUAL_UINT32(16, m.fe.mel_bands);
    TEST_ASSERT_EQUAL_FLOAT(100.0f, m.fe.mel_fmin_hz);
    TEST_ASSERT_TRUE(m.fe.time_major);
    TEST_ASSERT_FALSE(m.output_logit);
    TEST_ASSERT_EQUAL_UINT32(1, m.bird_first);
    TEST_ASSERT_EQUAL_UINT32(1, m.bird_count);
    TEST_ASSERT_EQUAL_FLOAT(0.35f, m.threshold);
    TEST_ASSERT_EQUAL_UINT32(6384, m.model_bytes);
}

static void test_reads_a_stft_logit_model(void)
{
    bps_model_config m;
    const char *why;
    TEST_ASSERT_TRUE_MESSAGE(bps_model_config_parse(BIRDNET, strlen(BIRDNET), &m, &why), why ? why : "");
    TEST_ASSERT_EQUAL_INT(BPS_FE_STFT_SQRT_MINMAX, m.fe.kind);
    TEST_ASSERT_EQUAL_UINT32(24000, m.fe.sample_rate);
    TEST_ASSERT_EQUAL_UINT32(384, m.fe.frames);
    TEST_ASSERT_FALSE(m.fe.time_major);
    TEST_ASSERT_TRUE(m.output_logit);
    TEST_ASSERT_EQUAL_UINT32(1007, m.bird_count);
    TEST_ASSERT_EQUAL_UINT32(1048576, m.arena_bytes);
}

static void test_says_what_is_wrong(void)
{
    bps_model_config m;
    const char *why;
    char bad[2048];
    strcpy(bad, MICRO);
    char *p = strstr(bad, "logmel_db_minmax");
    memcpy(p, "mfcc_something__", 16);
    TEST_ASSERT_FALSE(bps_model_config_parse(bad, strlen(bad), &m, &why));
    TEST_ASSERT_NOT_NULL(strstr(why, "front end"));
    strcpy(bad, MICRO);
    p = strstr(bad, "\"sample_rate\": 16000");
    memcpy(p + 15, "44100", 5);
    TEST_ASSERT_FALSE(bps_model_config_parse(bad, strlen(bad), &m, &why));
    TEST_ASSERT_NOT_NULL(strstr(why, "sample rate"));
    const char *empty = "{}";
    TEST_ASSERT_FALSE(bps_model_config_parse(empty, strlen(empty), &m, &why));
}

// The server switched models (a, then b) while model a's model.json was still
// being read: that read's answer must not be taken for model b's, or b's
// model.tflite is read to a's size (a model cut short, or overrun) and run
// with a's settings.
static void test_a_read_for_an_earlier_model_is_dropped(void)
{
    static char json_b[2048];
    snprintf(json_b, sizeof json_b, "%s", MICRO);
    char *p = strstr(json_b, "\"model_bytes\": 6384");
    memcpy(p, "\"model_bytes\": 8000", 19);
    static uint8_t tfl_a[6384], tfl_b[8000];
    memcpy(tfl_a + 4, "TFL3", 4);
    memcpy(tfl_b + 4, "TFL3", 4);
    tfl_a[8] = 'a';
    tfl_b[8] = 'b';
    card_file files[4] = {{"models/a/model.json", MICRO, (uint32_t)strlen(MICRO)},
                          {"models/a/model.tflite", tfl_a, sizeof tfl_a},
                          {"models/b/model.json", json_b, (uint32_t)strlen(json_b)},
                          {"models/b/model.tflite", tfl_b, sizeof tfl_b}};
    memcpy(card, files, sizeof card);
    card_present = true;

    static bps_ring ring;
    bps_ring_alloc(&ring, 48000, 2 * BPS_RING_BLOCK);
    bps_validate_init(&ring, 48000);
    bps_validation_config want;
    bps_validation_default(&want);
    want.type = BPS_VALIDATION_MODEL;
    want.threshold = -1;
    strcpy(want.model, "a");
    bps_validate_configure(&want);
    hold_reads = true;
    bps_validate_poll(0);
    TEST_ASSERT_EQUAL_STRING("models/a/model.json", read_path);

    strcpy(want.model, "b");
    bps_validate_configure(&want);
    hold_reads = false;
    for (int i = 0; i < 100 && !bps_validate_active(); i++)
        bps_validate_poll(0);
    TEST_ASSERT_TRUE_MESSAGE(bps_validate_active(), bps_validate_note() ? bps_validate_note() : "");
    TEST_ASSERT_EQUAL_UINT32(8000, bps_validate_model()->model_bytes);
    TEST_ASSERT_EQUAL_UINT32(8000, (uint32_t)loaded_bytes);
    TEST_ASSERT_EQUAL_UINT8('b', loaded_tag);
}

static bps_ring ring;
static uint8_t tfl_m[6384]; // read in two: 4096 bytes, then 2288

// Model m on the card, validation off, and whatever an earlier test loaded
// let go.
static void fresh(void)
{
    hold_reads = false;
    answer_len = -1;
    on_invoke = NULL;
    bps_validation_config off;
    bps_validation_default(&off);
    bps_validate_configure(&off);
    for (int i = 0; i < 10; i++)
        bps_validate_poll(0);
    memcpy(tfl_m + 4, "TFL3", 4);
    card_file files[4] = {{"models/m/model.json", MICRO, (uint32_t)strlen(MICRO)},
                          {"models/m/model.tflite", tfl_m, sizeof tfl_m}};
    memcpy(card, files, sizeof card);
    card_present = true;
    if (!ring.capacity)
        bps_ring_alloc(&ring, 48000, 32 * BPS_RING_BLOCK);
    bps_validate_init(&ring, 48000);
    reads = 0;
}

static void want_model(const char *name, float threshold)
{
    bps_validation_config want;
    bps_validation_default(&want);
    want.type = BPS_VALIDATION_MODEL;
    want.threshold = threshold;
    strcpy(want.model, name);
    bps_validate_configure(&want);
}

static void load(void)
{
    for (int i = 0; i < 20 && !bps_validate_active(); i++)
        bps_validate_poll(0);
    TEST_ASSERT_TRUE_MESSAGE(bps_validate_active(), bps_validate_note() ? bps_validate_note() : "");
}

// Validation turned off while the model is being read: that read finishes,
// and the model's buffer is let go (it stayed: up to 2 MB of PSRAM).
static void test_turned_off_while_loading_lets_go_of_the_model(void)
{
    fresh();
    int before = live_allocs;
    want_model("m", -1);
    bps_validate_poll(0); // asks for model.json
    bps_validate_poll(0); // has it; asks for the model's first piece
    TEST_ASSERT_EQUAL_STRING("models/m/model.tflite", read_path);
    TEST_ASSERT_EQUAL_INT(before + 1, live_allocs);

    hold_reads = true;
    bps_validation_config off;
    bps_validation_default(&off);
    bps_validate_configure(&off);
    bps_validate_poll(0);
    hold_reads = false;
    for (int i = 0; i < 10; i++)
        bps_validate_poll(0);
    TEST_ASSERT_EQUAL_INT_MESSAGE(before, live_allocs, "the model's buffer is kept");
    TEST_ASSERT_EQUAL_INT(BPS_CARD_FREE, bps_card_holder());
}

static void raise_threshold(void)
{
    on_invoke = NULL;
    want_model("m", 0.9f);
}

// The server raises the threshold while a check runs on the worker: the
// check wrote back the threshold it started with, and the change was lost
// (the heartbeat went on reporting the old one, and checks used it).
static void test_a_threshold_changed_during_a_check_is_kept(void)
{
    fresh();
    want_model("m", -1); // model.json's 0.35
    load();
    static int16_t silence[4096];
    while (bps_ring_written(&ring) < 300000)
        bps_ring_write(&ring, silence, 4096);
    out_tensor[1] = 0; // scores 0.5
    on_invoke = raise_threshold;
    TEST_ASSERT_TRUE(bps_validate_start(bps_ring_written(&ring) - 200000));
    bps_validate_result r;
    TEST_ASSERT_TRUE(bps_validate_take(&r));
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_EQUAL_FLOAT(0.5f, r.score);
    TEST_ASSERT_EQUAL_FLOAT(0.9f, r.threshold);
    TEST_ASSERT_FALSE(r.upload);
    bps_validation_config running;
    bps_validate_running(&running);
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.9f, running.threshold, "the new threshold was lost");
}

// The card's reads share one answer: while the spool's pass holds them, the
// model waits; while the model loads, the spool does (a spool pass kicked
// mid-load overwrote the model's answer, or had it copied into the model).
static void test_a_model_loads_once_the_spool_lets_go_of_the_card(void)
{
    fresh();
    TEST_ASSERT_TRUE(bps_card_take(BPS_CARD_SPOOL));
    want_model("m", -1);
    for (int i = 0; i < 5; i++)
        bps_validate_poll(0);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, reads, "read the card while the spool held it");
    bps_card_give(BPS_CARD_SPOOL);
    hold_reads = true;
    bps_validate_poll(0);
    TEST_ASSERT_EQUAL_INT(1, reads);
    TEST_ASSERT_FALSE_MESSAGE(bps_card_take(BPS_CARD_SPOOL), "the spool may read while a model loads");
    hold_reads = false;
    load();
    TEST_ASSERT_EQUAL_INT(BPS_CARD_FREE, bps_card_holder());
}

// An answer longer than the read asked for is another read's: it is not
// copied into the model's buffer, which it would overrun.
static void test_an_answer_longer_than_asked_for_is_not_taken(void)
{
    fresh();
    want_model("m", -1);
    bps_validate_poll(0); // asks for model.json
    bps_validate_poll(0); // has it; asks for the model's first 4096 bytes
    bps_validate_poll(0); // has them; asks for the last 2288
    TEST_ASSERT_EQUAL_UINT32(4096, read_off);
    TEST_ASSERT_EQUAL_UINT32(2288, (uint32_t)read_len);
    answer_len = PORT_SD_READ_MAX;
    bps_validate_poll(0);
    TEST_ASSERT_FALSE(bps_validate_active());
    TEST_ASSERT_NOT_NULL(bps_validate_note());
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(bps_validate_note(), "another read"), bps_validate_note());
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_reads_a_logmel_model);
    RUN_TEST(test_reads_a_stft_logit_model);
    RUN_TEST(test_says_what_is_wrong);
    RUN_TEST(test_a_read_for_an_earlier_model_is_dropped);
    RUN_TEST(test_turned_off_while_loading_lets_go_of_the_model);
    RUN_TEST(test_a_threshold_changed_during_a_check_is_kept);
    RUN_TEST(test_a_model_loads_once_the_spool_lets_go_of_the_card);
    RUN_TEST(test_an_answer_longer_than_asked_for_is_not_taken);
    return UNITY_END();
}
