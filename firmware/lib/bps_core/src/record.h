/*
 * A copy of every uploaded chunk on the storage card, while one is in: the
 * same FLAC stream the server gets, one file per chunk, under
 * recordings/YYYY-MM-DD/HHMMSS.mmm.flac (UTC, the chunk's start on the
 * server's clock). Each day's index.jsonl gets a line per chunk with its exact
 * start and end, its access point clock times, and how its upload went.
 */
#ifndef BPS_RECORD_H
#define BPS_RECORD_H

#include "bps_common.h"

/* Recording stops while less than this is free, leaving room on the card. */
#define BPS_RECORD_MIN_FREE (16ull << 20)

typedef struct {
    uint32_t files;   /* recordings saved since boot */
    uint64_t bytes;   /* their bytes */
    uint32_t skipped; /* chunks not saved: no card, or it was full */
} bps_record_stats;

/* Starts the copy of a chunk being uploaded. `start_ns` and `end_ns` are on
 * the server's clock; `tsf` is the x-bps-tsf header's value, or NULL; `head`
 * is the FLAC stream header. Does nothing without a card with room. */
void bps_record_begin(int64_t start_ns, int64_t end_ns, const char *tsf, uint32_t sample_rate,
                      uint32_t samples, const uint8_t *head, size_t len);
/* The next FLAC frame of the chunk. */
void bps_record_frame(const uint8_t *frame, size_t len);
/* The chunk ends after `samples` (of which `lost` were sent as silence);
 * `upload` says how its upload went ("sent", "aborted (...)"). */
void bps_record_end(uint32_t samples, uint32_t lost, const char *upload);
void bps_record_stats_get(bps_record_stats *out);

/* "2026-10-25" and "071530.123" for a time in ns since 1970 (UTC), for tests. */
void bps_record_names(int64_t ns, char day[11], char time[11]);

#endif
