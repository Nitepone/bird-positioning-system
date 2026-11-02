#include "anchor.h"

#include <math.h>
#include <string.h>

void bps_anchors_init(bps_anchors *t) { memset(t, 0, sizeof *t); }

static const bps_anchor *at(const bps_anchors *t, uint32_t i)
{
    return &t->a[(t->head + i) % BPS_ANCHORS];
}

void bps_anchors_push(bps_anchors *t, const bps_anchor *a)
{
    const bps_anchor *last = bps_anchors_last(t);
    if (last && a->local_ns <= last->local_ns)
        return;
    /* The latest of a contact replaces the one before it, but never its first. */
    if (t->n >= 2 && last->session == a->session && at(t, t->n - 2)->session == a->session) {
        t->a[(t->head + t->n - 1) % BPS_ANCHORS] = *a;
        return;
    }
    if (t->n == BPS_ANCHORS) {
        t->head = (t->head + 1) % BPS_ANCHORS;
        t->n--;
    }
    t->a[(t->head + t->n) % BPS_ANCHORS] = *a;
    t->n++;
}

const bps_anchor *bps_anchors_first(const bps_anchors *t) { return t->n ? at(t, 0) : NULL; }

const bps_anchor *bps_anchors_last(const bps_anchors *t) { return t->n ? at(t, t->n - 1) : NULL; }

/* The anchors either side of `local_ns`; false if it is outside them all. */
static bool around(const bps_anchors *t, int64_t local_ns, const bps_anchor **a,
                   const bps_anchor **b)
{
    for (uint32_t i = t->n; i >= 2; i--) {
        *a = at(t, i - 2);
        *b = at(t, i - 1);
        if ((*a)->local_ns <= local_ns && local_ns <= (*b)->local_ns)
            return true;
        if ((*a)->local_ns < local_ns)
            return false;
    }
    return false;
}

/* `ya` at `xa` to `yb` at `xb`, at `x`; in doubles relative to `ya`, which
 * keeps nanoseconds over hours. */
static int64_t lerp(int64_t xa, int64_t ya, int64_t xb, int64_t yb, int64_t x)
{
    if (xb == xa)
        return ya;
    double f = (double)(x - xa) / (double)(xb - xa);
    return ya + (x - xa) + (int64_t)llround(f * (double)((yb - xb) - (ya - xa)));
}

bool bps_anchors_server(const bps_anchors *t, int64_t local_ns, int64_t *out)
{
    const bps_anchor *a, *b;
    if (!around(t, local_ns, &a, &b))
        return false;
    *out = lerp(a->local_ns, a->server_ns, b->local_ns, b->server_ns, local_ns);
    return true;
}

/* An access point's clock keeps within this of the server's (crystals are
 * good to tens of ppm); between anchors further apart than that, it was
 * restarted or set while the radio was off. */
#define TSF_MAX_PPM 200
#define TSF_SLACK_NS (100 * 1000)

bool bps_anchors_tsf(const bps_anchors *t, int64_t local_ns, uint8_t bssid[6], int64_t *out)
{
    const bps_anchor *a, *b;
    if (!around(t, local_ns, &a, &b) || !a->has_tsf || !b->has_tsf ||
        memcmp(a->bssid, b->bssid, 6) != 0)
        return false;
    int64_t apart = (b->tsf_ns - a->tsf_ns) - (b->server_ns - a->server_ns);
    if (bps_abs64(apart) > (b->server_ns - a->server_ns) / 1000000 * TSF_MAX_PPM + TSF_SLACK_NS)
        return false;
    memcpy(bssid, a->bssid, 6);
    *out = lerp(a->local_ns, a->tsf_ns, b->local_ns, b->tsf_ns, local_ns);
    return true;
}

/* Where `a`'s clock line puts `b`'s instant, against what `b` measured. */
static int64_t miss(int64_t a_local, int64_t a_y, double rate, int64_t b_local, int64_t b_y)
{
    double dt = (double)(b_local - a_local);
    int64_t predicted = a_y + (b_local - a_local) + (int64_t)llround(dt * rate);
    return b_y - predicted;
}

int64_t bps_anchor_gap_ns(const bps_anchor *a, const bps_anchor *b)
{
    return miss(a->local_ns, a->server_ns, a->rate, b->local_ns, b->server_ns);
}

bool bps_anchor_gap_tsf_ns(const bps_anchor *a, const bps_anchor *b, int64_t *out)
{
    if (!a->has_tsf || !b->has_tsf || memcmp(a->bssid, b->bssid, 6) != 0)
        return false;
    *out = miss(a->local_ns, a->tsf_ns, a->tsf_rate, b->local_ns, b->tsf_ns);
    return true;
}
