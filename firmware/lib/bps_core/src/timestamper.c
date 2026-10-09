#include "timestamper.h"

void bps_timestamper_init(bps_timestamper *t, uint32_t sample_rate)
{
    t->sample_rate = sample_rate;
    t->has_anchor = false;
    t->anchor = 0;
    t->samples = 0;
    t->window_min_err = INT64_MAX;
    t->window_samples = 0;
    t->tolerance_ns = BPS_NANOS_PER_MS / 2;
    t->dropout_ns = 20 * BPS_NANOS_PER_MS;
}

int64_t bps_timestamper_push(bps_timestamper *t, int64_t estimate, size_t n, bool *dropout)
{
    if (!t->has_anchor) {
        t->has_anchor = true;
        t->anchor = estimate;
    }
    int64_t ts = t->anchor + bps_samples_to_ns(t->samples, t->sample_rate);
    if (estimate - ts < t->window_min_err)
        t->window_min_err = estimate - ts;
    t->samples += n;
    t->window_samples += n;

    *dropout = false;
    if (t->window_samples >= 2 * (uint64_t)t->sample_rate) {
        int64_t err = t->window_min_err;
        if (bps_abs64(err) > t->tolerance_ns) {
            t->anchor += err;
            *dropout = bps_abs64(err) > t->dropout_ns;
        }
        t->window_min_err = INT64_MAX;
        t->window_samples = 0;
    }
    return ts;
}
