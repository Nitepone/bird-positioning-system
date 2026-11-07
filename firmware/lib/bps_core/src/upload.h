/*
 * Audio uploads, for the client (client.c): once the gate opens, a chunk
 * starting a look-back before the sound, streamed to the server as FLAC.
 */
#ifndef BPS_UPLOAD_H
#define BPS_UPLOAD_H

#include "beacon.h"
#include "bps_common.h"
#include "capture.h"
#include "client.h"
#include "proto.h"
#include "ring.h"
#include "timesync.h"
#include "validate.h"

/* Kept free between the oldest sample uploaded and the ring's write position. */
#define BPS_UPLOAD_SLACK_MS 400

/* What uploads use of the client: its state (owned by client.c) and helpers. */
typedef struct {
    const bps_settings *cfg;
    const bps_ring *ring;
    const bps_capture *cap;
    bps_clock *clk;
    const bps_beacon *beacon; /* the access point's clock, when it is followed */
    const int64_t *epoch_ns;  /* local time is monotonic time plus this */
    const bool *epoch_set;
    uint32_t chunk_len, lookback; /* samples */
    uint64_t *chunks_sent;
    uint32_t *overruns;  /* counts chunks that lost audio */
    bool *need_register; /* set when the server has forgotten this client */
    int (*connect_server)(const char *what);
    void (*path_for)(char *out, size_t cap, const char *what);
    int (*read_response)(int h, char *buf, size_t cap, size_t *len, bps_http_response *resp);
    const char *(*centi)(char *buf, size_t cap, int32_t v);
    /* Validation's decision on a triggered chunk (validate.h), uploaded or not;
     * `latency_us` from the trigger to the decision. */
    void (*on_validated)(const bps_validate_result *r, uint64_t latency_us);
    uint64_t *chunks_rejected;
} bps_upload_ctx;

/* Once the ring buffer and capture are set up. */
void bps_upload_init(const bps_upload_ctx *ctx);
/* Starts, sends or finishes an upload; call from the client's loop. */
void bps_upload_poll(uint64_t now);
bool bps_upload_idle(void);
/* A chunk is being uploaded or saved (not waiting for validation, or to
 * connect). */
bool bps_upload_sending(void);
/* bps_upload_poll() needs no network for now: the chunk in progress, or the
 * next one, goes to the card (batch mode). Call it then while the client
 * connects, or audio is lost meanwhile. */
bool bps_upload_offline(void);
/* Notes which ring indices kept between uploads (the gate's latest pass, the
 * end of the last chunk, a rejected window, a chunk waiting for validation or
 * to connect) have left the ring: left alone for 2^31 samples (12.4 h at
 * 48 kHz: a quiet gate, or Wi-Fi down that long, when uploads are not polled)
 * they would compare as the future, and hold back every upload, or start
 * bogus ones, for as long again. Call from every loop, whatever the client's
 * state. */
void bps_upload_keep_recent(void);
/* The ring index just past the current (or latest) chunk. */
uint32_t bps_upload_end(void);
/* Gives up the upload in progress (the client is reconnecting); the next one
 * continues where it stopped. A chunk being saved on the card goes on. */
void bps_upload_drop(void);
/* From the next chunk on, save chunks on the card (spool.h) instead of
 * uploading them: batch mode. */
void bps_upload_set_spool(bool on);

#endif
