#include "capture.h"

#include <math.h>
#include <string.h>

#define STORE(p, v) __atomic_store_n(p, v, __ATOMIC_RELEASE)

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

static void on_frame(void *ctx, size_t end, const bps_gate_frame *f)
{
    bps_capture *c = ctx;
    if (!f->pass)
        return;
    STORE(&c->last_pass_end, c->block_start + (uint32_t)end);
    STORE(&c->pass_count, c->pass_count + 1);
}

static int32_t to_cdb(double sum, uint32_t n)
{
    double rms = sqrt(sum / (n ? n : 1));
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
    bool dropout;
    int64_t ts = bps_timestamper_push(&c->stamper, est, frames, &dropout);
    if (dropout)
        STORE(&c->dropouts, c->dropouts + 1);

    c->block_start = bps_ring_written(c->ring);
    bps_ring_write(c->ring, pcm, frames, ts);
    bps_gate_process(&c->gate, pcm, frames, on_frame, c);

    /* Diagnostics: both channels' level over the first 2 s (which one is the
     * mic?), and the sample rate measured against the monotonic clock. */
    if (!c->started) {
        c->started = true;
        c->first_est = est;
    }
    c->frames += frames;
    if (c->level_frames < 2 * c->sample_rate) {
        for (size_t i = 0; i < frames; i++)
            for (int ch = 0; ch < 2; ch++) {
                double x = lr[2 * i + ch] / 2147483648.0;
                c->level_sum[ch] += x * x;
            }
        c->level_frames += (uint32_t)frames;
        if (c->level_frames >= 2 * c->sample_rate) {
            STORE(&c->level_cdb[0], to_cdb(c->level_sum[0], c->level_frames));
            STORE(&c->level_cdb[1], to_cdb(c->level_sum[1], c->level_frames));
            STORE(&c->levels_ready, 1u);
        }
    }
    int64_t elapsed = est + bps_samples_to_ns(frames, c->sample_rate) - c->first_est;
    if (elapsed > 10 * BPS_NANOS_PER_SEC) {
        double nominal = (double)bps_samples_to_ns(c->frames, c->sample_rate);
        double ppb = (nominal - (double)elapsed) / (double)elapsed * 1e9;
        /* Way off means broken capture, not a slow clock: keep it readable. */
        ppb = ppb > 2e9 ? 2e9 : ppb < -2e9 ? -2e9 : ppb;
        STORE(&c->rate_err_ppb, (int32_t)lround(ppb));
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
