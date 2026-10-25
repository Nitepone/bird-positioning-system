/*
 * Follows the clock of the access point the board is connected to, from its
 * beacons. Each beacon carries the AP's clock (TSF) and arrives at a known
 * monotonic time; a line through the last BPS_BEACON_POINTS of them maps the
 * monotonic clock to the TSF. Every client of the same AP hears the same
 * beacons, so audio timestamped in its TSF lines up across those clients to
 * microseconds. The server converts each AP's TSF to its own clock.
 */
#ifndef BPS_BEACON_H
#define BPS_BEACON_H

#include "bps_common.h"

#define BPS_BEACON_POINTS 128 /* about 15-20 s of beacons */
/* Without a beacon for this long, the line is not trusted. */
#define BPS_BEACON_STALE_NS (5 * BPS_NANOS_PER_SEC)

typedef struct {
    int64_t mono_ns;
    int64_t tsf_ns;
} bps_beacon_point;

typedef struct {
    uint8_t bssid[6];
    bool has_bssid;
    bps_beacon_point pts[BPS_BEACON_POINTS];
    uint32_t head, n;
    /* tsf = at_tsf + (mono - at_mono) * (1 + rate) */
    bool has_fit;
    int64_t at_mono, at_tsf;
    double rate;
    int64_t scatter_ns; /* RMS distance of the beacons from the line */
    uint32_t fit_points;
    int64_t last_mono;
    uint32_t heard, restarts;
} bps_beacon;

void bps_beacon_init(bps_beacon *b);
/* A beacon of `bssid` carrying `tsf_us` arrived at monotonic time `mono_ns`. */
void bps_beacon_push(bps_beacon *b, const uint8_t bssid[6], uint64_t tsf_us, int64_t mono_ns);
/* Whether the line is fitted and recent beacons back it at `now_mono`. */
bool bps_beacon_ready(const bps_beacon *b, int64_t now_mono);
/* The AP's TSF (ns) at monotonic time `mono_ns`. */
int64_t bps_beacon_tsf_at(const bps_beacon *b, int64_t mono_ns);
/* "aa:bb:cc:dd:ee:ff" */
void bps_beacon_bssid(const bps_beacon *b, char out[18]);

#endif
