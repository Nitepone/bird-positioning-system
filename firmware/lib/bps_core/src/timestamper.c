#include "timestamper.h"

#include <math.h>
#include <string.h>

int64_t bps_timeline_at(const bps_timeline *m, uint32_t idx, uint32_t sample_rate)
{
    int32_t d = (int32_t)(idx - m->ref_idx);
    uint64_t ad = d < 0 ? (uint64_t)(-(int64_t)d) : (uint64_t)d;
    int64_t ns = bps_samples_to_ns(ad, sample_rate);
    /* ns * ppb / 1e9, in microseconds so it cannot overflow. */
    ns += ns / 1000 * m->ppb / 1000000;
    return m->ref_ts + (d < 0 ? -ns : ns);
}

void bps_timestamper_init(bps_timestamper *t, uint32_t sample_rate)
{
    memset(t, 0, sizeof *t);
    t->sample_rate = sample_rate;
    t->win_min_err = INT64_MAX;
}

const bps_timeline *bps_timestamper_model(const bps_timestamper *t) { return &t->model; }

static void add_point(bps_timestamper *t, bps_ts_point p)
{
    if (t->n_pts == BPS_TS_POINTS) {
        t->head = (t->head + 1) % BPS_TS_POINTS;
        t->n_pts--;
    }
    t->pts[(t->head + t->n_pts) % BPS_TS_POINTS] = p;
    t->n_pts++;
}

static double median_abs(const double *v, const bool *use, uint32_t n)
{
    double s[BPS_TS_POINTS];
    uint32_t k = 0;
    for (uint32_t i = 0; i < n; i++)
        if (use[i]) {
            double a = fabs(v[i]);
            uint32_t j = k++;
            for (; j > 0 && s[j - 1] > a; j--)
                s[j] = s[j - 1];
            s[j] = a;
        }
    return k ? s[k / 2] : 0.0;
}

/* Fits the line through the points; the model then follows it. */
static void refit(bps_timestamper *t)
{
    const double period = 1e9 / t->sample_rate;
    const bps_ts_point *p0 = &t->pts[t->head];
    uint32_t n = t->n_pts;
    /* Relative to the oldest point and to the nominal rate: small numbers. */
    double x[BPS_TS_POINTS], y[BPS_TS_POINTS], r[BPS_TS_POINTS];
    bool use[BPS_TS_POINTS];
    for (uint32_t i = 0; i < n; i++) {
        const bps_ts_point *p = &t->pts[(t->head + i) % BPS_TS_POINTS];
        x[i] = (double)(int32_t)(p->idx - p0->idx);
        y[i] = (double)(p->est - p0->est) - x[i] * period;
        use[i] = true;
    }
    double a = 0, b = t->model.ppb * period / 1e9;
    for (int pass = 0; pass < 2; pass++) {
        double sx = 0, sy = 0, k = 0;
        for (uint32_t i = 0; i < n; i++)
            if (use[i]) {
                sx += x[i];
                sy += y[i];
                k++;
            }
        double mx = sx / k, my = sy / k, sxx = 0, sxy = 0;
        for (uint32_t i = 0; i < n; i++)
            if (use[i]) {
                sxx += (x[i] - mx) * (x[i] - mx);
                sxy += (x[i] - mx) * (y[i] - my);
            }
        /* The rate needs a few seconds of points; until then, keep it. */
        if (k >= 3 && x[n - 1] - x[0] >= 4.0 * t->sample_rate && sxx > 0)
            b = sxy / sxx;
        a = my - b * mx;
        for (uint32_t i = 0; i < n; i++)
            r[i] = y[i] - (a + b * x[i]);
        if (pass == 0) {
            /* A second whose best estimate was still late (busy all second)
             * would pull the line late: leave such points out. */
            double limit = fmax(3.0 * 1.4826 * median_abs(r, use, n), 2000.0);
            for (uint32_t i = 0; i < n; i++)
                use[i] = r[i] <= limit;
        }
    }
    double ss = 0, k = 0;
    for (uint32_t i = 0; i < n; i++)
        if (use[i]) {
            ss += r[i] * r[i];
            k++;
        }
    t->scatter_ns = (int64_t)llround(sqrt(ss / k));

    double ppb = b / period * 1e9;
    if (ppb > BPS_TS_MAX_PPB)
        ppb = BPS_TS_MAX_PPB;
    if (ppb < -BPS_TS_MAX_PPB)
        ppb = -BPS_TS_MAX_PPB;
    const bps_ts_point *last = &t->pts[(t->head + n - 1) % BPS_TS_POINTS];
    double xl = x[n - 1];
    t->model.ref_idx = last->idx;
    t->model.ref_ts = p0->est + (int64_t)llround(a + b * xl + xl * period);
    t->model.ppb = (int32_t)lround(ppb);
}

int bps_timestamper_push(bps_timestamper *t, uint32_t idx, int64_t est, size_t n)
{
    if (!t->has_model) {
        t->has_model = true;
        t->model.ref_idx = idx;
        t->model.ref_ts = est;
        t->model.ppb = 0;
        t->seg_start = idx;
        return BPS_TS_NEW;
    }
    int64_t err = est - bps_timeline_at(&t->model, idx, t->sample_rate);
    if (err < t->win_min_err) {
        t->win_min_err = err;
        t->win_min.idx = idx;
        t->win_min.est = est;
    }
    t->win_samples += (uint32_t)n;
    if (t->win_samples < t->sample_rate)
        return BPS_TS_SAME;

    int64_t e = t->win_min_err;
    bps_ts_point p = t->win_min;
    t->win_samples = 0;
    t->win_min_err = INT64_MAX;
    t->predict_ns = e;
    if (bps_abs64(e) > BPS_TS_JUMP_NS) {
        /* Start again from this second's best estimate, keeping the rate. */
        t->head = t->n_pts = 0;
        add_point(t, p);
        t->model.ref_idx = p.idx;
        t->model.ref_ts = p.est;
        t->seg_start = p.idx;
        return BPS_TS_NEW;
    }
    add_point(t, p);
    refit(t);
    return BPS_TS_UPDATED;
}
