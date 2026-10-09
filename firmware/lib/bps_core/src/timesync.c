#include "timesync.h"

#include <math.h>
#include <string.h>

static const uint8_t MAGIC[3] = {'B', 'S', 'P'};
#define VERSION 1
#define KIND_REQUEST 1
#define KIND_RESPONSE 2

static void put_u32(uint8_t *b, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        b[i] = (uint8_t)(v >> (24 - 8 * i));
}

static void put_i64(uint8_t *b, int64_t v)
{
    uint64_t u = (uint64_t)v;
    for (int i = 0; i < 8; i++)
        b[i] = (uint8_t)(u >> (56 - 8 * i));
}

static uint32_t get_u32(const uint8_t *b)
{
    return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
}

static int64_t get_i64(const uint8_t *b)
{
    uint64_t u = 0;
    for (int i = 0; i < 8; i++)
        u = u << 8 | b[i];
    return (int64_t)u;
}

static void header(uint8_t *b, uint8_t kind, uint32_t seq)
{
    memcpy(b, MAGIC, 3);
    b[3] = VERSION;
    b[4] = kind;
    b[5] = b[6] = b[7] = 0;
    put_u32(b + 8, seq);
}

static bool check_header(const uint8_t *b, size_t len, uint8_t kind, size_t want, uint32_t *seq)
{
    if (len != want || memcmp(b, MAGIC, 3) != 0 || b[3] != VERSION || b[4] != kind)
        return false;
    *seq = get_u32(b + 8);
    return true;
}

void bps_ts_request_encode(const bps_ts_request *r, uint8_t out[BPS_TS_REQUEST_LEN])
{
    header(out, KIND_REQUEST, r->seq);
    memcpy(out + 12, r->client_id, 16);
    put_i64(out + 28, r->t1);
}

bool bps_ts_request_decode(const uint8_t *b, size_t len, bps_ts_request *out)
{
    if (!check_header(b, len, KIND_REQUEST, BPS_TS_REQUEST_LEN, &out->seq))
        return false;
    memcpy(out->client_id, b + 12, 16);
    out->t1 = get_i64(b + 28);
    return true;
}

void bps_ts_response_encode(const bps_ts_response *r, uint8_t out[BPS_TS_RESPONSE_LEN])
{
    header(out, KIND_RESPONSE, r->seq);
    put_i64(out + 12, r->t1);
    put_i64(out + 20, r->t2);
    put_i64(out + 28, r->t3);
}

bool bps_ts_response_decode(const uint8_t *b, size_t len, bps_ts_response *out)
{
    if (!check_header(b, len, KIND_RESPONSE, BPS_TS_RESPONSE_LEN, &out->seq))
        return false;
    out->t1 = get_i64(b + 12);
    out->t2 = get_i64(b + 20);
    out->t3 = get_i64(b + 28);
    return true;
}

void bps_ts_offset_rtt(const bps_ts_response *r, int64_t t4, int64_t *offset, int64_t *rtt)
{
    *offset = ((r->t2 - r->t1) + (r->t3 - t4)) / 2;
    *rtt = (t4 - r->t1) - (r->t3 - r->t2);
}

bool bps_ts_sample_of(const bps_ts_response *r, int64_t t4, bps_ts_sample *out)
{
    int64_t offset, rtt;
    bps_ts_offset_rtt(r, t4, &offset, &rtt);
    int64_t held = r->t3 - r->t2;
    if (rtt < 0 || held < 0 || held >= BPS_NANOS_PER_SEC)
        return false;
    out->at = r->t1 + (t4 - r->t1) / 2;
    out->offset_ns = offset;
    out->rtt_ns = rtt;
    return true;
}

int64_t bps_fit_offset_at(const bps_ts_fit *f, int64_t t)
{
    return (int64_t)round(f->offset_ns + f->rate * (double)(t - f->at));
}

double bps_fit_drift_ppm(const bps_ts_fit *f) { return f->rate * 1e6; }

/* ---- Estimator ---- */

/* Measurements in a row that must disagree with the fit to count as a step. */
#define STEP_RUN 3
/* Disagreement beyond the measurement's own uncertainty that counts. */
#define STEP_MARGIN_NS 100000
/* Fits spanning less of the window than this assume no drift. */
#define MIN_DRIFT_SPAN 0.25

void bps_est_init(bps_estimator *e, int64_t window_ns, uint32_t bins)
{
    memset(e, 0, sizeof *e);
    e->window_ns = window_ns;
    e->bins = bins < 1 ? 1 : bins > BPS_EST_MAX_BINS ? BPS_EST_MAX_BINS : bins;
}

static bps_ts_sample *sample_at(bps_estimator *e, uint32_t i)
{
    return &e->samples[(e->head + i) % BPS_EST_MAX_SAMPLES];
}

static void pop_front(bps_estimator *e)
{
    e->head = (e->head + 1) % BPS_EST_MAX_SAMPLES;
    e->len--;
}

const bps_ts_fit *bps_est_fit(const bps_estimator *e) { return e->has_fit ? &e->fit : NULL; }

/* Weight 1/rtt^2: a measurement's error bound grows with its round trip.
 * The 10 us floor keeps near-zero round trips from dominating. */
static double weight(const bps_ts_sample *s)
{
    double r = (double)s->rtt_ns + 10000.0;
    return 1.0 / (r * r);
}

static void refit(bps_estimator *e)
{
    if (e->len == 0) {
        e->has_fit = false;
        return;
    }
    int64_t newest = sample_at(e, e->len - 1)->at;
    int64_t bin_ns = e->window_ns / (int64_t)e->bins;
    if (bin_ns < 1)
        bin_ns = 1;
    /* Samples span at most the window, so at most window / bin_ns + 1 bins. */
    bps_ts_sample *best = e->best;
    const uint32_t cap = sizeof e->best / sizeof e->best[0];
    uint32_t nb = 0;
    for (uint32_t i = 0; i < e->len; i++) {
        bps_ts_sample s = *sample_at(e, i);
        int64_t bin = (newest - s.at) / bin_ns;
        if (nb > 0 && (newest - best[nb - 1].at) / bin_ns == bin) {
            if (s.rtt_ns < best[nb - 1].rtt_ns)
                best[nb - 1] = s;
        } else if (nb < cap) {
            best[nb++] = s;
        }
    }
    int64_t rtt_ns = best[0].rtt_ns;
    for (uint32_t i = 1; i < nb; i++)
        if (best[i].rtt_ns < rtt_ns)
            rtt_ns = best[i].rtt_ns;
    double span = (double)(newest - best[0].at);
#define X(i) ((double)(best[i].at - newest))
#define Y(i) ((double)best[i].offset_ns)
    double sw = 0, swx = 0, swy = 0;
    for (uint32_t i = 0; i < nb; i++)
        sw += weight(&best[i]);
    for (uint32_t i = 0; i < nb; i++)
        swx += weight(&best[i]) * X(i);
    for (uint32_t i = 0; i < nb; i++)
        swy += weight(&best[i]) * Y(i);
    double mx = swx / sw, my = swy / sw;
    double rate = 0.0;
    if (nb >= 3 && span >= MIN_DRIFT_SPAN * (double)e->window_ns) {
        double sxx = 0, sxy = 0;
        for (uint32_t i = 0; i < nb; i++)
            sxx += weight(&best[i]) * ((X(i) - mx) * (X(i) - mx));
        for (uint32_t i = 0; i < nb; i++)
            sxy += weight(&best[i]) * (X(i) - mx) * (Y(i) - my);
        rate = sxy / sxx;
    }
    double offset_ns = my - rate * mx;
    double ss = 0;
    for (uint32_t i = 0; i < nb; i++) {
        double d = Y(i) - offset_ns - rate * X(i);
        ss += weight(&best[i]) * (d * d);
    }
#undef X
#undef Y
    double scatter = sqrt(ss / sw);
    e->has_fit = true;
    e->fit.at = newest;
    e->fit.offset_ns = offset_ns;
    e->fit.rate = rate;
    e->fit.rtt_ns = rtt_ns;
    e->fit.error_ns = rtt_ns / 2 + (int64_t)round(scatter);
    e->fit.points = nb;
}

const bps_ts_fit *bps_est_push(bps_estimator *e, bps_ts_sample s)
{
    if (e->has_fit) {
        int64_t miss = bps_abs64(s.offset_ns - bps_fit_offset_at(&e->fit, s.at));
        if (miss > s.rtt_ns / 2 + e->fit.error_ns + STEP_MARGIN_NS) {
            if (!e->disagreeing) {
                e->disagreeing = true;
                e->disagreeing_since = s.at;
            }
            e->disagreements++;
            if (e->disagreements >= STEP_RUN) {
                /* Samples are in time order: drop what came before the step. */
                while (e->len > 0 && sample_at(e, 0)->at < e->disagreeing_since)
                    pop_front(e);
                e->disagreeing = false;
                e->disagreements = 0;
            }
        } else {
            e->disagreeing = false;
            e->disagreements = 0;
        }
    }
    /* Measurements arrive in order; drop anything newer (the clock stepped back). */
    while (e->len > 0 && sample_at(e, e->len - 1)->at > s.at)
        e->len--;
    if (e->len == BPS_EST_MAX_SAMPLES)
        pop_front(e);
    *sample_at(e, e->len) = s;
    e->len++;
    while (e->len > 0 && s.at - sample_at(e, 0)->at > e->window_ns)
        pop_front(e);
    /* While a run of disagreements may turn out to be a step, keep the old fit. */
    if (e->disagreements == 0 || !e->has_fit)
        refit(e);
    return bps_est_fit(e);
}

/* ---- Clock ---- */

void bps_clock_init(bps_clock *c, bool corrects)
{
    memset(c, 0, sizeof *c);
    c->corrects = corrects;
    bps_est_init(&c->est, BPS_CLOCK_WINDOW_NS, BPS_CLOCK_BINS);
}

int64_t bps_clock_correct(bps_clock *c, int64_t start, int64_t len_ns)
{
    if (!c->corrects)
        return start;
    int64_t mid = start + len_ns / 2;
    const bps_ts_fit *f = bps_est_fit(&c->est);
    int64_t corr = f ? bps_fit_offset_at(f, mid) : 0;
    c->has_applied = true;
    c->applied_mid = mid;
    c->applied_corr = corr;
    c->half_chunk_ns = len_ns / 2;
    return start + corr;
}

void bps_clock_count_request(bps_clock *c) { c->requests++; }

void bps_clock_add_sample(bps_clock *c, bps_ts_sample s)
{
    if (c->n_rtts < BPS_CLOCK_MAX_RTTS)
        c->rtts[c->n_rtts++] = s.rtt_ns;
    c->has_last = true;
    c->last_at = s.at;
    bps_est_push(&c->est, s);
}

bool bps_clock_status_take(bps_clock *c, int64_t now, bps_clock_status *out)
{
    int64_t rtts[BPS_CLOCK_MAX_RTTS];
    uint32_t n = c->n_rtts, requests = c->requests;
    memcpy(rtts, c->rtts, n * sizeof rtts[0]);
    c->n_rtts = 0;
    c->requests = 0;
    const bps_ts_fit *f = bps_est_fit(&c->est);
    if (!f || !c->has_last)
        return false;
    /* Offset left in the timestamps: for corrected chunks, how far the
     * current estimate has moved from the correction applied. */
    int64_t offset_ns = 0, drift_ns = 0;
    if (c->corrects && c->has_applied) {
        offset_ns = bps_fit_offset_at(f, c->applied_mid) - c->applied_corr;
        drift_ns = (int64_t)round(fabs(f->rate * (double)c->half_chunk_ns));
    } else if (!c->corrects) {
        offset_ns = bps_fit_offset_at(f, now);
    }
    for (uint32_t i = 1; i < n; i++) /* insertion sort; n is small */
        for (uint32_t j = i; j > 0 && rtts[j - 1] > rtts[j]; j--) {
            int64_t t = rtts[j];
            rtts[j] = rtts[j - 1];
            rtts[j - 1] = t;
        }
    out->offset_ns = offset_ns;
    out->rtt_ns = f->rtt_ns;
    out->measured_at = c->last_at + bps_fit_offset_at(f, c->last_at);
    out->error_ns = bps_abs64(offset_ns) + f->error_ns + drift_ns;
    out->corrects_timestamps = c->corrects;
    out->clock_offset_ns = bps_fit_offset_at(f, now);
    out->drift_ppm = bps_fit_drift_ppm(f);
    out->fit_points = f->points;
    out->requests = requests;
    out->replies = n;
    out->has_rtts = n > 0;
    out->rtt_min_ns = n ? rtts[0] : 0;
    out->rtt_median_ns = n ? rtts[n / 2] : 0;
    out->rtt_max_ns = n ? rtts[n - 1] : 0;
    return true;
}
