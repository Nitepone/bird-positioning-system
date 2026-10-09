/*
 * Capture side (runs on its own core / task): timestamps I2S blocks, keeps
 * them in the ring, and runs the noise gate continuously. The network side
 * reads the published fields below; each is a 32-bit value written only here.
 */
#ifndef BPS_CAPTURE_H
#define BPS_CAPTURE_H

#include "bps_common.h"
#include "gate.h"
#include "ring.h"
#include "timestamper.h"

/* Largest block processed at once (stereo frames); bigger reads are split.
 * Both boards read one 256-frame DMA buffer at a time. */
#define BPS_CAPTURE_MAX_FRAMES 256

typedef struct {
    bps_ring *ring;
    uint32_t sample_rate;
    int channel; /* 0 = left, 1 = right (the mic's SEL pin) */
    bps_gate gate;
    bps_timestamper stamper;

    /* Published to the network side. */
    uint32_t pass_count;    /* gate frames that passed */
    uint32_t last_pass_end; /* ring index just past the newest passing frame */
    uint32_t dropouts;      /* capture timeline jumps */
    int32_t rate_err_ppb;   /* sample rate vs. the monotonic clock, parts per billion */
    int32_t level_cdb[2];   /* first seconds' level per channel, centi-dBFS */
    uint32_t levels_ready;

    /* Capture-side bookkeeping. */
    bool started;
    int64_t first_est;
    uint64_t frames;
    double level_sum[2];
    uint32_t level_frames;
    uint32_t block_start; /* ring index of the block being gated */
    int16_t pcm[BPS_CAPTURE_MAX_FRAMES]; /* not on core 1's 2 KB stack */
} bps_capture;

void bps_capture_init(bps_capture *c, bps_ring *ring, uint32_t sample_rate, int channel,
                      const bps_gate_config *gate);

/* Called by the board layer with interleaved left/right 32-bit frames (24-bit
 * samples left-aligned) just read from I2S; `read_us` is the monotonic time
 * the read returned, when the last frame had been captured at the latest. */
void bps_capture_frames(const int32_t *lr, size_t frames, uint64_t read_us);

#endif
