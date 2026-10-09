/*
 * Noise gate: is there band-limited energy clearly above the adaptive noise
 * floor? A continuous, frame-by-frame port of bsp-client's gate.rs (which
 * judges whole chunks); the floor follows the same rules.
 */
#ifndef BPS_GATE_H
#define BPS_GATE_H

#include "bps_common.h"

/* As `bsp_proto::GateConfig`, handed out at registration. */
typedef struct {
    float band_low_hz;
    float band_high_hz;
    float threshold_db;
    uint32_t frame_ms;
} bps_gate_config;

void bps_gate_config_default(bps_gate_config *cfg);

/* Transposed direct form II biquad (as `bsp_core::audio::Biquad`). */
typedef struct {
    float b0, b1, b2, a1, a2, z1, z2;
} bps_biquad;

void bps_biquad_highpass(bps_biquad *f, uint32_t sample_rate, float freq);
void bps_biquad_lowpass(bps_biquad *f, uint32_t sample_rate, float freq);

static inline float bps_biquad_process(bps_biquad *f, float x)
{
    float y = f->b0 * x + f->z1;
    f->z1 = f->b1 * x - f->a1 * y + f->z2;
    f->z2 = f->b2 * x - f->a2 * y;
    return y;
}

/* Frames measured before the floor is first set (from their 20th percentile). */
#define BPS_GATE_WARMUP_FRAMES 30

typedef struct {
    bool pass;
    float snr_db;
    float floor_db;
} bps_gate_frame;

typedef struct {
    float threshold_db;
    bps_biquad hp, lp;
    uint32_t frame_len, in_frame;
    float sum;
    bool has_floor;
    float floor_db;
    float warm[BPS_GATE_WARMUP_FRAMES];
    uint32_t n_warm;
} bps_gate;

void bps_gate_init(bps_gate *g, const bps_gate_config *cfg, uint32_t sample_rate);
/* Feeds samples; calls `on_frame` with the sample offset (within `pcm`) just
 * past each completed frame. Warm-up frames are never reported as passing. */
void bps_gate_process(bps_gate *g, const int16_t *pcm, size_t n,
                      void (*on_frame)(void *ctx, size_t end, const bps_gate_frame *f), void *ctx);

#endif
