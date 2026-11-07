#include "upload.h"

#include <stdio.h>
#include <string.h>

#include "flac.h"
#include "port.h"
#include "record.h"
#include "spool.h"

#define LOAD(p) __atomic_load_n(p, __ATOMIC_ACQUIRE)

#define UPLOAD_STALL_US 5000000
/* For the server's answer, as for the client's other requests. */
#define ANSWER_TIMEOUT_US 10000000

/* UP_CHECK: a triggered chunk waits for validation (validate.h); UP_RETRY:
 * validation let it through, but the server could not be reached (yet). */
enum { UP_IDLE, UP_CHECK, UP_RETRY, UP_SEND, UP_RESP };

/* The client's state and helpers, from bps_upload_init(). */
static bps_upload_ctx c;

/* Chunks from the next one on are saved on the card (batch mode). */
static bool to_spool;

static void check_poll(uint64_t now);

static struct {
    int state, h;
    bool spool; /* this chunk goes to the card, not the server */
    uint32_t start, end, next, segs;
    /* Passes up to here were handled (a chunk covered them, or was given up).
     * prev_old: that end has left the ring, so ring indices no longer compare
     * with it (they would wrap), and every pass since is new. */
    bool has_prev, prev_old;
    uint32_t prev_end;
    /* The gate's latest pass has left the ring: the next is new once the
     * pass counter has moved on from pass_seen. */
    bool pass_old;
    uint32_t pass_seen;
    uint32_t frame; /* FLAC frames sent */
    int16_t pcm[BPS_FLAC_BLOCK];
    /* The request head, or a FLAC frame as one HTTP chunk: size line, frame,
     * CRLF, and after the last frame the closing empty chunk. */
    uint8_t buf[6 + BPS_FLAC_MAX_FRAME + 2 + 5];
    size_t len, off;
    char rx[256];
    size_t rx_len;
    bps_http_response resp;
    uint64_t progress_us, deadline_us, retry_us;
    uint32_t lost; /* samples of this chunk sent as silence */
    /* UP_CHECK: the chunk's start, the model's window, whether its check runs. */
    uint32_t chk_start, chk_win, chk_trigger;
    bool chk_running;
    /* Gate passes up to here were in a window validation rejected. */
    bool has_skip, skip_old;
    uint32_t skip_until, skip_count;
    uint64_t chk_since_us;
    /* Logged when the upload ends. */
    struct {
        uint64_t begun_us, sent_us; /* sent_us: when the last byte was handed over */
        uint32_t connect_ms;
        uint64_t bytes; /* written to the connection */
    } d;
} up = {.h = -1};

static unsigned long samples_ms(uint64_t n)
{
    return (unsigned long)(n * 1000 / c.cfg->sample_rate);
}

#ifdef BPS_NET_DEBUG
/* Upload writes that make no progress for longer count as a stall. */
#define UPLOAD_STALL_LOG_US 200000

/* More about the current upload, logged when it ends. */
static struct {
    uint32_t behind_start, behind_max; /* samples between the next to send and the newest */
    uint64_t stall_max_us, write_max_us;
    uint32_t stalls;
    uint64_t encode_us;     /* FLAC encoding, in all */
    uint32_t encode_max_us; /* the slowest frame */
    bool tcp_seen;
    uint32_t retransmits, nrtx, rto_max_ms, cwnd_min, wnd_min, wnd_zero;
} upd;

static void diag_upload_begin(void)
{
    memset(&upd, 0, sizeof upd);
    upd.behind_start = upd.behind_max = (uint32_t)bps_idx_diff(bps_ring_written(c.ring), up.start);
    upd.cwnd_min = upd.wnd_min = UINT32_MAX;
}

/* Notes how far behind the upload is, and the connection's TCP state (where
 * the board can tell). */
static void diag_upload_pump(void)
{
    uint32_t behind = (uint32_t)bps_idx_diff(bps_ring_written(c.ring), up.next);
    if (behind > upd.behind_max)
        upd.behind_max = behind;
    port_tcp_stats st;
    if (!port_tcp_get_stats(up.h, &st))
        return;
    upd.tcp_seen = true;
    if (st.retransmits > upd.nrtx) /* per segment: it restarts at 0 once acknowledged */
        upd.retransmits += st.retransmits - upd.nrtx;
    upd.nrtx = st.retransmits;
    if (st.rto_ms > upd.rto_max_ms)
        upd.rto_max_ms = st.rto_ms;
    if (st.cwnd < upd.cwnd_min)
        upd.cwnd_min = st.cwnd;
    if (st.peer_wnd < upd.wnd_min)
        upd.wnd_min = st.peer_wnd;
    if (st.peer_wnd == 0)
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

static void diag_upload_encode(uint32_t us)
{
    upd.encode_us += us;
    if (us > upd.encode_max_us)
        upd.encode_max_us = us;
}

static void diag_upload_report(void)
{
    port_log("upload:   behind %lu ms at start, %lu at most; %lu stalls over %lu ms, longest %lu "
             "ms; slowest write call %lu ms; encoding %lu ms (slowest frame %lu us)",
             samples_ms(upd.behind_start), samples_ms(upd.behind_max), (unsigned long)upd.stalls,
             (unsigned long)(UPLOAD_STALL_LOG_US / 1000), (unsigned long)(upd.stall_max_us / 1000),
             (unsigned long)(upd.write_max_us / 1000), (unsigned long)(upd.encode_us / 1000),
             (unsigned long)upd.encode_max_us);
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
static inline void diag_upload_encode(uint32_t us) { (void)us; }
static inline void diag_upload_report(void) {}
#endif

/* What the upload took, to tell a slow network from a slow server or loop. */
static void upload_report(uint64_t now, const char *outcome)
{
    uint32_t sent = up.next - up.start;
    bps_record_end(sent, up.lost, outcome);
    uint64_t send_us = (up.d.sent_us ? up.d.sent_us : now) - up.d.begun_us;
    unsigned long kb_s = send_us ? (unsigned long)(up.d.bytes * 1000 / send_us) : 0;
    char answer[32] = "-", ratio[16];
    if (up.d.sent_us)
        snprintf(answer, sizeof answer, "%lu ms", (unsigned long)((now - up.d.sent_us) / 1000));
    c.centi(ratio, sizeof ratio, up.d.bytes ? (int32_t)((uint64_t)sent * 200 / up.d.bytes) : 0);
    port_log("upload: %s: %lu of %lu ms of audio in %lu ms (%lu KB/s sent, real time is %lu; FLAC "
             "%s:1), %lu ms lost; connect %lu ms, server answered in %s",
             outcome, samples_ms(sent), samples_ms(c.chunk_len), (unsigned long)(send_us / 1000),
             kb_s, (unsigned long)(c.cfg->sample_rate * 2 / 1000), ratio, samples_ms(up.lost),
             (unsigned long)up.d.connect_ms, answer);
    diag_upload_report();
}

/* The next chunk continues from `end` (passes before it are handled). */
static void set_prev(uint32_t end)
{
    up.has_prev = true;
    up.prev_old = false;
    up.prev_end = end;
}

static void upload_finish(uint32_t end)
{
    port_tcp_close(up.h);
    up.h = -1;
    up.state = UP_IDLE;
    set_prev(end);
}

static void upload_abort(const char *why)
{
    char outcome[80];
    uint64_t now = port_mono_us();
    snprintf(outcome, sizeof outcome, "aborted (%s, nothing written for %lu ms)", why,
             (unsigned long)((now - up.progress_us) / 1000));
    upload_report(now, outcome);
    upload_finish(up.next);
}

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

/* Starts saving the chunk from ring index `start` on the card instead. */
static void spool_begin(uint32_t start, int64_t mono_ts, int64_t mono_end, int64_t start_ns,
                        int64_t end_ns, uint64_t now)
{
    uint8_t head[BPS_FLAC_HEADER_LEN];
    bps_flac_header(head, c.cfg->sample_rate, c.chunk_len);
    if (!bps_spool_begin(mono_ts + *c.epoch_ns, mono_end + *c.epoch_ns, start_ns, end_ns,
                         c.cfg->sample_rate, c.chunk_len, head, sizeof head)) {
        /* No card with room: the chunk is lost. */
        set_prev(start + c.chunk_len);
        up.retry_us = now + 1000000;
        return;
    }
    up.spool = true;
    up.start = start;
    up.end = start + c.chunk_len;
    up.next = start;
    up.segs = LOAD(&c.ring->n_segs);
    up.lost = 0;
    up.frame = 0;
    up.off = up.len = 0;
    up.state = UP_SEND;
}

/* Starts uploading `chunk_len` samples from ring index `start` (or saving
 * them). False if the server could not be reached: the chunk is still to go. */
static bool upload_begin(uint32_t start, uint64_t now)
{
    uint32_t w = bps_ring_written(c.ring);
    uint32_t keep = c.ring->capacity - 4096 - c.cfg->sample_rate * BPS_UPLOAD_SLACK_MS / 1000;
    if (bps_idx_diff(w, start) > (int32_t)keep)
        start = w - keep;
    /* A chunk must not span a timeline jump: start after the latest one. */
    int64_t mono_ts;
    bool has_next;
    uint32_t next_idx;
    for (;;) {
        if (!bps_ring_locate(c.ring, start, &mono_ts, &has_next, &next_idx)) {
            set_prev(w); /* older than any remembered timeline: skip it */
            return true;
        }
        if (!has_next || bps_idx_diff(next_idx, start) >= (int32_t)c.chunk_len)
            break;
        start = next_idx;
    }
    /* The chunk's end is still to be captured: its time comes from the timeline. */
    int64_t mono_end;
    if (!bps_ring_locate(c.ring, start + c.chunk_len, &mono_end, &has_next, &next_idx))
        mono_end = mono_ts + bps_samples_to_ns(c.chunk_len, c.cfg->sample_rate);
    /* Recorded for the status; the times sent are corrected at each end. */
    bps_clock_correct(c.clk, mono_ts + *c.epoch_ns, mono_end - mono_ts);
    int64_t start_ns = bps_clock_at(c.clk, mono_ts + *c.epoch_ns);
    int64_t end_ns = bps_clock_at(c.clk, mono_end + *c.epoch_ns);
    if (to_spool) {
        spool_begin(start, mono_ts, mono_end, start_ns, end_ns, now);
        return true;
    }

    char extra[192], path[96];
    bps_json x;
    bps_json_init(&x, extra, sizeof extra);
    bps_json_raw(&x, BPS_HEADER_START_NS ": ");
    bps_json_i64(&x, NULL, start_ns);
    bps_json_raw(&x, "\r\n" BPS_HEADER_END_NS ": ");
    bps_json_i64(&x, NULL, end_ns);
    bps_json_raw(&x, "\r\n");
    char tsf[80] = "";
    if (bps_beacon_ready(c.beacon, (int64_t)now * 1000)) {
        char bssid[18];
        bps_beacon_bssid(c.beacon, bssid);
        snprintf(tsf, sizeof tsf, "%s %lld %lld", bssid,
                 (long long)bps_beacon_tsf_at(c.beacon, mono_ts),
                 (long long)bps_beacon_tsf_at(c.beacon, mono_end));
        bps_json_raw(&x, BPS_HEADER_TSF ": ");
        bps_json_raw(&x, tsf);
        bps_json_raw(&x, "\r\n");
    }
    c.path_for(path, sizeof path, "audio");
    /* FLAC's size is not known ahead: send it in chunks, one per frame. */
    size_t head = bps_http_head((char *)up.buf, sizeof up.buf - (6 + BPS_FLAC_HEADER_LEN + 2),
                                "POST", c.cfg->server_host, c.cfg->server_port, path, "audio/flac",
                                BPS_HTTP_CHUNKED, extra);
    if (!head)
        return true;
    put_chunk_size(up.buf + head, BPS_FLAC_HEADER_LEN);
    bps_flac_header(up.buf + head + 6, c.cfg->sample_rate, c.chunk_len);
    memcpy(up.buf + head + 6 + BPS_FLAC_HEADER_LEN, "\r\n", 2);
    up.len = head + 6 + BPS_FLAC_HEADER_LEN + 2;
    up.off = 0;
    up.frame = 0;
    uint64_t t0 = port_mono_us();
    up.h = c.connect_server("an upload");
    if (up.h < 0) {
        port_log("upload: cannot connect to %s:%u", c.cfg->server_host,
                 (unsigned)c.cfg->server_port);
        up.retry_us = now + 1000000;
        return false;
    }
    memset(&up.d, 0, sizeof up.d);
    up.d.begun_us = now;
    up.d.connect_ms = (uint32_t)((port_mono_us() - t0) / 1000);
    up.start = start;
    up.end = start + c.chunk_len;
    up.next = start;
    up.segs = LOAD(&c.ring->n_segs);
    up.lost = 0;
    up.rx_len = 0;
    up.progress_us = now;
    up.spool = false;
    up.state = UP_SEND;
    bps_record_begin(start_ns, end_ns, tsf[0] ? tsf : NULL, c.cfg->sample_rate, c.chunk_len,
                     up.buf + head + 6, BPS_FLAC_HEADER_LEN);
    diag_upload_begin();
    return true;
}

/* Uploads the chunk from `start`, which validation let through (or could not
 * judge): if the server cannot be reached, that stands, and only connecting
 * is tried again (UP_RETRY), not the check. */
static void upload_checked(uint32_t start, uint64_t now)
{
    up.state = UP_IDLE;
    if (!upload_begin(start, now)) {
        up.state = UP_RETRY;
        up.chk_start = start;
    }
}

/* Reads `n` samples from `up.next`. Any already overwritten (the network
 * stalled for longer than the ring holds) are sent as silence: the chunk keeps
 * its length and every later sample its exact time (the server zero-fills
 * gaps in a client's audio the same way). False if the capture core kept
 * overtaking the read. */
static bool upload_read(int16_t *out, uint32_t n)
{
    for (int tries = 0; tries < 4; tries++) {
        int32_t gone = bps_idx_diff(bps_ring_oldest(c.ring), up.next);
        uint32_t zeros = gone <= 0 ? 0 : (uint32_t)gone < n ? (uint32_t)gone : n;
        if (zeros == n || bps_ring_read(c.ring, up.next + zeros, out + zeros, n - zeros)) {
            memset(out, 0, zeros * sizeof *out);
            up.lost += zeros;
            return true;
        }
    }
    return false;
}

/* Puts the next FLAC frame in the send buffer: 1 if it did, 0 if its audio
 * is not captured yet, -1 if the upload was aborted. */
static int upload_refill(void)
{
    uint32_t left = up.end - up.next;
    uint32_t want = left < BPS_FLAC_BLOCK ? left : BPS_FLAC_BLOCK;
    if (bps_idx_diff(bps_ring_written(c.ring), up.next) < (int32_t)want)
        return 0;
    if (LOAD(&c.ring->n_segs) != up.segs) {
        int64_t t;
        bool has_next;
        uint32_t nx;
        if (!bps_ring_locate(c.ring, up.start, &t, &has_next, &nx) ||
            (has_next && bps_idx_diff(nx, up.end) < 0)) {
            if (up.spool) {
                bps_spool_abort();
                up.state = UP_IDLE;
                set_prev(up.next);
            } else {
                upload_abort("capture timeline jumped");
            }
            return -1;
        }
        up.segs = LOAD(&c.ring->n_segs);
    }
    if (!upload_read(up.pcm, want))
        return 0;
    up.next += want;
    up.off = 0;
    uint64_t t0 = port_mono_us();
    size_t n = bps_flac_frame(up.buf + 6, up.pcm, want, up.frame++, c.cfg->sample_rate);
    diag_upload_encode((uint32_t)(port_mono_us() - t0));
    if (up.spool) {
        bps_spool_frame(up.buf + 6, n);
        up.len = 0;
        return 1;
    }
    bps_record_frame(up.buf + 6, n);
    put_chunk_size(up.buf, n);
    memcpy(up.buf + 6 + n, "\r\n", 2);
    up.len = 6 + n + 2;
    if (up.next == up.end) {
        memcpy(up.buf + up.len, "0\r\n\r\n", 5); /* the body ends */
        up.len += 5;
    }
    return 1;
}

static void upload_pump(uint64_t now)
{
    diag_upload_pump();
    for (int i = 0; i < 16; i++) {
        if (up.off == up.len) {
            if (up.next == up.end) {
                if (up.lost)
                    (*c.overruns)++;
                up.d.sent_us = now;
                up.state = UP_RESP;
                up.deadline_us = now + ANSWER_TIMEOUT_US;
                return;
            }
            int r = upload_refill();
            if (r < 0)
                return;
            if (r == 0)
                break;
        }
        uint64_t t0 = port_mono_us();
        int n = port_tcp_write(up.h, up.buf + up.off, up.len - up.off);
        diag_upload_write(port_mono_us() - t0);
        if (n < 0) {
            upload_abort("connection lost");
            return;
        }
        if (n == 0)
            break;
        up.off += (size_t)n;
        up.d.bytes += (uint64_t)n;
        diag_upload_progress(now - up.progress_us);
        up.progress_us = now;
    }
    if (now - up.progress_us > UPLOAD_STALL_US)
        upload_abort("stalled");
}

/* Saves what has been captured of the chunk; once complete, its .json too. */
static void spool_pump(void)
{
    for (int i = 0; i < 16 && up.next != up.end; i++)
        if (upload_refill() <= 0)
            return;
    if (up.next != up.end)
        return;
    if (up.lost)
        (*c.overruns)++;
    bps_spool_end(up.end - up.start, up.lost);
    up.state = UP_IDLE;
    set_prev(up.end);
}

/* A pass this close to the end of a chunk, or later, may still be going: the
 * next chunk follows on. The look-back (as much after the sound as before),
 * but at most what a chunk holds after the pass that started it: a longer
 * look-back would have every sound continue into a second chunk. */
static uint32_t near_end(void)
{
    uint32_t before = c.cap->gate.frame_len + c.lookback;
    uint32_t after = c.chunk_len > before ? c.chunk_len - before : 0;
    return after < c.lookback ? after : c.lookback;
}

void bps_upload_poll(uint64_t now)
{
    if (up.state == UP_SEND && up.spool) {
        spool_pump();
        return;
    }
    if (up.state == UP_SEND) {
        upload_pump(now);
        return;
    }
    if (up.state == UP_CHECK) {
        check_poll(now);
        return;
    }
    if (up.state == UP_RETRY) {
        if (now >= up.retry_us)
            upload_checked(up.chk_start, now);
        return;
    }
    if (up.state == UP_RESP) {
        int r = c.read_response(up.h, up.rx, sizeof up.rx, &up.rx_len, &up.resp);
        if (r == 0 && now < up.deadline_us)
            return;
        upload_finish(up.end);
        char outcome[40];
        if (r > 0 && up.resp.status >= 200 && up.resp.status < 300) {
            (*c.chunks_sent)++;
            snprintf(outcome, sizeof outcome, "sent");
        } else if (r > 0 && up.resp.status == 404) {
            port_log("server forgot this client; re-registering");
            *c.need_register = true;
            snprintf(outcome, sizeof outcome, "server forgot this client");
        } else if (r > 0) {
            snprintf(outcome, sizeof outcome, "server returned %d", up.resp.status);
        } else {
            snprintf(outcome, sizeof outcome, "no answer");
        }
        upload_report(now, outcome);
        return;
    }

    /* Idle: has the gate opened? Uploads wait for the first clock fit. */
    if (!*c.epoch_set || !bps_est_fit(&c.clk->est) || now < up.retry_us)
        return;
    /* The count first: a pass counted after it ends later than the end read
     * next (capture.c stores them the other way round). */
    uint32_t count = LOAD(&c.cap->pass_count);
    if (!count)
        return;
    if (up.pass_old) {
        /* The latest pass left the ring unhandled (bps_upload_keep_recent):
         * its end no longer compares, and its audio is gone. */
        if (count == up.pass_seen)
            return;
        up.pass_old = false;
    }
    uint32_t p = LOAD(&c.cap->last_pass_end);
    uint32_t w = bps_ring_written(c.ring);
    if (bps_idx_diff(w, p) > (int32_t)(c.ring->capacity / 2) && !up.has_prev) {
        /* Gated long ago (before the clock was ready): too old to send. */
        set_prev(w);
        return;
    }
    /* Already judged: a pass inside a window the model rejected. Once that
     * window is out of the ring, ring indices no longer compare (and wrap), so
     * the pass counter tells whether the gate has passed since. */
    if (up.has_skip && !up.skip_old && bps_idx_diff(w, up.skip_until) > (int32_t)c.ring->capacity)
        up.skip_old = true;
    if (up.has_skip && up.skip_old && count != up.skip_count)
        up.has_skip = false;
    if (up.has_skip && (up.skip_old || bps_idx_diff(p, up.skip_until) <= 0))
        return;
    uint32_t start = p - c.cap->gate.frame_len - c.lookback;
    if (up.has_prev && !up.prev_old) {
        /* Nothing new unless it passed near the end of the last chunk or after. */
        if (bps_idx_diff(p, up.prev_end - near_end()) <= 0)
            return;
        if (bps_idx_diff(start, up.prev_end) < 0)
            start = up.prev_end; /* still open: continue without a gap */
    }
    if (bps_validate_active()) {
        /* The model's window: a third of it before the triggering frame. */
        uint32_t frame_start = p - c.cap->gate.frame_len;
        up.chk_win = frame_start - bps_validate_window() / 3;
        up.chk_start = start;
        up.chk_trigger = p;
        up.chk_running = false;
        up.chk_since_us = now;
        up.state = UP_CHECK;
        return;
    }
    upload_begin(start, now);
}

/* A triggered chunk waits for validation: for the rest of the model's window
 * to be captured, then for the model. Fails open: anything that keeps the
 * check from finishing uploads the chunk. */
static void check_poll(uint64_t now)
{
    if (!bps_validate_active()) {
        upload_checked(up.chk_start, now);
        return;
    }
    uint32_t w = bps_ring_written(c.ring);
    /* Waiting longer would let the chunk's start go out of the ring: as far
     * as upload_begin() reaches back, less a second for the upload to start. */
    uint32_t keep = c.ring->capacity - 4096 -
                    c.cfg->sample_rate * (BPS_UPLOAD_SLACK_MS + 1000) / 1000;
    if (bps_idx_diff(w, up.chk_start) > (int32_t)keep) {
        uint32_t end = up.chk_win + bps_validate_window() + bps_validate_margin();
        port_log("validation: no decision in time; uploading (waited %lu ms, %s, window end %ld ms ago, "
                 "trigger %ld ms ago, start %ld ms ago)",
                 (unsigned long)((now - up.chk_since_us) / 1000),
                 up.chk_running ? "check running" : "check not started",
                 (long)((int64_t)bps_idx_diff(w, end) * 1000 / c.cfg->sample_rate),
                 (long)((int64_t)bps_idx_diff(w, up.chk_trigger) * 1000 / c.cfg->sample_rate),
                 (long)((int64_t)bps_idx_diff(w, up.chk_start) * 1000 / c.cfg->sample_rate));
        upload_checked(up.chk_start, now);
        return;
    }
    if (!up.chk_running) {
        uint32_t end = up.chk_win + bps_validate_window() + bps_validate_margin();
        if (bps_idx_diff(w, end) < 0 || !bps_validate_start(up.chk_win))
            return;
        up.chk_running = true;
        return;
    }
    bps_validate_result r;
    if (!bps_validate_take(&r))
        return;
    up.state = UP_IDLE;
    if (c.on_validated)
        c.on_validated(&r, now - up.chk_since_us);
    if (r.upload) {
        upload_checked(up.chk_start, now);
        return;
    }
    /* Rejected: passes inside the window are passed over. Nothing was sent,
     * so the next chunk still starts its full look-back before its pass. */
    (*c.chunks_rejected)++;
    up.has_skip = true;
    up.skip_old = false;
    up.skip_until = up.chk_win + bps_validate_window();
    up.skip_count = LOAD(&c.cap->pass_count);
}

void bps_upload_keep_recent(void)
{
    if (!c.ring)
        return;
    uint32_t w = bps_ring_written(c.ring);
    int32_t cap = (int32_t)c.ring->capacity;
    /* The count first, as in bps_upload_poll(). */
    uint32_t count = LOAD(&c.cap->pass_count);
    uint32_t p = LOAD(&c.cap->last_pass_end);
    if (count && bps_idx_diff(w, p) > cap) {
        up.pass_old = true;
        up.pass_seen = count;
    }
    if (up.has_prev && !up.prev_old && bps_idx_diff(w, up.prev_end) > cap)
        up.prev_old = true;
    if (up.has_skip && !up.skip_old && bps_idx_diff(w, up.skip_until) > cap)
        up.skip_old = true;
    if ((up.state == UP_CHECK || up.state == UP_RETRY) && bps_idx_diff(w, up.chk_start) > cap) {
        /* Its audio is gone: as if it had been sent, the passes it held are
         * handled. A check still running ends on its own, and the next one
         * waits for it (bps_validate_start). */
        port_log("%s: the chunk left the ring before it could go; dropped",
                 up.state == UP_CHECK ? "validation" : "upload");
        up.state = UP_IDLE;
        set_prev(up.chk_start + c.chunk_len);
    }
}

void bps_upload_init(const bps_upload_ctx *ctx)
{
    c = *ctx;
    memset(&up, 0, sizeof up); /* as at boot (once, but tests start each afresh) */
    up.h = -1;
}

bool bps_upload_idle(void) { return up.state == UP_IDLE; }

bool bps_upload_sending(void) { return up.state == UP_SEND || up.state == UP_RESP; }

bool bps_upload_offline(void)
{
    if (up.state == UP_SEND || up.state == UP_RESP)
        return up.spool;
    return to_spool;
}

uint32_t bps_upload_end(void) { return up.end; }

void bps_upload_set_spool(bool on) { to_spool = on; }

void bps_upload_drop(void)
{
    /* Saving on the card goes on without the network, and so does a check,
     * or a chunk waiting to connect. */
    if (bps_upload_sending() && !up.spool) {
        bps_record_end(up.next - up.start, up.lost, "dropped (reconnecting)");
        port_tcp_close(up.h);
        up.h = -1;
        up.state = UP_IDLE;
        set_prev(up.next);
    }
}
