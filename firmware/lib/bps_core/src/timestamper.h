/*
 * Turns per-block capture-time estimates into a sample timeline: when each
 * sample was captured, on the monotonic clock.
 *
 * Each block yields an estimate of when its first sample was captured;
 * scheduling or interrupt delay only ever makes it late. Every second, the
 * earliest estimate of that second (the one least delayed) becomes a point;
 * a line through the last BPS_TS_POINTS points gives the timeline's offset
 * and the sample clock's rate against the monotonic clock. The line is
 * refitted every second, and its scatter tells how precise it is.
 *
 * A second whose earliest estimate is off the line by more than
 * BPS_TS_JUMP_NS means samples were lost (or the line was wrong): the
 * timeline starts afresh from there.
 *
 * (bsp-client's capture.rs anchors once and corrects in steps past 0.5 ms;
 * this does better where estimates are only microseconds late.)
 */
#ifndef BPS_TIMESTAMPER_H
#define BPS_TIMESTAMPER_H

#include "bps_common.h"

#define BPS_TS_POINTS 32
#define BPS_TS_JUMP_NS (BPS_NANOS_PER_MS / 2)
/* Largest sample clock rate error believed, parts per billion. */
#define BPS_TS_MAX_PPB 1000000

/* ts(idx) = ref_ts + (idx - ref_idx) / sample_rate * (1 + ppb / 1e9) */
typedef struct {
    uint32_t ref_idx;
    int64_t ref_ts;
    int32_t ppb; /* sample period against nominal: + means a slow sample clock */
} bps_timeline;

/* Capture time of sample `idx` (any distance either side of ref_idx up to 2^31). */
int64_t bps_timeline_at(const bps_timeline *m, uint32_t idx, uint32_t sample_rate);

typedef struct {
    uint32_t idx;
    int64_t est;
} bps_ts_point;

typedef struct {
    uint32_t sample_rate;
    bool has_model;
    bps_timeline model;
    uint32_t seg_start; /* where the latest timeline starts */
    /* The current second: its earliest estimate against the model. */
    uint32_t win_samples;
    int64_t win_min_err;
    bps_ts_point win_min;
    /* Recent seconds' earliest estimates, oldest first from `head`. */
    bps_ts_point pts[BPS_TS_POINTS];
    uint32_t head, n_pts;
    /* Diagnostics: RMS distance of the points from the line, and of the
     * newest point from the line before it was added (prediction error). */
    int64_t scatter_ns;
    int64_t predict_ns;
} bps_timestamper;

enum { BPS_TS_SAME, BPS_TS_UPDATED, BPS_TS_NEW };

void bps_timestamper_init(bps_timestamper *t, uint32_t sample_rate);
/* Feeds the estimated capture time `est` of sample `idx`, the first of `n`.
 * Returns BPS_TS_NEW when a new timeline starts (the first block, or after a
 * jump; it starts at `seg_start`, which after a jump is earlier than `idx`),
 * BPS_TS_UPDATED when the model was refined, else BPS_TS_SAME. The model is
 * then bps_timestamper_model(). */
int bps_timestamper_push(bps_timestamper *t, uint32_t idx, int64_t est, size_t n);
const bps_timeline *bps_timestamper_model(const bps_timestamper *t);

#endif
