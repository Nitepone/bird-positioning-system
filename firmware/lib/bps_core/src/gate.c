#include "gate.h"

#include <math.h>

#define PI_F 3.14159265358979323846f
#define FRAC_1_SQRT_2_F 0.70710678118654752440f

/* Per-frame smoothing factors for the floor estimate. */
#define FALL_RATE 0.3f
#define QUIET_RISE_RATE 0.05f
/* Lets the floor follow sustained loud backgrounds (rain, wind) slowly. */
#define LOUD_RISE_RATE 0.002f

void bps_gate_config_default(bps_gate_config *cfg)
{
    cfg->band_low_hz = 1000.0f;
    cfg->band_high_hz = 10000.0f;
    cfg->threshold_db = 10.0f;
    cfg->frame_ms = 100;
}

static void biquad_set(bps_biquad *f, const float b[3], const float a[3])
{
    f->b0 = b[0] / a[0];
    f->b1 = b[1] / a[0];
    f->b2 = b[2] / a[0];
    f->a1 = a[1] / a[0];
    f->a2 = a[2] / a[0];
    f->z1 = f->z2 = 0.0f;
}

static void params(uint32_t sample_rate, float freq, float *cos_w0, float *alpha)
{
    float max = (float)sample_rate * 0.45f;
    if (freq > max)
        freq = max;
    float w0 = 2.0f * PI_F * freq / (float)sample_rate;
    *cos_w0 = cosf(w0);
    *alpha = sinf(w0) / (2.0f * FRAC_1_SQRT_2_F);
}

void bps_biquad_highpass(bps_biquad *f, uint32_t sample_rate, float freq)
{
    float c, al;
    params(sample_rate, freq, &c, &al);
    const float b[3] = {(1.0f + c) / 2.0f, -(1.0f + c), (1.0f + c) / 2.0f};
    const float a[3] = {1.0f + al, -2.0f * c, 1.0f - al};
    biquad_set(f, b, a);
}

void bps_biquad_lowpass(bps_biquad *f, uint32_t sample_rate, float freq)
{
    float c, al;
    params(sample_rate, freq, &c, &al);
    const float b[3] = {(1.0f - c) / 2.0f, 1.0f - c, (1.0f - c) / 2.0f};
    const float a[3] = {1.0f + al, -2.0f * c, 1.0f - al};
    biquad_set(f, b, a);
}

void bps_gate_init(bps_gate *g, const bps_gate_config *cfg, uint32_t sample_rate)
{
    g->threshold_db = cfg->threshold_db;
    bps_biquad_highpass(&g->hp, sample_rate, cfg->band_low_hz);
    bps_biquad_lowpass(&g->lp, sample_rate, cfg->band_high_hz);
    g->frame_len = sample_rate * cfg->frame_ms / 1000;
    if (g->frame_len == 0)
        g->frame_len = 1;
    g->in_frame = 0;
    g->sum = 0.0f;
    g->has_floor = false;
    g->floor_db = 0.0f;
    g->n_warm = 0;
}

/* Moves the floor towards a frame's level; returns the frame's SNR. */
static float update_floor(bps_gate *g, float db)
{
    float snr = db - g->floor_db;
    float rate = db < g->floor_db         ? FALL_RATE
                 : snr < g->threshold_db ? QUIET_RISE_RATE
                                         : LOUD_RISE_RATE;
    g->floor_db += (db - g->floor_db) * rate;
    return snr;
}

static void end_warmup(bps_gate *g)
{
    float v[BPS_GATE_WARMUP_FRAMES];
    uint32_t n = g->n_warm;
    for (uint32_t i = 0; i < n; i++)
        v[i] = g->warm[i];
    for (uint32_t i = 1; i < n; i++)
        for (uint32_t j = i; j > 0 && v[j - 1] > v[j]; j--) {
            float t = v[j];
            v[j] = v[j - 1];
            v[j - 1] = t;
        }
    g->floor_db = v[(uint32_t)lroundf((float)(n - 1) * 0.2f)];
    g->has_floor = true;
    for (uint32_t i = 0; i < n; i++)
        update_floor(g, g->warm[i]);
}

void bps_gate_process(bps_gate *g, const int16_t *pcm, size_t n,
                      void (*on_frame)(void *ctx, size_t end, const bps_gate_frame *f), void *ctx)
{
    for (size_t i = 0; i < n; i++) {
        float x = (float)pcm[i] * (1.0f / 32768.0f);
        float y = bps_biquad_process(&g->lp, bps_biquad_process(&g->hp, x));
        g->sum += y * y;
        if (++g->in_frame < g->frame_len)
            continue;
        float rms = sqrtf(g->sum / (float)g->frame_len);
        float db = 20.0f * log10f(rms > 1e-9f ? rms : 1e-9f);
        g->in_frame = 0;
        g->sum = 0.0f;
        bps_gate_frame f = {false, 0.0f, g->floor_db};
        if (!g->has_floor) {
            g->warm[g->n_warm++] = db;
            if (g->n_warm == BPS_GATE_WARMUP_FRAMES)
                end_warmup(g);
            f.floor_db = g->floor_db;
        } else {
            f.snr_db = update_floor(g, db);
            f.pass = f.snr_db >= g->threshold_db;
            f.floor_db = g->floor_db;
        }
        if (on_frame)
            on_frame(ctx, i + 1, &f);
    }
}
