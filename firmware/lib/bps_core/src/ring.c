#include "ring.h"

#include <stdlib.h>
#include <string.h>

#define LOAD(p) __atomic_load_n(p, __ATOMIC_ACQUIRE)
#define STORE(p, v) __atomic_store_n(p, v, __ATOMIC_RELEASE)

/* The oldest samples may be mid-overwrite by a block in progress; treat this
 * many (more than any capture block) as gone already. */
#define GUARD 4096

uint32_t bps_ring_alloc(bps_ring *r, uint32_t sample_rate, uint32_t max_samples)
{
    memset(r, 0, sizeof *r);
    r->sample_rate = sample_rate;
    uint32_t want = max_samples / BPS_RING_BLOCK;
    if (want > BPS_RING_MAX_BLOCKS)
        want = BPS_RING_MAX_BLOCKS;
    while (r->n_blocks < want) {
        int16_t *b = malloc(BPS_RING_BLOCK * sizeof *b);
        if (!b)
            break;
        r->blocks[r->n_blocks++] = b;
    }
    r->capacity = r->n_blocks * BPS_RING_BLOCK;
    return r->capacity;
}

void bps_ring_write(bps_ring *r, const int16_t *pcm, size_t n, int64_t ts)
{
    if (r->capacity == 0 || n == 0)
        return;
    uint32_t w = r->written;
    /* New segment if the timeline does not continue the current one. */
    bool fresh = r->n_segs == 0;
    if (!fresh) {
        const bps_ring_segment *s = &r->segs[(r->n_segs - 1) % BPS_RING_SEGMENTS];
        int64_t expected = s->ts + bps_samples_to_ns(w - s->idx, r->sample_rate);
        fresh = bps_abs64(ts - expected) > BPS_NANOS_PER_SEC / 2000;
    }
    if (fresh) {
        bps_ring_segment *s = &r->segs[r->n_segs % BPS_RING_SEGMENTS];
        s->idx = w;
        s->ts = ts;
        STORE(&r->n_segs, r->n_segs + 1);
    }
    uint32_t pos = r->wpos;
    for (size_t i = 0; i < n;) {
        uint32_t off = pos % BPS_RING_BLOCK;
        size_t k = BPS_RING_BLOCK - off;
        if (k > n - i)
            k = n - i;
        memcpy(&r->blocks[pos / BPS_RING_BLOCK][off], pcm + i, k * sizeof *pcm);
        i += k;
        pos = (pos + (uint32_t)k) % r->capacity;
    }
    STORE(&r->seq, r->seq + 1);
    STORE(&r->written, w + (uint32_t)n);
    STORE(&r->wpos, pos);
    STORE(&r->seq, r->seq + 1);
}

/* A consistent (written, wpos) pair. */
static void snapshot(const bps_ring *r, uint32_t *w, uint32_t *wpos)
{
    for (;;) {
        uint32_t s = LOAD(&r->seq);
        if (s & 1)
            continue;
        *w = LOAD(&r->written);
        *wpos = LOAD(&r->wpos);
        if (LOAD(&r->seq) == s)
            return;
    }
}

uint32_t bps_ring_written(const bps_ring *r) { return LOAD(&r->written); }

uint32_t bps_ring_oldest(const bps_ring *r)
{
    uint32_t keep = r->capacity > GUARD ? r->capacity - GUARD : 0;
    return LOAD(&r->written) - keep;
}

bool bps_ring_read(const bps_ring *r, uint32_t idx, int16_t *out, size_t n)
{
    if (r->capacity == 0)
        return false;
    uint32_t w, wpos;
    snapshot(r, &w, &wpos);
    uint32_t back = (uint32_t)bps_idx_diff(w, idx);
    if (bps_idx_diff(w, idx) < (int32_t)n || back + GUARD > r->capacity)
        return false;
    uint32_t pos = (wpos + r->capacity - back) % r->capacity;
    for (size_t i = 0; i < n;) {
        uint32_t off = pos % BPS_RING_BLOCK;
        size_t k = BPS_RING_BLOCK - off;
        if (k > n - i)
            k = n - i;
        memcpy(out + i, &r->blocks[pos / BPS_RING_BLOCK][off], k * sizeof *out);
        i += k;
        pos = (pos + (uint32_t)k) % r->capacity;
    }
    /* Still valid only if the producer did not lap us while copying. */
    w = LOAD(&r->written);
    return (uint32_t)bps_idx_diff(w, idx) + GUARD <= r->capacity;
}

bool bps_ring_locate(const bps_ring *r, uint32_t idx, int64_t *ts, bool *has_next,
                     uint32_t *next_idx)
{
    for (;;) {
        uint32_t n = LOAD(&r->n_segs);
        uint32_t avail = n < BPS_RING_SEGMENTS ? n : BPS_RING_SEGMENTS;
        bool found = false;
        bps_ring_segment seg = {0, 0}, next = {0, 0};
        bool nxt = false;
        for (uint32_t k = 0; k < avail; k++) {
            bps_ring_segment s = r->segs[(n - 1 - k) % BPS_RING_SEGMENTS];
            if (bps_idx_diff(idx, s.idx) >= 0) {
                seg = s;
                found = true;
                break;
            }
            next = s;
            nxt = true;
        }
        /* Retry if segments were added (and possibly overwritten) meanwhile. */
        if (LOAD(&r->n_segs) != n)
            continue;
        if (!found)
            return false;
        *ts = seg.ts + bps_samples_to_ns(idx - seg.idx, r->sample_rate);
        *has_next = nxt;
        *next_idx = next.idx;
        return true;
    }
}
