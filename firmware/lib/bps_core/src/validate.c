#include "validate.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "card.h"
#include "port.h"
#include "proto.h"

#define STORE(p, v) __atomic_store_n(p, v, __ATOMIC_RELEASE)
#define LOAD(p) __atomic_load_n(p, __ATOMIC_ACQUIRE)

/* A card that is not there, or a model that is not on it, is looked for again
 * this often. */
#define RETRY_US (30ull * 1000000)

/* ---- model.json ---- */

static bool num(const char *json, size_t len, const char *key, double *v) { return bps_json_number(json, len, key, v); }

bool bps_model_config_parse(const char *json, size_t len, bps_model_config *out, const char **why)
{
    bps_model_config m;
    memset(&m, 0, sizeof m);
    char s[24];
    double sr, win, fft, frames, first, count, thr, arena, bytes, hop = 0, mels = 0, fmin = 0, fmax = 0;
    *why = NULL;
    if (!bps_json_string(json, len, "model_name", m.name, sizeof m.name) ||
        !bps_json_string(json, len, "model_file", m.file, sizeof m.file))
        return *why = "model.json has no model_name or model_file", false;
    if (!num(json, len, "sample_rate", &sr) || !num(json, len, "window_samples", &win) ||
        !num(json, len, "fft_length", &fft) || !num(json, len, "frames", &frames) ||
        !num(json, len, "bird_output_first", &first) || !num(json, len, "bird_output_count", &count) ||
        !num(json, len, "score_threshold", &thr) || !num(json, len, "arena_bytes", &arena) ||
        !num(json, len, "model_bytes", &bytes))
        return *why = "model.json lacks a setting (sample_rate, window_samples, fft_length, frames, "
                      "bird_output_first/count, score_threshold, arena_bytes, model_bytes)",
               false;
    if (!bps_json_string(json, len, "frontend", s, sizeof s))
        return *why = "model.json has no frontend", false;
    if (strcmp(s, "logmel_db_minmax") == 0) {
        m.fe.kind = BPS_FE_LOGMEL_DB_MINMAX;
        if (!num(json, len, "hop_length", &hop) || !num(json, len, "mel_bands", &mels) ||
            !num(json, len, "mel_fmin_hz", &fmin) || !num(json, len, "mel_fmax_hz", &fmax))
            return *why = "model.json lacks hop_length or the mel settings", false;
    } else if (strcmp(s, "stft_sqrt_minmax") == 0) {
        m.fe.kind = BPS_FE_STFT_SQRT_MINMAX;
    } else {
        return *why = "model.json names a front end this firmware does not have", false;
    }
    if (!bps_json_string(json, len, "layout", s, sizeof s) ||
        (strcmp(s, "time_freq") != 0 && strcmp(s, "freq_time") != 0))
        return *why = "model.json layout is not time_freq or freq_time", false;
    m.fe.time_major = strcmp(s, "time_freq") == 0;
    if (!bps_json_string(json, len, "output_kind", s, sizeof s) ||
        (strcmp(s, "probability") != 0 && strcmp(s, "logit") != 0))
        return *why = "model.json output_kind is not probability or logit", false;
    m.output_logit = strcmp(s, "logit") == 0;
    m.fe.sample_rate = (uint32_t)sr;
    m.fe.window_samples = (uint32_t)win;
    m.fe.fft_length = (uint32_t)fft;
    m.fe.frames = (uint32_t)frames;
    m.fe.hop_length = (uint32_t)hop;
    m.fe.mel_bands = (uint32_t)mels;
    m.fe.mel_fmin_hz = (float)fmin;
    m.fe.mel_fmax_hz = (float)fmax;
    m.bird_first = (uint32_t)first;
    m.bird_count = (uint32_t)count;
    m.threshold = (float)thr;
    m.arena_bytes = (uint32_t)arena;
    m.model_bytes = (uint32_t)bytes;
    if (m.fe.sample_rate == 0 || 48000 % m.fe.sample_rate || 48000 / m.fe.sample_rate > 3)
        return *why = "the model's sample rate is not 48, 24 or 16 kHz", false;
    if (!m.bird_count || m.model_bytes < 16 || m.arena_bytes < 1024 || !(m.threshold >= 0 && m.threshold <= 1))
        return *why = "model.json has an unusable output range, size, arena or threshold", false;
    *out = m;
    return true;
}

/* ---- State ---- */

typedef enum { V_OFF, V_JSON, V_MODEL, V_INIT, V_READY, V_FAILED } vstate;
enum { JOB_NONE, JOB_INIT, JOB_CHECK };

static struct {
    const bps_ring *ring;
    uint32_t rate;
    bool worker;
    vstate st;
    bps_validation_config want;
    char note[96];
    uint64_t retry_us;
    bool reading;
    bool stale_read; /* the read in progress is for a model no longer wanted */
    uint32_t asked;  /* bytes it asked for */
    char path[96];
    /* The model. */
    bps_model_config m;
    uint8_t *model;
    uint32_t got;
    /* Running it. */
    uint32_t factor, window48, margin;
    bps_decimator dec;
    bps_frontend *fe; /* allocated with the model: boards that never load one keep the RAM */
    int16_t *pcm;
    float *x, *feat;
    bool feat_in_tensor; /* feat is the model's (float) input: not ours to free */
    port_ml_tensor in, out;
    size_t arena_used;
    bool loaded; /* port_ml_load succeeded */
    /* Worker hand-over: job set by the loop, done_seq moved on by the worker. */
    uint32_t job, req_seq, done_seq, taken_seq;
    bool init_ok;
    char init_err[64];
    uint32_t test_us;
    bps_validate_result res;
    /* Set by the loop only, and applied when a check is taken: a change while
     * one runs is not lost. */
    float threshold;
} v;

static void set_note(const char *fmt, const char *a, const char *b)
{
    snprintf(v.note, sizeof v.note, fmt, a, b);
}

static void release(void)
{
    if (v.loaded)
        port_ml_unload();
    v.loaded = false;
    if (v.fe)
        bps_frontend_free(v.fe);
    port_big_free(v.fe);
    v.fe = NULL;
    port_big_free(v.model);
    port_big_free(v.pcm);
    port_big_free(v.x);
    if (!v.feat_in_tensor)
        port_big_free(v.feat);
    v.feat_in_tensor = false;
    v.model = NULL;
    v.pcm = NULL;
    v.x = v.feat = NULL;
}

static void fail(uint64_t now, const char *why, const char *detail)
{
    release();
    v.st = V_FAILED;
    v.reading = false;
    v.retry_us = now + RETRY_US;
    char note[sizeof v.note];
    snprintf(note, sizeof note, "model %s: %s%s%s", v.want.model, why, detail ? ": " : "",
             detail ? detail : "");
    /* Retries that fail the same way are not logged again. */
    if (strcmp(note, v.note) != 0)
        port_log("validation: %s; every recording is uploaded", note);
    strcpy(v.note, note);
}

/* ---- Worker side ---- */

static float score_of(void)
{
    const port_ml_tensor *o = &v.out;
    uint32_t n = (uint32_t)(o->is_int8 ? o->bytes : o->bytes / 4);
    uint32_t last = v.m.bird_first + v.m.bird_count;
    if (last > n)
        last = n;
    float best = 0.0f;
    for (uint32_t i = v.m.bird_first; i < last; i++) {
        float y = o->is_int8 ? ((int8_t *)o->data)[i] - o->zero_point : ((float *)o->data)[i];
        if (o->is_int8)
            y *= o->scale;
        if (v.m.output_logit)
            y = 1.0f / (1.0f + expf(-y));
        if (i == v.m.bird_first || y > best)
            best = y;
    }
    return best;
}

/* The front end's output into the model's input tensor. */
static void fill_input(const float *feat, uint32_t n)
{
    if (!v.in.is_int8) {
        if (feat != v.in.data)
            memcpy(v.in.data, feat, n * sizeof(float));
        return;
    }
    int8_t *q = v.in.data;
    float inv = 1.0f / v.in.scale;
    for (uint32_t i = 0; i < n; i++) {
        long r = lroundf(feat[i] * inv) + v.in.zero_point;
        q[i] = (int8_t)(r < -128 ? -128 : r > 127 ? 127 : r);
    }
}

static void run_check(void)
{
    bps_validate_result r;
    memset(&r, 0, sizeof r);
    r.window_start = v.res.window_start;
    r.window_ms = v.m.fe.window_samples * 1000 / v.m.fe.sample_rate;
    uint64_t t0 = port_mono_us();
    uint32_t total = v.window48 + 2 * v.margin;
    if (bps_ring_read(v.ring, r.window_start - v.margin, v.pcm, total)) {
        if (v.factor == 1) {
            for (uint32_t i = 0; i < v.m.fe.window_samples; i++)
                v.x[i] = v.pcm[i] * (1.0f / 32768.0f);
        } else {
            bps_decimate(&v.dec, v.pcm, v.x, v.m.fe.window_samples);
        }
        r.resample_us = (uint32_t)(port_mono_us() - t0);
        uint32_t outs = bps_frontend_outputs(v.fe);
        bps_frontend_run(v.fe, v.x, v.feat);
        fill_input(v.feat, outs);
        uint64_t t1 = port_mono_us();
        r.ok = port_ml_invoke();
        uint64_t t2 = port_mono_us();
        r.frontend_us = (uint32_t)(t1 - t0);
        r.invoke_us = (uint32_t)(t2 - t1);
        if (r.ok)
            r.score = score_of();
    }
    v.res = r;
}

static void run_init(void)
{
    v.init_ok = port_ml_load(v.model, v.m.model_bytes, v.m.arena_bytes, &v.in, &v.out,
                             &v.arena_used, v.init_err, sizeof v.init_err);
    if (!v.init_ok)
        return;
    v.loaded = true;
    uint32_t outs = bps_frontend_outputs(v.fe);
    size_t want = v.in.is_int8 ? outs : outs * sizeof(float);
    if (v.in.bytes != want) {
        snprintf(v.init_err, sizeof v.init_err, "its input holds %u bytes, the front end gives %u",
                 (unsigned)v.in.bytes, (unsigned)want);
        v.init_ok = false;
        return;
    }
    size_t n_out = v.out.is_int8 ? v.out.bytes : v.out.bytes / 4;
    if (v.m.bird_first + v.m.bird_count > n_out) {
        snprintf(v.init_err, sizeof v.init_err, "it has %u outputs, model.json names %u",
                 (unsigned)n_out, (unsigned)(v.m.bird_first + v.m.bird_count));
        v.init_ok = false;
        return;
    }
    /* A float input the front end can fill directly. */
    if (!v.in.is_int8) {
        port_big_free(v.feat);
        v.feat = v.in.data;
        v.feat_in_tensor = true;
    }
    /* A test run on silence, timed. */
    memset(v.in.data, 0, v.in.bytes);
    uint64_t t = port_mono_us();
    v.init_ok = port_ml_invoke();
    v.test_us = (uint32_t)(port_mono_us() - t);
    if (!v.init_ok)
        snprintf(v.init_err, sizeof v.init_err, "the test run failed");
}

static void work(void)
{
    uint32_t job = LOAD(&v.job);
    if (job == JOB_INIT)
        run_init();
    else if (job == JOB_CHECK)
        run_check();
    STORE(&v.job, (uint32_t)JOB_NONE);
    STORE(&v.done_seq, v.req_seq);
}

static bool worker_idle(void) { return LOAD(&v.done_seq) == v.req_seq; }

static void post(uint32_t job)
{
    v.req_seq++;
    STORE(&v.job, job);
    port_worker_kick();
}

/* ---- Loop side ---- */

void bps_validate_init(const bps_ring *ring, uint32_t sample_rate)
{
    v.ring = ring;
    v.rate = sample_rate;
    bps_validation_default(&v.want);
}

void bps_validate_configure(const bps_validation_config *want)
{
    if (bps_validation_equal(want, &v.want))
        return;
    bool same_model = v.want.type == BPS_VALIDATION_MODEL && want->type == BPS_VALIDATION_MODEL &&
                      strcmp(v.want.model, want->model) == 0;
    v.want = *want;
    if (same_model) {
        v.threshold = want->threshold >= 0 ? want->threshold : v.m.threshold;
        return; /* only the threshold changed */
    }
    /* A model in use is let go once the worker is done with it (poll). */
    v.st = want->type == BPS_VALIDATION_MODEL ? V_JSON : V_OFF;
    /* A read in progress answers for the earlier model: dropped when done. */
    if (v.reading)
        v.stale_read = true;
    v.retry_us = 0;
    v.note[0] = 0;
    if (want->type == BPS_VALIDATION_MODEL)
        set_note("model %s: loading%s", want->model, "");
}

static bool read_next(const char *what, uint32_t off)
{
    snprintf(v.path, sizeof v.path, "models/%s/%s", v.want.model, what);
    uint32_t left = v.st == V_MODEL ? v.m.model_bytes - off : PORT_SD_READ_MAX;
    v.asked = left < PORT_SD_READ_MAX ? left : PORT_SD_READ_MAX;
    v.reading = port_sd_read(v.path, off, v.asked);
    return v.reading;
}

static void finish_load(uint64_t now)
{
    const bps_model_config *m = &v.m;
    if (memcmp(v.model + 4, "TFL3", 4) != 0)
        return fail(now, "model.tflite is not a TFLite model", NULL);
    v.factor = v.rate / m->fe.sample_rate;
    if (v.rate % m->fe.sample_rate || v.factor > 3)
        return fail(now, "its sample rate does not divide the capture's", NULL);
    if (v.factor > 1 && !bps_decimator_init(&v.dec, v.factor))
        return fail(now, "no resampler for its sample rate", NULL);
    v.fe = port_big_alloc(sizeof *v.fe);
    if (!v.fe)
        return fail(now, "not enough memory for its front end", NULL);
    if (!bps_frontend_init(v.fe, &m->fe)) {
        port_big_free(v.fe); /* nothing of it to free yet */
        v.fe = NULL;
        return fail(now, "unusable front end settings in model.json", NULL);
    }
    v.margin = v.factor > 1 ? bps_decimator_margin(&v.dec) : 0;
    v.window48 = m->fe.window_samples * v.factor;
    v.pcm = port_big_alloc((v.window48 + 2 * v.margin) * sizeof(int16_t));
    v.x = port_big_alloc(m->fe.window_samples * sizeof(float));
    v.feat = port_big_alloc(bps_frontend_outputs(v.fe) * sizeof(float));
    if (!v.pcm || !v.x || !v.feat)
        return fail(now, "not enough memory for its buffers", NULL);
    v.st = V_INIT;
    post(JOB_INIT);
}

static void poll(uint64_t now)
{
    if (!v.worker) {
        v.worker = port_worker_start(work);
        if (!v.worker && v.want.type == BPS_VALIDATION_MODEL && v.st != V_FAILED) {
            v.st = V_FAILED;
            v.retry_us = UINT64_MAX;
            snprintf(v.note, sizeof v.note, "this board cannot run models");
        }
        if (!v.worker)
            return;
    }
    const void *data;
    uint32_t total;
    /* A read for a model no longer wanted: dropped once done (whatever is
     * wanted now), so the model it was for can be let go. */
    if (v.reading && v.stale_read) {
        if (port_sd_result(&data, &total) == PORT_SD_PENDING)
            return;
        v.reading = false;
        v.stale_read = false;
    }
    /* Let go of a model that is no longer wanted (or is to be reloaded). */
    if ((v.st == V_OFF || v.st == V_JSON) && (v.model || v.loaded) && !v.reading) {
        if (!worker_idle())
            return;
        release();
        port_log("validation: model unloaded");
    }
    if (v.st == V_FAILED && v.want.type == BPS_VALIDATION_MODEL && now >= v.retry_us)
        v.st = V_JSON;
    if (v.st == V_OFF || v.st == V_FAILED || v.st == V_READY)
        return;

    if (v.st == V_INIT) {
        if (!worker_idle())
            return;
        if (!v.init_ok)
            return fail(now, "it does not run", v.init_err);
        v.threshold = v.want.threshold >= 0 ? v.want.threshold : v.m.threshold;
        v.taken_seq = v.req_seq; /* the set-up run is not a check */
        v.st = V_READY;
        v.note[0] = 0;
        port_log("validation: model %s loaded (%lu KB, arena %lu KB used), test run %lu ms, "
                 "threshold %ld.%02ld",
                 v.m.name, (unsigned long)(v.m.model_bytes / 1024),
                 (unsigned long)(v.arena_used / 1024), (unsigned long)(v.test_us / 1000),
                 (long)lroundf(v.threshold * 100) / 100, (long)lroundf(v.threshold * 100) % 100);
        return;
    }

    /* Reading the card. */
    if (v.reading) {
        int n = port_sd_result(&data, &total);
        if (n == PORT_SD_PENDING)
            return;
        v.reading = false;
        if (n < 0)
            return fail(now, v.st == V_JSON ? "no models/<name>/model.json on the card" : "reading the model failed",
                        NULL);
        if ((uint32_t)n > v.asked) /* not this read's answer: nothing to trust */
            return fail(now, "the card answered another read", NULL);
        if (v.st == V_JSON) {
            const char *why; /* parsed where the card's task left it */
            if (!bps_model_config_parse((const char *)data, (size_t)n, &v.m, &why))
                return fail(now, why, NULL);
            v.model = port_big_alloc(v.m.model_bytes);
            if (!v.model)
                return fail(now, "not enough memory for it", NULL);
            v.got = 0;
            v.st = V_MODEL;
        } else {
            if (n == 0)
                return fail(now, "model.tflite is shorter than model.json's model_bytes", NULL);
            memcpy(v.model + v.got, data, (size_t)n);
            v.got += (uint32_t)n;
            if (v.got == v.m.model_bytes)
                return finish_load(now);
        }
    }
    port_sd_info info;
    port_sd_info_get(&info);
    if (!info.present) {
        if (v.st == V_JSON && now >= v.retry_us)
            return fail(now, "no storage card", NULL);
        return;
    }
    if (!bps_card_take(BPS_CARD_MODEL))
        return; /* after the spool's pass */
    if (v.st == V_JSON)
        read_next("model.json", 0);
    else
        read_next(v.m.file, v.got);
}

void bps_validate_poll(uint64_t now)
{
    poll(now);
    /* The card is held from a model's first read to its last answer. */
    if (!v.reading && v.st != V_JSON && v.st != V_MODEL)
        bps_card_give(BPS_CARD_MODEL);
}

bool bps_validate_active(void) { return v.st == V_READY; }

void bps_validate_running(bps_validation_config *out)
{
    if (v.st == V_READY) {
        *out = v.want;
        out->threshold = v.threshold;
    } else {
        bps_validation_default(out);
    }
}

const char *bps_validate_note(void)
{
    if (v.want.type == BPS_VALIDATION_UNKNOWN)
        return "unknown validation type, or an unusable model name";
    return v.note[0] ? v.note : NULL;
}

const bps_model_config *bps_validate_model(void) { return v.st == V_READY ? &v.m : NULL; }
size_t bps_validate_arena_used(void) { return v.arena_used; }
uint32_t bps_validate_window(void) { return v.window48; }
uint32_t bps_validate_margin(void) { return v.margin; }

bool bps_validate_start(uint32_t start)
{
    if (v.st != V_READY || !worker_idle())
        return false;
    v.res.window_start = start;
    post(JOB_CHECK);
    return true;
}

bool bps_validate_take(bps_validate_result *out)
{
    if (!worker_idle() || v.taken_seq == v.req_seq || v.st != V_READY)
        return false;
    v.taken_seq = v.req_seq;
    *out = v.res;
    out->threshold = v.threshold;
    out->upload = !out->ok || out->score >= out->threshold;
    return true;
}
