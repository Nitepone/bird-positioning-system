/*
 * Timestamps for audio recorded while the radio was off (batch mode).
 *
 * Whenever the client is in contact and its clock measurement is fresh, it
 * notes where its clock stands against the server's, and against its access
 * point's (TSF) if it follows one: an anchor. Audio recorded between two
 * anchors goes on those clocks by interpolating between them, which takes out
 * whatever the drift was over the gap; what is left is how far the drift
 * changed along the way (for a crystal, microseconds over minutes).
 */
#ifndef BPS_ANCHOR_H
#define BPS_ANCHOR_H

#include "bps_common.h"

/* Two per contact: 64 cover over 2.5 hours of contacts every 5 minutes. */
#define BPS_ANCHORS 64

typedef struct {
    int64_t local_ns;  /* the client's clock: monotonic time plus its epoch */
    int64_t server_ns; /* the server's clock at the same instant, by the fit */
    double rate;       /* the fit's drift: ns of offset change per ns */
    bool has_tsf;
    uint8_t bssid[6];
    int64_t tsf_ns;   /* the access point's clock at the same instant */
    double tsf_rate;  /* the beacon line's drift, as `rate` */
    uint32_t session; /* the contact it was taken in */
} bps_anchor;

typedef struct {
    bps_anchor a[BPS_ANCHORS];
    uint32_t head, n;
} bps_anchors;

void bps_anchors_init(bps_anchors *t);
/* Adds an anchor, later than any before. Within one contact the fit holds
 * throughout, so only its first anchor and its latest are kept. */
void bps_anchors_push(bps_anchors *t, const bps_anchor *a);
/* The earliest anchor kept and the latest, or NULL. */
const bps_anchor *bps_anchors_first(const bps_anchors *t);
const bps_anchor *bps_anchors_last(const bps_anchors *t);
/* Local time `local_ns` on the server's clock, between the anchors either
 * side of it. False if it is not between two anchors. */
bool bps_anchors_server(const bps_anchors *t, int64_t local_ns, int64_t *out);
/* The same on the access point's clock: both anchors must follow the same
 * access point (put in `bssid`), whose clock must not have jumped between
 * them (the AP restarted). */
bool bps_anchors_tsf(const bps_anchors *t, int64_t local_ns, uint8_t bssid[6], int64_t *out);
/* How far `b` is from where `a`'s drift predicted it, on the server's clock
 * and (if both follow the same access point) the TSF: what the drift
 * changed over the gap between them. */
int64_t bps_anchor_gap_ns(const bps_anchor *a, const bps_anchor *b);
bool bps_anchor_gap_tsf_ns(const bps_anchor *a, const bps_anchor *b, int64_t *out);

#endif
