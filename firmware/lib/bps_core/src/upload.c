#include "upload.h"

#include <stdio.h>
#include <string.h>

#include "flac.h"
#include "port.h"
#include "record.h"
#include "spool.h"

#define LOAD(p) __atomic_load_n(p, __ATOMIC_ACQUIRE)

/* The ring's oldest samples count as gone this early (ring.c's guard). */
#define RING_GUARD 4096
#define UPLOAD_STALL_US 5000000
/* For the server's answer, as for the client's other requests. */
#define ANSWER_TIMEOUT_US 10000000
/* A complete chunk waits this long for validation's decision, then goes. */
#define VERDICT_WAIT_US 5000000
/* After a failed upload, the chunk goes again this much later (doubling up to
 * the most), and is dropped after this many tries. */
#define RETRY_US 1000000
#define RETRY_MAX_US 30000000
#define MAX_TRIES 6
/* Bytes from the store per HTTP chunk. */
#define SEND_PIECE 4096

/* Validation's decision on a chunk. */
enum { V_NONE, V_WAIT, V_PASS, V_REJECT };
/* The sender. */
enum { S_IDLE, S_SEND, S_RESP };

/* The client's state and helpers, from bps_upload_init(). */
static bps_upload_ctx c;

/* Chunks from the next one on are saved on the card (batch mode). */
static bool to_spool;
/* Stored chunks wait (ultra-low-power mode): none starts going up. */
static bool hold;

/* What is known of each chunk in the store, by its slot there. */
typedef struct {
    uint32_t id;
    int64_t start_ns, end_ns; /* on the server's clock, from the fit when it began */
    char tsf[80];             /* the x-bps-tsf header's value, or empty */
    uint32_t samples, lost;
    int verdict;
    uint64_t stored_us; /* when its encoding was done */
    uint32_t tries;
    bool dead; /* streamed, and the upload failed: given up */
    bps_record_file card;
} chunk_meta;
static chunk_meta *meta;

static uint32_t next_id = 1;
static uint32_t kept; /* chunks stored, or saved on the card */
static bool streaming; /* the store cannot hold a whole chunk */

static struct {
    uint32_t dropped, peak;
} st;

/* Which chunk comes next: the gate's passes, and what was handled of them. */
static struct {
    /* Passes up to here were handled (a chunk covered them, or was given up).
     * prev_old: that end has left the ring, so ring indices no longer compare
     * with it (they would wrap), and every pass since is new. */
    bool has_prev, prev_old;
    uint32_t prev_end;
    /* The gate's latest pass has left the ring: the next is new once the
     * pass counter has moved on from pass_seen. */
    bool pass_old;
    uint32_t pass_seen;
    /* Gate passes up to here were in a window validation rejected. */
    bool has_skip, skip_old;
    uint32_t skip_until, skip_count;
    uint64_t retry_us;
} sel;

/* The chunk being encoded. */
static struct {
    bool on;
    bool spool; /* onto the card (batch mode), not into the store */
    int slot;   /* in the store */
    uint32_t id;
    uint32_t start, end, next, segs;
    uint32_t frame, lost;
    /* Spool: complete, its .json held back until validation decides. */
    bool spool_wait;
    int spool_verdict;
    uint64_t done_us;
    int16_t pcm[BPS_FLAC_BLOCK];
    uint8_t buf[BPS_FLAC_MAX_FRAME];
} enc = {.slot = -1};

/* Validation of a chunk: collecting its window, or the model running. */
static struct {
    bool on;
    int slot; /* -1: the chunk being spooled */
    uint32_t id;
    uint32_t win, trigger;
    uint64_t since_us;
} chk;

/* The upload in progress. */
static struct {
    int state, h, slot;
    uint32_t id;
    uint32_t off; /* bytes of the chunk handed over */
    bool ended;   /* the body's closing chunk is in the buffer */
    /* The request head, or a piece of the chunk as one HTTP chunk: size line,
     * bytes, CRLF, and after the last the closing empty chunk. */
    uint8_t buf[6 + SEND_PIECE + 2 + 5];
    size_t len, pos;
    char rx[256];
    size_t rx_len;
    bps_http_response resp;
    uint64_t progress_us, deadline_us, retry_us;
    /* Logged when the upload ends. */
    struct {
        uint64_t begun_us, sent_us; /* sent_us: when the last byte was handed over */
        uint32_t connect_ms;
        uint64_t bytes; /* written to the connection */
    } d;
} snd = {.h = -1, .slot = -1};

static unsigned long samples_ms(uint64_t n)
{
    return (unsigned long)(n * 1000 / c.cfg->sample_rate);
}

#ifdef BPS_NET_DEBUG
/* Upload writes that make no progress for longer count as a stall. */
#define UPLOAD_STALL_LOG_US 200000

/* More about the current upload, logged when it ends. */
static struct {
    uint64_t stall_max_us, write_max_us;
    uint32_t stalls;
    bool tcp_seen;
    uint32_t retransmits, nrtx, rto_max_ms, cwnd_min, wnd_min, wnd_zero;
} upd;

static void diag_upload_begin(void)
{
    memset(&upd, 0, sizeof upd);
    upd.cwnd_min = upd.wnd_min = UINT32_MAX;
}

/* Notes the connection's TCP state (where the board can tell). */
static void diag_upload_pump(void)
{
    port_tcp_stats ts;
    if (!port_tcp_get_stats(snd.h, &ts))
        return;
    upd.tcp_seen = true;
    if (ts.retransmits > upd.nrtx) /* per segment: it restarts at 0 once acknowledged */
        upd.retransmits += ts.retransmits - upd.nrtx;
    upd.nrtx = ts.retransmits;
    if (ts.rto_ms > upd.rto_max_ms)
        upd.rto_max_ms = ts.rto_ms;
    if (ts.cwnd < upd.cwnd_min)
        upd.cwnd_min = ts.cwnd;
    if (ts.peer_wnd < upd.wnd_min)
        upd.wnd_min = ts.peer_wnd;
    if (ts.peer_wnd == 0)
        upd.wnd_zero++;
}

static void diag_upload_write(uint64_t call_us)
{
    if (call_us > upd.write_max_us)
        upd.write_max_us = call_us;
}

/* A write that made progress, `gap_us` after the previous one. */
static void diag_upload_progress(uint64_t gap_us)
{
    if (gap_us > upd.stall_max_us)
        upd.stall_max_us = gap_us;
    if (gap_us > UPLOAD_STALL_LOG_US)
        upd.stalls++;
}

static void diag_upload_report(void)
{
    port_log("upload:   %lu stalls over %lu ms, longest %lu ms; slowest write call %lu ms",
             (unsigned long)upd.stalls, (unsigned long)(UPLOAD_STALL_LOG_US / 1000),
             (unsigned long)(upd.stall_max_us / 1000), (unsigned long)(upd.write_max_us / 1000));
    if (upd.tcp_seen)
        port_log("upload:   tcp: %lu retransmits, timeout up to %lu ms, cwnd down to %lu B, server "
                 "window down to %lu B (0 seen %lu times)",
                 (unsigned long)upd.retransmits, (unsigned long)upd.rto_max_ms,
                 (unsigned long)upd.cwnd_min, (unsigned long)upd.wnd_min,
                 (unsigned long)upd.wnd_zero);
}
#else
static inline void diag_upload_begin(void) {}
static inline void diag_upload_pump(void) {}
static inline void diag_upload_write(uint64_t call_us) { (void)call_us; }
static inline void diag_upload_progress(uint64_t gap_us) { (void)gap_us; }
static inline void diag_upload_report(void) {}
#endif

/* ---- The store ---- */

static bool has_store(void) { return c.store && c.store->cap && meta; }

static void note_peak(void)
{
    uint32_t used = c.store->cap - bps_store_free(c.store);
    if (used > st.peak)
        st.peak = used;
}

/* Gives up the upload in progress, closing its connection. */
static void send_reset(void)
{
    if (snd.h >= 0)
        port_tcp_close(snd.h);
    snd.h = -1;
    snd.state = S_IDLE;
    snd.slot = -1;
}

/* Removes the front chunk, which went or never will (`outcome` for the card
 * copy's index; NULL when validation rejected it: the copy goes too). */
static void pop_front(const char *outcome)
{
    int slot = bps_store_at(c.store, 0);
    if (slot < 0)
        return;
    chunk_meta *m = &meta[slot];
    if (snd.slot == slot)
        send_reset();
    if (outcome)
        bps_record_index(&m->card, m->samples, m->lost, outcome);
    else
        bps_record_discard(&m->card);
    bps_store_pop(c.store);
}

/* Room for `n` more bytes: the oldest complete chunks give way (one being
 * sent too: it is the oldest). False if the chunk being encoded fills the
 * store, or (streaming) the upload has not caught up yet. */
static bool make_room(uint32_t n)
{
    while (bps_store_free(c.store) < n) {
        int slot = bps_store_at(c.store, 0);
        if (slot < 0 || bps_store_is_open(c.store, slot) || streaming)
            return false;
        chunk_meta *m = &meta[slot];
        port_log("upload: the store is full; dropped the chunk stored %lu s ago, unsent",
                 (unsigned long)((port_mono_us() - m->stored_us) / 1000000));
        st.dropped++;
        pop_front("dropped (the store was full)");
    }
    return true;
}

/* ---- Choosing and encoding chunks ---- */

/* The next chunk continues from `end` (passes before it are handled). */
static void set_prev(uint32_t end)
{
    sel.has_prev = true;
    sel.prev_old = false;
    sel.prev_end = end;
}

/* The chunk being encoded is given up; its passes count as handled up to
 * `handled` unless that is NULL (a rejected chunk: the next sound still
 * starts its full look-back before it). */
static void enc_cancel(const uint32_t *handled)
{
    if (!enc.on)
        return;
    if (enc.spool) {
        bps_spool_abort();
    } else {
        if (snd.slot == enc.slot)
            send_reset();
        bps_store_cancel(c.store);
        bps_record_abort();
    }
    if (chk.on && chk.id == enc.id && bps_validate_collecting()) {
        /* Its window will not be complete: no check (one running ends on its own). */
        bps_validate_uncollect();
        chk.on = false;
    }
    enc.on = false;
    enc.spool_wait = false;
    if (handled)
        set_prev(*handled);
}

/* Starts validating the chunk just begun (`slot` -1: spooled), triggered by
 * the pass ending at `trigger`: its window, a third of it before the
 * triggering frame, but inside the chunk (it is fed as the chunk is
 * encoded). Unchecked (it goes) if validation is off or busy, or the window
 * does not fit. */
static int check_begin(int slot, uint32_t trigger, uint64_t now)
{
    if (!bps_validate_active())
        return V_NONE;
    if (chk.on) {
        port_log("validation: the last check is still running; this chunk goes unchecked");
        return V_NONE;
    }
    uint32_t window = bps_validate_window(), margin = bps_validate_margin();
    uint32_t win = trigger - c.cap->gate.frame_len - window / 3;
    if (bps_idx_diff(win, enc.start + margin) < 0)
        win = enc.start + margin;
    if (bps_idx_diff(win + window + margin, enc.end) > 0 || !bps_validate_collect(win))
        return V_NONE;
    chk.on = true;
    chk.slot = slot;
    chk.id = enc.id;
    chk.win = win;
    chk.trigger = trigger;
    chk.since_us = now;
    return V_WAIT;
}

/* Starts the chunk of `chunk_len` samples from ring index `start` (the pass
 * ending at `trigger` opened it): on the card, or into the store. */
static void enc_begin(uint32_t start, uint32_t trigger, uint64_t now)
{
    uint32_t w = bps_ring_written(c.ring);
    uint32_t keep = c.ring->capacity - RING_GUARD - c.cfg->sample_rate * BPS_UPLOAD_SLACK_MS / 1000;
    if (bps_idx_diff(w, start) > (int32_t)keep)
        start = w - keep;
    /* A chunk must not span a timeline jump: start after the latest one. */
    int64_t mono_ts;
    bool has_next;
    uint32_t next_idx;
    for (;;) {
        if (!bps_ring_locate(c.ring, start, &mono_ts, &has_next, &next_idx)) {
            set_prev(w); /* older than any remembered timeline: skip it */
            return;
        }
        if (!has_next || bps_idx_diff(next_idx, start) >= (int32_t)c.chunk_len)
            break;
        start = next_idx;
    }
    /* The chunk's end is still to be captured: its time comes from the timeline. */
    int64_t mono_end;
    if (!bps_ring_locate(c.ring, start + c.chunk_len, &mono_end, &has_next, &next_idx))
        mono_end = mono_ts + bps_samples_to_ns(c.chunk_len, c.cfg->sample_rate);
    /* Recorded for the status. */
    bps_clock_correct(c.clk, mono_ts + *c.epoch_ns, mono_end - mono_ts);
    int64_t start_ns = bps_clock_at(c.clk, mono_ts + *c.epoch_ns);
    int64_t end_ns = bps_clock_at(c.clk, mono_end + *c.epoch_ns);
    uint8_t head[BPS_FLAC_HEADER_LEN];
    bps_flac_header(head, c.cfg->sample_rate, c.chunk_len);

    enc.start = enc.next = start;
    enc.end = start + c.chunk_len;
    enc.segs = LOAD(&c.ring->n_segs);
    enc.frame = enc.lost = 0;
    enc.id = next_id++;
    enc.spool_wait = false;
    enc.slot = -1;

    if (to_spool) {
        if (!bps_spool_begin(mono_ts + *c.epoch_ns, mono_end + *c.epoch_ns, start_ns, end_ns,
                             c.cfg->sample_rate, c.chunk_len, head, sizeof head)) {
            /* No card with room: the chunk is lost. */
            set_prev(start + c.chunk_len);
            sel.retry_us = now + 1000000;
            return;
        }
        enc.spool = true;
        enc.on = true;
        enc.spool_verdict = check_begin(-1, trigger, now);
        return;
    }

    if (!has_store()) {
        set_prev(start + c.chunk_len);
        return;
    }
    int slot = bps_store_open(c.store);
    if (slot < 0 && !streaming && bps_store_count(c.store) == BPS_STORE_ITEMS &&
        !bps_store_is_open(c.store, bps_store_at(c.store, 0))) {
        port_log("upload: the store holds as many chunks as it can; dropped the oldest, unsent");
        st.dropped++;
        pop_front("dropped (the store was full)");
        slot = bps_store_open(c.store);
    }
    if (slot < 0 || !make_room(sizeof head) || !bps_store_append(c.store, head, sizeof head)) {
        if (slot >= 0)
            bps_store_cancel(c.store);
        port_log("upload: no room in the store for a chunk; it is lost");
        st.dropped++;
        set_prev(start + c.chunk_len);
        sel.retry_us = now + 1000000;
        return;
    }
    chunk_meta *m = &meta[slot];
    memset(m, 0, sizeof *m);
    m->id = enc.id;
    m->start_ns = start_ns;
    m->end_ns = end_ns;
    if (c.beacon && bps_beacon_ready(c.beacon, (int64_t)now * 1000)) {
        char bssid[18];
        bps_beacon_bssid(c.beacon, bssid);
        snprintf(m->tsf, sizeof m->tsf, "%s %lld %lld", bssid,
                 (long long)bps_beacon_tsf_at(c.beacon, mono_ts),
                 (long long)bps_beacon_tsf_at(c.beacon, mono_end));
    }
    enc.spool = false;
    enc.slot = slot;
    enc.on = true;
    bps_record_begin(start_ns, end_ns, m->tsf[0] ? m->tsf : NULL, c.cfg->sample_rate, c.chunk_len,
                     head, sizeof head);
    m->verdict = check_begin(slot, trigger, now);
    note_peak();
}

/* Reads `n` samples from `enc.next`. Any already overwritten (the loop was
 * held up for longer than the ring holds) become silence: the chunk keeps its
 * length and every later sample its exact time (the server zero-fills gaps in
 * a client's audio the same way). False if the capture core kept overtaking
 * the read. */
static bool enc_read(int16_t *out, uint32_t n)
{
    for (int tries = 0; tries < 4; tries++) {
        int32_t gone = bps_idx_diff(bps_ring_oldest(c.ring), enc.next);
        uint32_t zeros = gone <= 0 ? 0 : (uint32_t)gone < n ? (uint32_t)gone : n;
        if (zeros == n || bps_ring_read(c.ring, enc.next + zeros, out + zeros, n - zeros)) {
            memset(out, 0, zeros * sizeof *out);
            enc.lost += zeros;
            return true;
        }
    }
    return false;
}

/* The spooled chunk is done, and validation has had its say (or no more time). */
static void spool_finish(void)
{
    if (enc.spool_verdict == V_REJECT) {
        bps_spool_abort();
    } else {
        if (enc.lost)
            (*c.overruns)++;
        bps_spool_end(enc.end - enc.start, enc.lost);
        set_prev(enc.end);
        kept++;
    }
    enc.on = false;
    enc.spool_wait = false;
}

/* Every sample of the chunk is encoded. */
static void enc_done(uint64_t now)
{
    if (enc.spool) {
        enc.done_us = now;
        enc.spool_wait = true;
        if (enc.spool_verdict != V_WAIT)
            spool_finish();
        return;
    }
    chunk_meta *m = &meta[enc.slot];
    bps_store_close(c.store);
    m->samples = enc.end - enc.start;
    m->lost = enc.lost;
    m->stored_us = now;
    bps_record_close(&m->card);
    if (enc.lost)
        (*c.overruns)++;
    enc.on = false;
    set_prev(enc.end);
    kept++;
}

/* Encodes what has been captured of the chunk (16 frames at most a turn). */
static void enc_pump(uint64_t now)
{
    if (enc.spool_wait) {
        if (enc.spool_verdict == V_WAIT && now - enc.done_us > VERDICT_WAIT_US) {
            port_log("validation: no decision in time; keeping the chunk");
            enc.spool_verdict = V_PASS;
        }
        if (enc.spool_verdict != V_WAIT)
            spool_finish();
        return;
    }
    if (!enc.spool && meta[enc.slot].dead) {
        /* Streamed, and its upload failed: the rest would go nowhere. */
        uint32_t handled = enc.next;
        enc_cancel(&handled);
        return;
    }
    for (int i = 0; i < 16; i++) {
        if (enc.next == enc.end) {
            enc_done(now);
            return;
        }
        uint32_t left = enc.end - enc.next;
        uint32_t want = left < BPS_FLAC_BLOCK ? left : BPS_FLAC_BLOCK;
        if (bps_idx_diff(bps_ring_written(c.ring), enc.next) < (int32_t)want)
            return;
        if (LOAD(&c.ring->n_segs) != enc.segs) {
            int64_t t;
            bool has_next;
            uint32_t nx;
            if (!bps_ring_locate(c.ring, enc.start, &t, &has_next, &nx) ||
                (has_next && bps_idx_diff(nx, enc.end) < 0)) {
                port_log("upload: capture timeline jumped; chunk dropped");
                uint32_t handled = enc.next;
                enc_cancel(&handled);
                return;
            }
            enc.segs = LOAD(&c.ring->n_segs);
        }
        /* Streaming: the store may be waiting for the upload to catch up. */
        if (!enc.spool && !make_room(BPS_FLAC_MAX_FRAME))
            return;
        if (!enc_read(enc.pcm, want))
            return;
        if (chk.on && chk.id == enc.id)
            bps_validate_feed(enc.next, enc.pcm, want);
        size_t n = bps_flac_frame(enc.buf, enc.pcm, want, enc.frame++, c.cfg->sample_rate);
        enc.next += want;
        if (enc.spool) {
            bps_spool_frame(enc.buf, n);
        } else {
            bps_store_append(c.store, enc.buf, (uint32_t)n);
            bps_record_frame(enc.buf, n);
            note_peak();
        }
    }
}

/* ---- Validation ---- */

/* The chunk `chk` is about, in the store (-1 if it has gone). */
static int chk_slot(void)
{
    for (uint32_t k = 0; chk.slot >= 0 && k < bps_store_count(c.store); k++)
        if (bps_store_at(c.store, k) == chk.slot && meta[chk.slot].id == chk.id)
            return chk.slot;
    return -1;
}

static void set_verdict(int v)
{
    if (chk.slot < 0) {
        if (enc.on && enc.spool && enc.id == chk.id)
            enc.spool_verdict = v;
        return;
    }
    int slot = chk_slot();
    if (slot >= 0)
        meta[slot].verdict = v;
}

static void check_poll(uint64_t now)
{
    if (!chk.on)
        return;
    bps_validate_result r;
    if (!bps_validate_take(&r)) {
        if (!bps_validate_active()) {
            /* The model went: the chunk goes unchecked. */
            bps_validate_uncollect();
            chk.on = false;
            set_verdict(V_PASS);
        }
        return;
    }
    chk.on = false;
    if (c.on_validated)
        c.on_validated(&r, now - chk.since_us);
    set_verdict(r.upload ? V_PASS : V_REJECT);
    if (r.upload)
        return;
    /* Rejected: passes inside the window are passed over. Nothing goes, so
     * the next chunk still starts its full look-back before its pass. */
    (*c.chunks_rejected)++;
    sel.has_skip = true;
    sel.skip_old = false;
    sel.skip_until = chk.win + bps_validate_window();
    sel.skip_count = LOAD(&c.cap->pass_count);
    if (enc.on && enc.id == chk.id && !enc.spool_wait)
        enc_cancel(NULL);
}

/* A pass this close to the end of a chunk, or later, may still be going: the
 * next chunk follows on. The look-back (as much after the sound as before),
 * but at most what a chunk holds after the pass that started it: a longer
 * look-back would have every sound continue into a second chunk. */
/* From the end of the frame that opened the gate back to where the sound
 * began: the run of passing frames it took (capture.h's min_run). */
static uint32_t onset_back(void)
{
    uint32_t run = LOAD(&c.cap->min_run);
    return c.cap->gate.frame_len * (run ? run : 1);
}

static uint32_t near_end(void)
{
    uint32_t before = onset_back() + c.lookback;
    uint32_t after = c.chunk_len > before ? c.chunk_len - before : 0;
    return after < c.lookback ? after : c.lookback;
}

/* Starts a chunk if the gate has opened. Chunks wait for the first clock fit. */
static void select_poll(uint64_t now)
{
    if (!*c.epoch_set || !bps_est_fit(&c.clk->est) || now < sel.retry_us)
        return;
    /* The count first: a pass counted after it ends later than the end read
     * next (capture.c stores them the other way round). */
    uint32_t count = LOAD(&c.cap->pass_count);
    if (!count)
        return;
    if (sel.pass_old) {
        /* The latest pass left the ring unhandled (bps_upload_keep_recent):
         * its end no longer compares, and its audio is gone. */
        if (count == sel.pass_seen)
            return;
        sel.pass_old = false;
    }
    uint32_t p = LOAD(&c.cap->last_pass_end);
    uint32_t w = bps_ring_written(c.ring);
    if (bps_idx_diff(w, p) > (int32_t)(c.ring->capacity / 2) && !sel.has_prev) {
        /* Gated long ago (before the clock was ready): too old to send. */
        set_prev(w);
        return;
    }
    /* Already judged: a pass inside a window the model rejected. Once that
     * window is out of the ring, ring indices no longer compare (and wrap), so
     * the pass counter tells whether the gate has passed since. */
    if (sel.has_skip && !sel.skip_old && bps_idx_diff(w, sel.skip_until) > (int32_t)c.ring->capacity)
        sel.skip_old = true;
    if (sel.has_skip && sel.skip_old && count != sel.skip_count)
        sel.has_skip = false;
    if (sel.has_skip && (sel.skip_old || bps_idx_diff(p, sel.skip_until) <= 0))
        return;
    uint32_t start = p - onset_back() - c.lookback;
    if (sel.has_prev && !sel.prev_old) {
        /* Nothing new unless it passed near the end of the last chunk or after. */
        if (bps_idx_diff(p, sel.prev_end - near_end()) <= 0)
            return;
        if (bps_idx_diff(start, sel.prev_end) < 0)
            start = sel.prev_end; /* still open: continue without a gap */
    }
    enc_begin(start, p, now);
}

void bps_upload_poll(uint64_t now)
{
    check_poll(now);
    if (enc.on)
        enc_pump(now);
    if (!enc.on)
        select_poll(now);
    if (enc.on)
        enc_pump(now);
}

/* ---- Sending ---- */

/* An HTTP chunk's size line, always 6 bytes ("%04X\r\n"; leading zeros are
 * allowed), so it can go in front of data already in place. */
static void put_chunk_size(uint8_t *out, size_t n)
{
    static const char hex[] = "0123456789ABCDEF";
    for (int i = 0; i < 4; i++)
        out[i] = (uint8_t)hex[(n >> (12 - 4 * i)) & 0xf];
    out[4] = '\r';
    out[5] = '\n';
}

/* The chunk to upload now, if any; rejected ones at the front are dropped on
 * the way. */
static int front_ready(uint64_t now, bool drop)
{
    for (;;) {
        int slot = bps_store_at(c.store, 0);
        if (slot < 0)
            return -1;
        chunk_meta *m = &meta[slot];
        bool open = bps_store_is_open(c.store, slot);
        if (!open && (m->verdict == V_REJECT || m->dead)) {
            if (!drop)
                return -1;
            pop_front(m->dead ? "aborted" : NULL);
            continue;
        }
        if (open)
            /* Streaming: as soon as validation lets it through. */
            return streaming && !m->dead && (m->verdict == V_NONE || m->verdict == V_PASS) ? slot : -1;
        if (m->verdict == V_WAIT) {
            if (now - m->stored_us < VERDICT_WAIT_US)
                return -1;
            if (drop) {
                port_log("validation: no decision in time; uploading");
                m->verdict = V_PASS;
            }
        }
        return slot;
    }
}

/* What the upload took, to tell a slow network from a slow server. */
static void upload_report(uint64_t now, const char *outcome)
{
    chunk_meta *m = &meta[snd.slot];
    uint64_t send_us = (snd.d.sent_us ? snd.d.sent_us : now) - snd.d.begun_us;
    unsigned long kb_s = send_us ? (unsigned long)(snd.d.bytes * 1000 / send_us) : 0;
    char answer[32] = "-", ratio[16], waited[48] = "";
    if (snd.d.sent_us)
        snprintf(answer, sizeof answer, "%lu ms", (unsigned long)((now - snd.d.sent_us) / 1000));
    uint32_t len = bps_store_len(c.store, snd.slot);
    c.centi(ratio, sizeof ratio, len ? (int32_t)((uint64_t)c.chunk_len * 200 / len) : 0);
    if (m->stored_us)
        snprintf(waited, sizeof waited, ", stored %lu ms before",
                 (unsigned long)((snd.d.begun_us - m->stored_us) / 1000));
    port_log("upload: %s: %lu ms of audio, %lu KB in %lu ms (%lu KB/s; FLAC %s:1), %lu ms lost; "
             "connect %lu ms, server answered in %s%s%s",
             outcome, samples_ms(c.chunk_len), (unsigned long)(len / 1024),
             (unsigned long)(send_us / 1000), kb_s, ratio, samples_ms(m->lost),
             (unsigned long)snd.d.connect_ms, answer, waited,
             m->tries > 1 ? ", after earlier tries" : "");
    diag_upload_report();
}

/* The upload failed: the chunk goes again later (or, streamed or tried too
 * often, not at all). */
static void send_failed(uint64_t now, const char *outcome)
{
    int slot = snd.slot;
    chunk_meta *m = &meta[slot];
    upload_report(now, outcome);
    send_reset();
    bool open = bps_store_is_open(c.store, slot);
    if (streaming || m->tries >= MAX_TRIES) {
        st.dropped++;
        if (open) {
            m->dead = true; /* the encoder gives it up */
        } else if (bps_store_at(c.store, 0) == slot) {
            pop_front(outcome);
        }
        return;
    }
    uint64_t wait = RETRY_US << (m->tries < 6 ? m->tries - 1 : 5);
    snd.retry_us = now + (wait < RETRY_MAX_US ? wait : RETRY_MAX_US);
}

static void send_begin(int slot, uint64_t now)
{
    chunk_meta *m = &meta[slot];
    char extra[192], path[96];
    bps_json x;
    bps_json_init(&x, extra, sizeof extra);
    bps_json_raw(&x, BPS_HEADER_START_NS ": ");
    bps_json_i64(&x, NULL, m->start_ns);
    bps_json_raw(&x, "\r\n" BPS_HEADER_END_NS ": ");
    bps_json_i64(&x, NULL, m->end_ns);
    bps_json_raw(&x, "\r\n");
    if (m->tsf[0]) {
        bps_json_raw(&x, BPS_HEADER_TSF ": ");
        bps_json_raw(&x, m->tsf);
        bps_json_raw(&x, "\r\n");
    }
    c.path_for(path, sizeof path, "audio");
    /* Sent in HTTP chunks: a chunk being streamed has no size yet. */
    size_t head = bps_http_head((char *)snd.buf, sizeof snd.buf, "POST", c.cfg->server_host,
                                c.cfg->server_port, path, "audio/flac", BPS_HTTP_CHUNKED, extra);
    if (!head)
        return;
    m->tries++;
    uint64_t t0 = port_mono_us();
    snd.h = c.connect_server("an upload");
    if (snd.h < 0) {
        port_log("upload: cannot connect to %s:%u", c.cfg->server_host,
                 (unsigned)c.cfg->server_port);
        uint64_t wait = RETRY_US << (m->tries < 6 ? m->tries - 1 : 5);
        snd.retry_us = now + (wait < RETRY_MAX_US ? wait : RETRY_MAX_US);
        if (m->tries >= MAX_TRIES && !streaming) {
            st.dropped++;
            snd.slot = slot;
            pop_front("not sent (cannot connect)");
        }
        snd.slot = -1;
        return;
    }
    memset(&snd.d, 0, sizeof snd.d);
    snd.d.begun_us = now;
    snd.d.connect_ms = (uint32_t)((port_mono_us() - t0) / 1000);
    snd.slot = slot;
    snd.id = m->id;
    snd.off = 0;
    snd.ended = false;
    snd.len = head;
    snd.pos = 0;
    snd.rx_len = 0;
    snd.progress_us = now;
    snd.state = S_SEND;
    diag_upload_begin();
}

/* Puts the next piece of the chunk in the send buffer: false if there is
 * none yet (streaming, ahead of the encoder). */
static bool send_refill(void)
{
    bool open = bps_store_is_open(c.store, snd.slot);
    uint32_t avail = bps_store_len(c.store, snd.slot) - snd.off;
    snd.pos = 0;
    if (avail == 0) {
        if (open)
            return false;
        memcpy(snd.buf, "0\r\n\r\n", 5); /* the body ends */
        snd.len = 5;
        snd.ended = true;
        return true;
    }
    uint32_t n = avail < SEND_PIECE ? avail : SEND_PIECE;
    n = bps_store_read(c.store, snd.slot, snd.off, snd.buf + 6, n);
    put_chunk_size(snd.buf, n);
    memcpy(snd.buf + 6 + n, "\r\n", 2);
    snd.len = 6 + n + 2;
    snd.off += n;
    if (streaming) /* nothing goes again: its space is free for the encoder */
        bps_store_release(c.store, snd.off);
    return true;
}

static void send_pump(uint64_t now)
{
    diag_upload_pump();
    for (int i = 0; i < 16; i++) {
        if (snd.pos == snd.len) {
            if (snd.ended) {
                snd.d.sent_us = now;
                snd.state = S_RESP;
                snd.deadline_us = now + ANSWER_TIMEOUT_US;
                return;
            }
            if (!send_refill())
                break;
        }
        uint64_t t0 = port_mono_us();
        int n = port_tcp_write(snd.h, snd.buf + snd.pos, snd.len - snd.pos);
        diag_upload_write(port_mono_us() - t0);
        if (n < 0) {
            send_failed(now, "aborted (connection lost)");
            return;
        }
        if (n == 0)
            break;
        snd.pos += (size_t)n;
        snd.d.bytes += (uint64_t)n;
        diag_upload_progress(now - snd.progress_us);
        snd.progress_us = now;
    }
    /* Streaming waits for the encoder too: not a stall. */
    bool waiting = streaming && snd.pos == snd.len && !snd.ended;
    if (waiting)
        snd.progress_us = now;
    if (now - snd.progress_us > UPLOAD_STALL_US)
        send_failed(now, "aborted (stalled)");
}

static void resp_poll(uint64_t now)
{
    int r = c.read_response(snd.h, snd.rx, sizeof snd.rx, &snd.rx_len, &snd.resp);
    if (r == 0 && now < snd.deadline_us)
        return;
    char outcome[48];
    if (r > 0 && snd.resp.status >= 200 && snd.resp.status < 300) {
        (*c.chunks_sent)++;
        upload_report(now, "sent");
        pop_front("sent");
        return;
    }
    if (r > 0 && snd.resp.status == 404) {
        port_log("server forgot this client; re-registering");
        *c.need_register = true;
        snprintf(outcome, sizeof outcome, "server forgot this client");
    } else if (r > 0) {
        snprintf(outcome, sizeof outcome, "server returned %d", snd.resp.status);
    } else {
        snprintf(outcome, sizeof outcome, "no answer");
    }
    /* A request the server will never take (other than for a client it
     * forgot, or for being busy) is not tried again. */
    if (r > 0 && snd.resp.status >= 400 && snd.resp.status < 500 && snd.resp.status != 404 &&
        snd.resp.status != 408 && snd.resp.status != 429)
        meta[snd.slot].tries = MAX_TRIES;
    send_failed(now, outcome);
}

void bps_upload_send_poll(uint64_t now)
{
    if (!has_store())
        return;
    if (snd.state == S_SEND) {
        send_pump(now);
        return;
    }
    if (snd.state == S_RESP) {
        resp_poll(now);
        return;
    }
    if (now < snd.retry_us || hold)
        return;
    int slot = front_ready(now, true);
    if (slot >= 0)
        send_begin(slot, now);
}

/* ---- The client's view ---- */

void bps_upload_keep_recent(void)
{
    if (!c.ring)
        return;
    uint32_t w = bps_ring_written(c.ring);
    int32_t cap = (int32_t)c.ring->capacity;
    /* The count first, as in select_poll(). */
    uint32_t count = LOAD(&c.cap->pass_count);
    uint32_t p = LOAD(&c.cap->last_pass_end);
    if (count && bps_idx_diff(w, p) > cap) {
        sel.pass_old = true;
        sel.pass_seen = count;
    }
    if (sel.has_prev && !sel.prev_old && bps_idx_diff(w, sel.prev_end) > cap)
        sel.prev_old = true;
    if (sel.has_skip && !sel.skip_old && bps_idx_diff(w, sel.skip_until) > cap)
        sel.skip_old = true;
    if (enc.on && !enc.spool_wait && bps_idx_diff(w, enc.next) > cap) {
        /* Its audio is gone: as if it had been sent, the passes it held are
         * handled. A check still running ends on its own. */
        port_log("upload: the chunk left the ring before it was encoded; dropped");
        uint32_t handled = enc.end;
        enc_cancel(&handled);
    }
}

void bps_upload_init(const bps_upload_ctx *ctx)
{
    c = *ctx;
    /* As at boot (once, but tests start each afresh). */
    memset(&sel, 0, sizeof sel);
    memset(&enc, 0, sizeof enc);
    memset(&chk, 0, sizeof chk);
    memset(&snd, 0, sizeof snd);
    hold = false;
    memset(&st, 0, sizeof st);
    enc.slot = snd.slot = -1;
    snd.h = -1;
    if (!meta)
        meta = port_big_alloc(BPS_STORE_ITEMS * sizeof *meta);
    streaming = !c.store || c.store->cap < bps_upload_store_least(c.chunk_len);
}

uint32_t bps_upload_store_least(uint32_t chunk_len)
{
    /* A chunk at its largest: every frame verbatim. */
    uint32_t frames = (chunk_len + BPS_FLAC_BLOCK - 1) / BPS_FLAC_BLOCK;
    uint64_t largest = BPS_FLAC_HEADER_LEN + (uint64_t)frames * BPS_FLAC_MAX_FRAME;
    largest += BPS_FLAC_MAX_FRAME;
    return largest > UINT32_MAX ? UINT32_MAX : (uint32_t)largest;
}

bool bps_upload_idle(void)
{
    return !enc.on && !chk.on && snd.state == S_IDLE && (!c.store || !bps_store_count(c.store));
}

bool bps_upload_encoding(void) { return enc.on; }

void bps_upload_set_lookback(uint32_t samples) { c.lookback = samples; }

uint32_t bps_upload_kept(void) { return kept; }

bool bps_upload_sending(void) { return snd.state != S_IDLE; }

bool bps_upload_ready(uint64_t now)
{
    return has_store() && !hold && snd.state == S_IDLE && now >= snd.retry_us &&
           front_ready(now, false) >= 0;
}

void bps_upload_hold(bool on) { hold = on; }

bool bps_upload_oldest_start(int64_t *start_ns)
{
    if (!has_store() || !bps_store_count(c.store))
        return false;
    *start_ns = meta[bps_store_at(c.store, 0)].start_ns;
    return true;
}

uint32_t bps_upload_end(void) { return enc.end; }

void bps_upload_set_spool(bool on) { to_spool = on; }

void bps_upload_drop(void)
{
    if (snd.state == S_IDLE)
        return;
    send_failed(port_mono_us(), "dropped (reconnecting)");
    snd.retry_us = 0; /* not the network's fault: go again once connected */
}

void bps_upload_status_get(bps_upload_status *out)
{
    memset(out, 0, sizeof *out);
    out->streaming = streaming;
    out->dropped = st.dropped;
    out->peak_bytes = st.peak;
    if (!has_store())
        return;
    out->cap = c.store->cap;
    out->queued = bps_store_count(c.store);
    out->bytes = c.store->cap - bps_store_free(c.store);
    for (uint32_t k = 0; k < out->queued; k++) {
        int slot = bps_store_at(c.store, k);
        if (!bps_store_is_open(c.store, slot)) {
            out->oldest_us = meta[slot].stored_us;
            break;
        }
    }
}
