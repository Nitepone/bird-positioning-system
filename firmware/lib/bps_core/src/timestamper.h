/*
 * Converts per-block capture-time estimates into a smooth sample timeline
 * (C port of `Timestamper` in bsp-client's capture.rs).
 *
 * Each block yields a noisy estimate of when its first sample was captured;
 * scheduling delay only ever makes it late. The timeline is
 * `anchor + n / sample_rate`; every 2 s the smallest error seen against it is
 * applied to the anchor if it exceeds the tolerance. This follows the sample
 * clock and recovers from dropouts while ignoring jitter.
 */
#ifndef BPS_TIMESTAMPER_H
#define BPS_TIMESTAMPER_H

#include "bps_common.h"

typedef struct {
    uint32_t sample_rate;
    bool has_anchor;
    int64_t anchor;
    uint64_t samples;
    int64_t window_min_err;
    uint64_t window_samples;
    int64_t tolerance_ns;
    int64_t dropout_ns;
} bps_timestamper;

void bps_timestamper_init(bps_timestamper *t, uint32_t sample_rate);
/* Timestamp of the first of `n` new samples whose estimated capture time is
 * `estimate`; sets `*dropout` when a large timeline correction was made. */
int64_t bps_timestamper_push(bps_timestamper *t, int64_t estimate, size_t n, bool *dropout);

#endif
