/*
 * Audio uploads, for the client (client.c): once the gate opens, a chunk
 * starting a look-back before the sound is encoded as FLAC into the store
 * (store.h) as it is captured, and uploaded once the whole chunk is there,
 * in one burst. Encoding goes on whatever the network does; uploading needs
 * it. Validation (validate.h) runs on the chunk meanwhile, fed its window as
 * it is encoded, and a chunk it rejects is dropped from the store unsent.
 *
 * Boards whose store cannot hold a whole chunk (no PSRAM) stream instead:
 * the chunk goes up as it is encoded, as soon as validation has let it
 * through, and its bytes leave the store as they are sent.
 *
 * In batch mode chunks go to the card (spool.h) instead, as encoded.
 */
#ifndef BPS_UPLOAD_H
#define BPS_UPLOAD_H

#include "beacon.h"
#include "bps_common.h"
#include "capture.h"
#include "client.h"
#include "proto.h"
#include "ring.h"
#include "store.h"
#include "timesync.h"
#include "validate.h"

/* Kept free between the oldest sample encoded and the ring's write position. */
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
    bps_store *store;             /* recordings waiting to go */
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

/* Once the ring buffer, the store and capture are set up. */
void bps_upload_init(const bps_upload_ctx *ctx);
/* Changes the look-back (samples), as the ring buffer is resized: chunks
 * started from now on use it. */
void bps_upload_set_lookback(uint32_t samples);
/* The least store that holds a whole chunk of `chunk_len` samples: smaller,
 * chunks stream as they are encoded. */
uint32_t bps_upload_store_least(uint32_t chunk_len);
/* While on, stored chunks wait: none starts going up (one going up goes on).
 * Ultra-low-power mode lets them go together, in its wakes. */
void bps_upload_hold(bool on);
/* The start (server clock) of the oldest chunk in the store, being recorded
 * or waiting to go; false if there is none. */
bool bps_upload_oldest_start(int64_t *start_ns);
/* Starts and encodes chunks, and follows their validation: call from the
 * client's loop whenever it captures, connected or not. */
void bps_upload_poll(uint64_t now);
/* Uploads what the store holds: call from the client's loop while connected. */
void bps_upload_send_poll(uint64_t now);

/* Nothing being encoded, checked, stored or sent. */
bool bps_upload_idle(void);
/* A chunk is being encoded (into the store, or onto the card). */
bool bps_upload_encoding(void);
/* Chunks kept so far (complete in the store, or saved on the card): not
 * rejected or given up. */
uint32_t bps_upload_kept(void);
/* An upload is under way (connected to the server). */
bool bps_upload_sending(void);
/* A stored chunk could go now: the radio is wanted (low-power mode). */
bool bps_upload_ready(uint64_t now);
/* Notes which ring indices kept between chunks (the gate's latest pass, the
 * end of the last chunk, a rejected window, the chunk being encoded) have
 * left the ring: left alone for 2^31 samples (12.4 h at 48 kHz: a quiet
 * gate, or the loop held up that long) they would compare as the future, and
 * hold back every chunk, or start bogus ones, for as long again. Call from
 * every loop, whatever the client's state. */
void bps_upload_keep_recent(void);
/* The ring index just past the current (or latest) chunk. */
uint32_t bps_upload_end(void);
/* Gives up the upload in progress (the client is reconnecting): a stored
 * chunk goes again later, one streamed is lost. Encoding goes on. */
void bps_upload_drop(void);
/* From the next chunk on, save chunks on the card (spool.h) instead of
 * uploading them: batch mode. Chunks already stored still go up. */
void bps_upload_set_spool(bool on);

typedef struct {
    bool streaming;       /* the store cannot hold a chunk: chunks stream */
    uint32_t cap;         /* the store's bytes */
    uint32_t queued;      /* chunks in it (complete or being encoded) */
    uint32_t bytes;       /* bytes they hold */
    uint32_t peak_bytes;  /* the most it held since boot */
    uint32_t dropped;     /* chunks dropped unsent: the store was full, or the server refused them */
    uint64_t oldest_us;   /* when the oldest complete chunk was stored; 0 if none */
} bps_upload_status;
void bps_upload_status_get(bps_upload_status *out);

#endif
