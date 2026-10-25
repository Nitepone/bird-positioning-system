#include "beacon.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* A beacon this far off the line means the AP's clock restarted (or jumped). */
#define RESTART_NS BPS_NANOS_PER_MS
/* The line needs this many beacons, over at least this long. */
#define MIN_POINTS 8
#define MIN_SPAN_NS (2 * BPS_NANOS_PER_SEC)

void bps_beacon_init(bps_beacon *b) { memset(b, 0, sizeof *b); }

static void restart(bps_beacon *b)
{
    b->head = b->n = 0;
    b->has_fit = false;
    b->restarts++;
}

static void refit(bps_beacon *b)
{
    const bps_beacon_point *p0 = &b->pts[b->head];
    const bps_beacon_point *pn = &b->pts[(b->head + b->n - 1) % BPS_BEACON_POINTS];
    if (b->n < MIN_POINTS || pn->mono_ns - p0->mono_ns < MIN_SPAN_NS)
        return;
    /* Relative to the oldest beacon, and to a rate of exactly 1. */
    static double x[BPS_BEACON_POINTS], y[BPS_BEACON_POINTS], r[BPS_BEACON_POINTS];
    static bool use[BPS_BEACON_POINTS];
    uint32_t n = b->n;
    for (uint32_t i = 0; i < n; i++) {
        const bps_beacon_point *p = &b->pts[(b->head + i) % BPS_BEACON_POINTS];
        x[i] = (double)(p->mono_ns - p0->mono_ns);
        y[i] = (double)(p->tsf_ns - p0->tsf_ns) - x[i];
        use[i] = true;
    }
    double a = 0, s = 0;
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
        s = sxx > 0 ? sxy / sxx : 0;
        a = my - s * mx;
        double ss = 0;
        for (uint32_t i = 0; i < n; i++) {
            r[i] = y[i] - (a + s * x[i]);
            if (use[i])
                ss += r[i] * r[i];
        }
        double rms = sqrt(ss / k);
        b->scatter_ns = (int64_t)llround(rms);
        b->fit_points = (uint32_t)k;
        /* Leave out beacons far off the line (a late timestamp, a bad frame). */
        if (pass == 0)
            for (uint32_t i = 0; i < n; i++)
                use[i] = fabs(r[i]) <= fmax(4 * rms, 2000.0);
    }
    double xn = x[n - 1];
    b->at_mono = pn->mono_ns;
    b->at_tsf = p0->tsf_ns + (int64_t)llround(xn + a + s * xn);
    b->rate = s;
    b->has_fit = true;
}

void bps_beacon_push(bps_beacon *b, const uint8_t bssid[6], uint64_t tsf_us, int64_t mono_ns)
{
    if (!b->has_bssid || memcmp(b->bssid, bssid, 6) != 0) {
        /* Another access point (roaming): another clock. */
        memcpy(b->bssid, bssid, 6);
        b->has_bssid = true;
        restart(b);
    }
    int64_t tsf_ns = (int64_t)tsf_us * 1000;
    if (b->has_fit && bps_abs64(tsf_ns - bps_beacon_tsf_at(b, mono_ns)) > RESTART_NS)
        restart(b);
    if (b->n == BPS_BEACON_POINTS) {
        b->head = (b->head + 1) % BPS_BEACON_POINTS;
        b->n--;
    }
    bps_beacon_point *p = &b->pts[(b->head + b->n) % BPS_BEACON_POINTS];
    p->mono_ns = mono_ns;
    p->tsf_ns = tsf_ns;
    b->n++;
    b->heard++;
    b->last_mono = mono_ns;
    refit(b);
}

bool bps_beacon_ready(const bps_beacon *b, int64_t now_mono)
{
    return b->has_fit && now_mono - b->last_mono < BPS_BEACON_STALE_NS;
}

int64_t bps_beacon_tsf_at(const bps_beacon *b, int64_t mono_ns)
{
    double d = (double)(mono_ns - b->at_mono);
    return b->at_tsf + (mono_ns - b->at_mono) + (int64_t)llround(d * b->rate);
}

void bps_beacon_bssid(const bps_beacon *b, char out[18])
{
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x", b->bssid[0], b->bssid[1], b->bssid[2],
             b->bssid[3], b->bssid[4], b->bssid[5]);
}
