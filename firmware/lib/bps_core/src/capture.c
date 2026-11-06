#include "capture.h"

#include <math.h>
#include <string.h>

#define STORE(p, v) __atomic_store_n(p, v, __ATOMIC_RELEASE)
#define LOAD(p) __atomic_load_n(p, __ATOMIC_ACQUIRE)

static bps_capture *active;

void bps_capture_init(bps_capture *c, bps_ring *ring, uint32_t sample_rate, int channel,
                      const bps_gate_config *gate)
{
    memset(c, 0, sizeof *c);
    c->ring = ring;
    c->sample_rate = sample_rate;
    c->channel = channel ? 1 : 0;
    bps_gate_init(&c->gate, gate, sample_rate);
    bps_timestamper_init(&c->stamper, sample_rate);
    STORE(&active, c);
}

bool bps_capture_set_gate(bps_capture *c, const bps_gate_config *gate, bool same_band)
{
    if (LOAD(&c->gate_ack) != c->gate_req)
        return false;
    uint32_t frame_len = c->gate.frame_len; /* only ever changed here */
    bps_gate_init(&c->next_gate, gate, c->sample_rate);
    c->next_gate.frame_len = frame_len;
    c->next_keep_floor = same_band;
    STORE(&c->gate_req, c->gate_req + 1);
    return true;
}

/* Capture side: takes a gate the network side has set up. */
static void take_gate(bps_capture *c)
{
    uint32_t req = LOAD(&c->gate_req);
    if (req == c->gate_ack)
        return;
    bps_gate *g = &c->gate, *n = &c->next_gate;
    if (c->next_keep_floor) {
        /* Only the threshold changed: the floor and the frame in progress
         * still hold. */
        n->in_frame = g->in_frame;
        n->sum = g->sum;
        n->hp = g->hp;
        n->lp = g->lp;
        n->has_floor = g->has_floor;
        n->floor_db = g->floor_db;
        n->n_warm = g->n_warm;
        memcpy(n->warm, g->warm, sizeof n->warm);
    }
    *g = *n;
    STORE(&c->gate_ack, req);
}

static void on_frame(void *ctx, size_t end, const bps_gate_frame *f)
{
    bps_capture *c = ctx;
    if (!f->pass)
        return;
    STORE(&c->last_pass_end, c->block_start + (uint32_t)end);
    STORE(&c->pass_count, c->pass_count + 1);
}

/* Level of `n` 16-bit samples whose squares add up to `sum`. */
static int32_t to_cdb(uint64_t sum, uint32_t n)
{
    double rms = sqrt((double)sum / (n ? n : 1)) / 32768.0;
    return (int32_t)lround(2000.0 * log10(rms > 1e-9 ? rms : 1e-9));
}

static void process(bps_capture *c, const int32_t *lr, size_t frames, uint64_t read_us)
{
    int16_t *pcm = c->pcm;
    for (size_t i = 0; i < frames; i++)
        pcm[i] = (int16_t)(lr[2 * i + c->channel] >> 16);

    /* The read returned after its last frame was captured, so this estimate
     * of the first frame's capture time can only be late. */
    int64_t est = (int64_t)read_us * 1000 - bps_samples_to_ns(frames, c->sample_rate);
    uint32_t idx = bps_ring_written(c->ring);
    int r = bps_timestamper_push(&c->stamper, idx, est, frames);
    if (r != BPS_TS_SAME) {
        const bps_timestamper *t = &c->stamper;
        bps_ring_timeline(c->ring, bps_timestamper_model(t), r == BPS_TS_NEW, t->seg_start);
        if (r == BPS_TS_NEW && c->started)
            STORE(&c->dropouts, c->dropouts + 1);
        STORE(&c->rate_err_ppb, -t->model.ppb);
        STORE(&c->scatter_ns, (int32_t)t->scatter_ns);
        int64_t p = t->predict_ns;
        STORE(&c->predict_ns, (int32_t)(p > INT32_MAX ? INT32_MAX : p < -INT32_MAX ? -INT32_MAX : p));
    }

    c->block_start = idx;
    bps_ring_write(c->ring, pcm, frames);
    take_gate(c);
    bps_gate_process(&c->gate, pcm, frames, on_frame, c);

    /* Diagnostics: both channels' level over the first 2 s (which one is the mic?). */
    c->started = true;
    if (c->level_frames < 2 * c->sample_rate) {
        /* In integers (the RP2040 has no FPU), from the top 16 bits as uploaded. */
        for (size_t i = 0; i < frames; i++)
            for (int ch = 0; ch < 2; ch++) {
                int32_t x = lr[2 * i + ch] >> 16;
                c->level_sum[ch] += (uint32_t)(x * x);
            }
        c->level_frames += (uint32_t)frames;
        if (c->level_frames >= 2 * c->sample_rate) {
            STORE(&c->level_cdb[0], to_cdb(c->level_sum[0], c->level_frames));
            STORE(&c->level_cdb[1], to_cdb(c->level_sum[1], c->level_frames));
            STORE(&c->levels_ready, 1u);
        }
    }
}

void bps_capture_frames(const int32_t *lr, size_t frames, uint64_t read_us)
{
    bps_capture *c = __atomic_load_n(&active, __ATOMIC_ACQUIRE);
    if (!c)
        return;
    /* Split large reads; each piece is stamped by when its last frame was due. */
    while (frames > 0) {
        size_t k = frames < BPS_CAPTURE_MAX_FRAMES ? frames : BPS_CAPTURE_MAX_FRAMES;
        uint64_t rest_us = (uint64_t)bps_samples_to_ns(frames - k, c->sample_rate) / 1000;
        process(c, lr, k, read_us - rest_us);
        lr += 2 * k;
        frames -= k;
    }
}
