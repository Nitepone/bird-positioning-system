/*
 * Audio uploads, for the client (client.c): once the gate opens, a chunk
 * starting a look-back before the sound, streamed to the server as FLAC.
 */
#ifndef BPS_UPLOAD_H
#define BPS_UPLOAD_H

#include "bps_common.h"
#include "capture.h"
#include "client.h"
#include "proto.h"
#include "ring.h"
#include "timesync.h"

/* Kept free between the oldest sample uploaded and the ring's write position. */
#define BPS_UPLOAD_SLACK_MS 400

/* What uploads use of the client: its state (owned by client.c) and helpers. */
typedef struct {
    const bps_settings *cfg;
    const bps_ring *ring;
    const bps_capture *cap;
    bps_clock *clk;
    const int64_t *epoch_ns; /* local time is monotonic time plus this */
    const bool *epoch_set;
    uint32_t chunk_len, lookback; /* samples */
    uint64_t *chunks_sent;
    uint32_t *overruns;  /* counts chunks that lost audio */
    bool *need_register; /* set when the server has forgotten this client */
    int (*connect_server)(const char *what);
    void (*path_for)(char *out, size_t cap, const char *what);
    int (*read_response)(int h, char *buf, size_t cap, size_t *len, bps_http_response *resp);
    const char *(*centi)(char *buf, size_t cap, int32_t v);
} bps_upload_ctx;

/* Once the ring buffer and capture are set up. */
void bps_upload_init(const bps_upload_ctx *ctx);
/* Starts, sends or finishes an upload; call from the client's loop. */
void bps_upload_poll(uint64_t now);
bool bps_upload_idle(void);
/* The ring index just past the current (or latest) chunk. */
uint32_t bps_upload_end(void);
/* Gives up the upload in progress (the client is reconnecting); the next one
 * continues where it stopped. */
void bps_upload_drop(void);

#endif
