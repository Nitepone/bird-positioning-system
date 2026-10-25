/*
 * Recent audio, written by the capture core and read by the network core
 * (single producer, single consumer).
 *
 * Samples are addressed by a running 32-bit index (wraps after ~25 h at
 * 48 kHz; all comparisons are wrap-safe). Storage is a list of fixed-size
 * blocks, so it can use a fragmented heap. Each run of samples with a
 * continuous timeline is a *segment*, which starts where the timestamper
 * started a timeline (at a jump of more than 0.5 ms, as bsp-client's Chunker
 * ends a chunk there). A segment's timeline is refined in place as the
 * timestamper learns more, so its samples' times are the latest estimate.
 */
#ifndef BPS_RING_H
#define BPS_RING_H

#include "bps_common.h"
#include "timestamper.h"

#define BPS_RING_BLOCK 8192 /* samples per block (16 KB) */
#define BPS_RING_MAX_BLOCKS 64
#define BPS_RING_SEGMENTS 8

typedef struct {
    uint32_t idx;         /* first sample of the segment */
    bps_timeline tl;      /* its samples' capture times (monotonic ns) */
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
    uint32_t tseq;   /* seqlock for changes to the segments */
} bps_ring;

/* Signed distance from index `a` to index `b` (wrap-safe). */
static inline int32_t bps_idx_diff(uint32_t b, uint32_t a) { return (int32_t)(b - a); }

/* Allocates up to `max_samples` of storage (rounded down to whole blocks);
 * returns the capacity actually obtained. */
uint32_t bps_ring_alloc(bps_ring *r, uint32_t sample_rate, uint32_t max_samples);

/* Producer. */
void bps_ring_write(bps_ring *r, const int16_t *pcm, size_t n);
/* Producer: starts a segment at sample `start` (not before the current
 * one's start, nor after the next sample to be written) with timeline `tl`,
 * or with `start_new` false, sets the current segment's timeline. */
void bps_ring_timeline(bps_ring *r, const bps_timeline *tl, bool start_new, uint32_t start);

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
