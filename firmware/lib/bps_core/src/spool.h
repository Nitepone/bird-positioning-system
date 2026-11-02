/*
 * Audio kept on the storage card until it can be uploaded (batch mode): each
 * chunk as spool/<start>.flac, the FLAC stream the server would have been
 * sent, and once complete spool/<start>.json, its times and size. Uploading
 * waits until anchors (anchor.h) either side of the chunk put it on the
 * server's clock; once the server has it, both files are deleted.
 *
 * A chunk's times are kept on the client's own clock (monotonic plus the
 * epoch set at boot), with an estimate on the server's clock from the fit at
 * the time: if the client restarts, the anchors are gone, and chunks from
 * before go up with that estimate (and no access point clock).
 */
#ifndef BPS_SPOOL_H
#define BPS_SPOOL_H

#include "anchor.h"
#include "bps_common.h"
#include "client.h"
#include "proto.h"

#define BPS_SPOOL_DIR "spool"

typedef struct {
    const bps_settings *cfg;
    const bps_anchors *anchors;
    const int64_t *epoch_ns;
    uint64_t *chunks_sent;
    bool *need_register; /* set when the server has forgotten this client */
    int (*connect_server)(const char *what);
    void (*path_for)(char *out, size_t cap, const char *what);
    int (*read_response)(int h, char *buf, size_t cap, size_t *len, bps_http_response *resp);
} bps_spool_ctx;

void bps_spool_init(const bps_spool_ctx *ctx);

/* ---- Saving (upload.c, in batch mode) ---- */

/* Starts saving a chunk: its start and end on the client's clock, and as
 * the current fit puts them on the server's; `head` is the FLAC stream
 * header. False (nothing saved) without a card with room. */
bool bps_spool_begin(int64_t start_local, int64_t end_local, int64_t start_est, int64_t end_est,
                     uint32_t sample_rate, uint32_t samples, const uint8_t *head, size_t len);
void bps_spool_frame(const uint8_t *frame, size_t len);
/* The chunk is complete; `lost` of its samples were silence (overwritten). */
void bps_spool_end(uint32_t samples, uint32_t lost);
/* The chunk will not be complete: its file is removed. */
void bps_spool_abort(void);

/* ---- Uploading ---- */

/* Starts a pass over the spool, uploading what can be (unless one is going). */
void bps_spool_kick(void);
/* Carries on with the pass; call from the client's loop while connected. */
void bps_spool_poll(uint64_t now);
/* Whether a pass is going. */
bool bps_spool_busy(void);
/* Gives up the pass (the client is disconnecting); the next starts over. */
void bps_spool_drop(void);

typedef struct {
    bool known;       /* the card has been looked at since boot */
    uint32_t items;   /* chunks waiting to be uploaded */
    uint64_t bytes;   /* their bytes, as far as known */
    uint32_t waiting; /* of them, in the last pass, ones still without anchors */
    bool has_oldest;
    int64_t oldest_local_ns; /* the earliest start of a chunk waiting, or saved now */
    uint32_t skipped;        /* chunks not saved since boot: no card, or full */
    bool failing;            /* the last pass stopped on an upload that failed */
} bps_spool_status;
void bps_spool_status_get(bps_spool_status *out);

/* "1762090000123" for a chunk starting at local time `ns`, for tests. */
void bps_spool_name(int64_t local_ns, char out[24]);

#endif
