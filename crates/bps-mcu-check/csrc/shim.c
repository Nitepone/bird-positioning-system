/* Plain-argument wrappers around the firmware core, for the Rust tests. */
#include <stdlib.h>
#include <string.h>

#include "gate.h"
#include "timesync.h"

void *chk_est_new(int64_t window_ns, uint32_t bins)
{
    bps_estimator *e = malloc(sizeof *e);
    if (e)
        bps_est_init(e, window_ns, bins);
    return e;
}

void chk_est_free(void *e) { free(e); }

/* Pushes a measurement; returns 1 and fills `out` with the fit (at, offset
 * bits, rate bits, rtt, error, points), or 0 if there is none. */
int chk_est_push(void *e, int64_t at, int64_t offset_ns, int64_t rtt_ns, int64_t *fit_at,
                 double *offset, double *rate, int64_t *fit_rtt, int64_t *error, uint32_t *points)
{
    bps_ts_sample s = {at, offset_ns, rtt_ns};
    const bps_ts_fit *f = bps_est_push(e, s);
    if (!f)
        return 0;
    *fit_at = f->at;
    *offset = f->offset_ns;
    *rate = f->rate;
    *fit_rtt = f->rtt_ns;
    *error = f->error_ns;
    *points = f->points;
    return 1;
}

void chk_request_encode(uint32_t seq, const uint8_t id[16], int64_t t1, uint8_t out[36])
{
    bps_ts_request r = {.seq = seq, .t1 = t1};
    memcpy(r.client_id, id, 16);
    bps_ts_request_encode(&r, out);
}

int chk_response_decode(const uint8_t *b, size_t len, uint32_t *seq, int64_t t[3])
{
    bps_ts_response r;
    if (!bps_ts_response_decode(b, len, &r))
        return 0;
    *seq = r.seq;
    t[0] = r.t1;
    t[1] = r.t2;
    t[2] = r.t3;
    return 1;
}

int chk_sample_of(const int64_t t[3], int64_t t4, int64_t out[3])
{
    bps_ts_response r = {0, t[0], t[1], t[2]};
    bps_ts_sample s;
    if (!bps_ts_sample_of(&r, t4, &s))
        return 0;
    out[0] = s.at;
    out[1] = s.offset_ns;
    out[2] = s.rtt_ns;
    return 1;
}

/* Band-passes `n` samples in place with the gate's filters. */
void chk_bandpass(uint32_t sample_rate, float low_hz, float high_hz, float *pcm, size_t n)
{
    bps_biquad hp, lp;
    bps_biquad_highpass(&hp, sample_rate, low_hz);
    bps_biquad_lowpass(&lp, sample_rate, high_hz);
    for (size_t i = 0; i < n; i++)
        pcm[i] = bps_biquad_process(&lp, bps_biquad_process(&hp, pcm[i]));
}
