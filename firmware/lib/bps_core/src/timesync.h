/*
 * NTP-style UDP clock measurement: a C port of `bsp_proto::timesync` (packet
 * codec and `Estimator`) and of the `Clock` in bsp-client's timesync.rs.
 *
 * Request (client -> server, 36 bytes): magic, version, kind=1, seq, client id, t1.
 * Response (server -> client, 36 bytes): magic, version, kind=2, seq, t1 (echoed), t2, t3.
 * The magic is the wire protocol's "BSP", which the server expects.
 */
#ifndef BPS_TIMESYNC_H
#define BPS_TIMESYNC_H

#include "bps_common.h"

#define BPS_TS_REQUEST_LEN 36
#define BPS_TS_RESPONSE_LEN 36

/* Fewer than the Rust estimator's 4096: 64 s of measurements at 2 Hz is 128. */
#ifndef BPS_EST_MAX_SAMPLES
#define BPS_EST_MAX_SAMPLES 192
#endif
#define BPS_EST_MAX_BINS 16 /* the client uses BPS_CLOCK_BINS */
/* Round trips kept per heartbeat interval for the min/median/max report. */
#define BPS_CLOCK_MAX_RTTS 64

typedef struct {
    uint32_t seq;
    uint8_t client_id[16];
    int64_t t1;
} bps_ts_request;

typedef struct {
    uint32_t seq;
    int64_t t1, t2, t3;
} bps_ts_response;

/* One clock measurement, on the client's clock. */
typedef struct {
    int64_t at;        /* client time halfway through the exchange */
    int64_t offset_ns; /* server clock minus client clock */
    int64_t rtt_ns;
} bps_ts_sample;

/* Offset and drift of the client clock, fitted over recent measurements. */
typedef struct {
    int64_t at;       /* client time the fit is anchored at */
    double offset_ns; /* server minus client at `at` */
    double rate;      /* ns of offset change per ns of client time */
    int64_t rtt_ns;   /* lowest round trip used */
    int64_t error_ns; /* half that round trip plus the scatter around the line */
    uint32_t points;
} bps_ts_fit;

void bps_ts_request_encode(const bps_ts_request *r, uint8_t out[BPS_TS_REQUEST_LEN]);
bool bps_ts_request_decode(const uint8_t *b, size_t len, bps_ts_request *out);
void bps_ts_response_encode(const bps_ts_response *r, uint8_t out[BPS_TS_RESPONSE_LEN]);
bool bps_ts_response_decode(const uint8_t *b, size_t len, bps_ts_response *out);

/* `offset` is server clock minus client clock, given client receive time `t4`. */
void bps_ts_offset_rtt(const bps_ts_response *r, int64_t t4, int64_t *offset, int64_t *rtt);
/* The measurement a reply completes; false if a clock stepped during the exchange. */
bool bps_ts_sample_of(const bps_ts_response *r, int64_t t4, bps_ts_sample *out);

int64_t bps_fit_offset_at(const bps_ts_fit *f, int64_t t);
double bps_fit_drift_ppm(const bps_ts_fit *f);

/* See `Estimator` in bsp-proto: fastest measurement per bin, weighted line fit,
 * and a run of disagreeing measurements counts as a clock step. */
typedef struct {
    int64_t window_ns;
    uint32_t bins;
    bps_ts_sample samples[BPS_EST_MAX_SAMPLES];
    uint32_t head, len;
    bool has_fit;
    bps_ts_fit fit;
    bool disagreeing;
    int64_t disagreeing_since;
    uint32_t disagreements;
    /* Refit scratch: the fastest measurement per bin. Kept here, not on the
     * stack, which is only 2-4 KB per core on a Pico. */
    bps_ts_sample best[2 * BPS_EST_MAX_BINS + 2];
} bps_estimator;

void bps_est_init(bps_estimator *e, int64_t window_ns, uint32_t bins);
/* Adds a measurement; returns the updated fit (NULL only before the first). */
const bps_ts_fit *bps_est_push(bps_estimator *e, bps_ts_sample s);
const bps_ts_fit *bps_est_fit(const bps_estimator *e);

/* Clock status for a heartbeat, as `bsp_proto::ClockStatus` + `SyncDetail`. */
typedef struct {
    int64_t offset_ns;
    int64_t rtt_ns;
    int64_t measured_at;
    int64_t error_ns;
    bool corrects_timestamps;
    int64_t clock_offset_ns;
    double drift_ppm;
    uint32_t fit_points;
    uint32_t requests;
    uint32_t replies;
    bool has_rtts;
    int64_t rtt_min_ns, rtt_median_ns, rtt_max_ns;
} bps_clock_status;

/* The clock estimate, and what has been done with it. */
typedef struct {
    bool corrects;
    bps_estimator est;
    bool has_last;
    int64_t last_at;
    bool has_applied;
    int64_t applied_mid, applied_corr;
    int64_t half_chunk_ns;
    uint32_t requests;
    int64_t rtts[BPS_CLOCK_MAX_RTTS];
    uint32_t n_rtts;
} bps_clock;

/* Measurements older than this are forgotten, and the window is cut into this many bins. */
#define BPS_CLOCK_WINDOW_NS (64 * BPS_NANOS_PER_SEC)
#define BPS_CLOCK_BINS 16

void bps_clock_init(bps_clock *c, bool corrects);
/* The timestamp to send for audio captured from client time `start` for
 * `len_ns`: on the server's clock, evaluated at the chunk's midpoint. */
int64_t bps_clock_correct(bps_clock *c, int64_t start, int64_t len_ns);
/* Counts a request once it was answered or timed out. */
void bps_clock_count_request(bps_clock *c);
void bps_clock_add_sample(bps_clock *c, bps_ts_sample s);
/* Status for the next heartbeat; starts a new interval for the counts.
 * False while there is no estimate yet. */
bool bps_clock_status_take(bps_clock *c, int64_t now, bps_clock_status *out);

#endif
