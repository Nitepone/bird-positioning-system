/*
 * Recent audio, written by the capture core and read by the network core
 * (single producer, single consumer).
 *
 * Samples are addressed by a running 32-bit index (wraps after ~25 h at
 * 48 kHz; all comparisons are wrap-safe). Storage is a list of fixed-size
 * blocks, so it can use a fragmented heap. Each run of samples with a
 * continuous timeline is a *segment*; a new one starts whenever the timeline
 * jumps by more than 0.5 ms, as bsp-client's Chunker ends a chunk there.
 */
#ifndef BPS_RING_H
#define BPS_RING_H

#include "bps_common.h"

#define BPS_RING_BLOCK 8192 /* samples per block (16 KB) */
#define BPS_RING_MAX_BLOCKS 64
#define BPS_RING_SEGMENTS 8

typedef struct {
    uint32_t idx; /* first sample of the segment */
    int64_t ts;   /* its capture time (monotonic ns) */
} bps_ring_segment;

typedef struct {
    uint32_t sample_rate;
    int16_t *blocks[BPS_RING_MAX_BLOCKS];
    uint32_t n_blocks;
    uint32_t capacity; /* samples */
    /* Seqlock-protected pair: samples written so far, and where the next one
     * goes (a running index cannot address storage that is not a power of two
     * in size across its wrap). */
    uint32_t seq;
    uint32_t written;
    uint32_t wpos;
    bps_ring_segment segs[BPS_RING_SEGMENTS];
    uint32_t n_segs; /* segments started so far */
} bps_ring;

/* Signed distance from index `a` to index `b` (wrap-safe). */
static inline int32_t bps_idx_diff(uint32_t b, uint32_t a) { return (int32_t)(b - a); }

/* Allocates up to `max_samples` of storage (rounded down to whole blocks);
 * returns the capacity actually obtained. */
uint32_t bps_ring_alloc(bps_ring *r, uint32_t sample_rate, uint32_t max_samples);

/* Producer. `ts` is the capture time of the first sample. */
void bps_ring_write(bps_ring *r, const int16_t *pcm, size_t n, int64_t ts);

/* Consumer. */
uint32_t bps_ring_written(const bps_ring *r);
/* Copies `n` samples starting at `idx`; false if any were already overwritten
 * (or are about to be: the oldest 4096 samples count as gone). */
bool bps_ring_read(const bps_ring *r, uint32_t idx, int16_t *out, size_t n);
/* The oldest sample still readable (older ones are gone or about to be). */
uint32_t bps_ring_oldest(const bps_ring *r);
/* The segment holding sample `idx`: its capture time, and the index where the
 * next segment starts (`*has_next` false when it is the current one). False
 * if `idx` is older than every remembered segment. */
bool bps_ring_locate(const bps_ring *r, uint32_t idx, int64_t *ts, bool *has_next,
                     uint32_t *next_idx);

#endif
