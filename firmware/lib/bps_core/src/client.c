#include "client.h"

#include <stdio.h>
#include <string.h>

#include "capture.h"
#include "port.h"
#include "proto.h"
#include "ring.h"
#include "timesync.h"

#define LOAD(p) __atomic_load_n(p, __ATOMIC_ACQUIRE)

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "uploads send samples in memory order, which must be little-endian"
#endif

#define TIMESYNC_INTERVAL_US 500000
#define TIMESYNC_TIMEOUT_US 400000
/* Replies on a LAN take a few ms; waiting for them right after sending keeps
 * the rest of the loop out of the measured round trip. */
#define TIMESYNC_SPIN_US 20000
#define TXN_TIMEOUT_US 10000000
#define UPLOAD_STALL_US 5000000
#define WIFI_RETRY_US 20000000
#define STATUS_LOG_US 60000000
/* The server's default `max_clock_offset_us`, for the LED only. */
#define SYNCED_ERROR_NS 2000000
/* Kept free between the oldest sample uploaded and the ring's write position. */
#define RING_SLACK_MS 400
/* Heap left for the network stack and Wi-Fi driver after the ring buffer. */
#define HEAP_RESERVE (32 * 1024)

enum { ST_WIFI, ST_REGISTER, ST_RUN };
enum { TXN_IDLE, TXN_SENDING, TXN_RECEIVING, TXN_DONE, TXN_FAILED };
enum { UP_IDLE, UP_SEND, UP_RESP };

/* One buffered HTTP request and its (small) response.
 * Buffers here are static or in structs, never large on the stack: a Pico
 * gives each core only 2-4 KB of it. */
typedef struct {
    int h, state;
    char tx[1024];
    size_t tx_len, tx_off;
    char rx[768];
    size_t rx_len;
    uint64_t deadline_us;
    bps_http_response resp;
} txn;

static bps_settings cfg;
static uint8_t client_id[16];
static char id_str[37], hostname[48];
static int state;
static uint64_t state_since_us, boot_us;
static bool need_register;

/* Local clock: monotonic time plus an epoch set once from the server, so the
 * offsets the estimator works with stay small. */
static int64_t epoch_ns;
static bool epoch_set;
static bps_clock clk;

static bps_ring ring;
static bps_capture cap;
static bool capturing;

/* From the latest registration. */
static uint16_t udp_port;
static uint64_t hb_interval_us;
static uint32_t chunk_len, lookback;
static bps_gate_config gate_cfg;

static txn reg_txn, hb_txn, pos_txn;
static uint64_t reg_next_us, reg_delay_us, hb_next_us, status_next_us;
static bool hb_failing;

static struct {
    int h;
    uint32_t seq;
    bool waiting, warned;
    int64_t t1;
    uint64_t sent_us, next_us;
} ts = {.h = -1};

static struct {
    int state, h;
    uint32_t start, end, next, segs;
    bool has_prev;
    uint32_t prev_end;
    union { /* samples are read from the ring straight into the send buffer */
        uint8_t buf[1460];
        int16_t pcm[730];
    };
    size_t len, off;
    char rx[256];
    size_t rx_len;
    bps_http_response resp;
    uint64_t progress_us, deadline_us, retry_us;
    uint32_t lost; /* samples of this chunk sent as silence */
} up = {.h = -1};

static struct {
    uint64_t chunks_sent, chunks_gated;
    uint32_t overruns;
    uint32_t dropouts_reported, dropouts_sending;
    uint32_t idle_mark;
} stats;

static struct {
    bool raw, pressed;
    uint64_t changed_us;
} button;

static bool led_on, levels_logged;

static int64_t local_ns(uint64_t mono_us) { return (int64_t)mono_us * 1000 + epoch_ns; }

static void path_for(char *out, size_t cap, const char *what)
{
    if (what)
        snprintf(out, cap, "%s/%s/%s", BPS_CLIENT_API_PREFIX, id_str, what);
    else
        snprintf(out, cap, "%s/register", BPS_CLIENT_API_PREFIX);
}

/* "-12.34" from hundredths. */
static const char *centi(char *buf, size_t cap, int32_t v)
{
    long a = v < 0 ? -(long)v : v;
    snprintf(buf, cap, "%s%ld.%02ld", v < 0 ? "-" : "", a / 100, a % 100);
    return buf;
}

static void set_state(int s)
{
    state = s;
    state_since_us = port_mono_us();
}

/* ---- HTTP transactions ---- */

/* Reads more of a response: 1 = complete, 0 = not yet, -1 = failed. */
static int read_response(int h, char *buf, size_t cap, size_t *len, bps_http_response *resp)
{
    int n = 0;
    if (*len + 1 < cap) {
        n = port_tcp_read(h, buf + *len, cap - 1 - *len);
        if (n > 0)
            *len += (size_t)n;
    }
    buf[*len] = 0;
    bool head = bps_http_parse(buf, *len, resp);
    if (head) {
        size_t body = *len - resp->body_off;
        if (resp->status == 204 || resp->status == 304 ||
            (resp->content_length >= 0 && body >= (size_t)resp->content_length) || *len + 1 >= cap)
            return 1;
    }
    if (n < 0)
        return head ? 1 : -1;
    return 0;
}

/* Connecting blocks the loop (and so the audio stream), so say when it is slow. */
static int connect_server(const char *what)
{
    uint64_t t0 = port_mono_us();
    int h = port_tcp_connect(cfg.server_host, cfg.server_port);
    uint64_t ms = (port_mono_us() - t0) / 1000;
    if (ms >= 200)
        port_log("net: connecting for %s took %lu ms%s", what, (unsigned long)ms,
                 h < 0 ? " and failed" : "");
    return h;
}

static void txn_close(txn *t)
{
    if (t->h >= 0)
        port_tcp_close(t->h);
    t->h = -1;
}

static void txn_reset(txn *t)
{
    txn_close(t);
    t->state = TXN_IDLE;
}

static void txn_start(txn *t, const char *what, const char *path, const char *body,
                      size_t body_len)
{
    size_t head = bps_http_head(t->tx, sizeof t->tx, "POST", cfg.server_host, cfg.server_port, path,
                                body_len ? "application/json" : NULL, body_len, NULL);
    t->state = TXN_FAILED;
    if (!head || head + body_len > sizeof t->tx)
        return;
    memcpy(t->tx + head, body, body_len);
    t->tx_len = head + body_len;
    t->tx_off = 0;
    t->rx_len = 0;
    t->h = connect_server(what);
    if (t->h < 0)
        return;
    t->state = TXN_SENDING;
    t->deadline_us = port_mono_us() + TXN_TIMEOUT_US;
}

static void txn_poll(txn *t)
{
    if (t->state != TXN_SENDING && t->state != TXN_RECEIVING)
        return;
    if (port_mono_us() > t->deadline_us) {
        txn_close(t);
        t->state = TXN_FAILED;
        return;
    }
    if (t->state == TXN_SENDING) {
        int n = port_tcp_write(t->h, t->tx + t->tx_off, t->tx_len - t->tx_off);
        if (n < 0) {
            txn_close(t);
            t->state = TXN_FAILED;
            return;
        }
        t->tx_off += (size_t)n;
        if (t->tx_off < t->tx_len)
            return;
        t->state = TXN_RECEIVING;
    }
    int r = read_response(t->h, t->rx, sizeof t->rx, &t->rx_len, &t->resp);
    if (r != 0) {
        txn_close(t);
        t->state = r > 0 ? TXN_DONE : TXN_FAILED;
    }
}

static bool txn_ok(const txn *t)
{
    return t->state == TXN_DONE && t->resp.status >= 200 && t->resp.status < 300;
}

static bool txn_unknown(const txn *t) { return t->state == TXN_DONE && t->resp.status == 404; }

/* ---- Registration ---- */

static void start_register(void)
{
    static char body[640];
    char path[64];
    bps_json j;
    bps_json_init(&j, body, sizeof body);
    bps_json_raw(&j, "{");
    bps_json_key(&j, "client_id");
    bps_json_str(&j, id_str);
    bps_json_key(&j, "hostname");
    bps_json_str(&j, hostname);
    bps_json_key(&j, "version");
    bps_json_str(&j, cfg.version);
    bps_json_key(&j, "capabilities");
    bps_json_raw(&j, "{");
    bps_json_key(&j, "sample_rate");
    bps_json_i64(&j, cfg.sample_rate);
    bps_json_key(&j, "channels");
    bps_json_i64(&j, 1);
    bps_json_key(&j, "sample_format");
    bps_json_str(&j, "I32");
    bps_json_key(&j, "clock_source");
    bps_json_str(&j, "crystal (UDP timesync)");
    bps_json_key(&j, "device_name");
    bps_json_str(&j, cfg.device_name);
    bps_json_raw(&j, "}");
    if (cfg.name[0]) {
        bps_json_key(&j, "name");
        bps_json_str(&j, cfg.name);
    }
    bps_json_raw(&j, "}");
    path_for(path, sizeof path, NULL);
    txn_start(&reg_txn, "registration", path, body, j.overflow ? 0 : j.len);
}

static void start_capture(void)
{
    /* As much as asked for, but always leave some heap for the network stack. */
    size_t free_bytes = port_free_heap();
    uint32_t want = cfg.ring_bytes / 2;
    uint32_t fits = free_bytes > HEAP_RESERVE ? (uint32_t)((free_bytes - HEAP_RESERVE) / 2) : 0;
    if (fits < want)
        want = fits;
    want = want / BPS_RING_BLOCK * BPS_RING_BLOCK;
    uint32_t got = bps_ring_alloc(&ring, cfg.sample_rate, want);
    if (got < cfg.ring_bytes / 2 / BPS_RING_BLOCK * BPS_RING_BLOCK)
        port_log("audio: %lu of the %lu KB asked for fit in the ring buffer",
                 (unsigned long)got / 512, (unsigned long)cfg.ring_bytes / 1024);
    uint32_t frame = cfg.sample_rate * gate_cfg.frame_ms / 1000;
    uint32_t slack = cfg.sample_rate * RING_SLACK_MS / 1000 + 4096 + frame;
    uint32_t max_lb = got > slack ? got - slack : 0;
    lookback = (uint32_t)((uint64_t)cfg.lookback_ms * cfg.sample_rate / 1000);
    if (lookback > max_lb) {
        port_log("audio: look-back limited to %lu ms by the ring buffer",
                 (unsigned long)((uint64_t)max_lb * 1000 / cfg.sample_rate));
        lookback = max_lb;
    }
    port_log("audio: %lu ms ring buffer, %lu ms look-back, %lu bytes free",
             (unsigned long)((uint64_t)got * 1000 / cfg.sample_rate),
             (unsigned long)((uint64_t)lookback * 1000 / cfg.sample_rate),
             (unsigned long)port_free_heap());
    bps_capture_init(&cap, &ring, cfg.sample_rate, cfg.mic_channel, &gate_cfg);
    stats.idle_mark = bps_ring_written(&ring);
    port_audio_start(cfg.sample_rate);
    capturing = true;
}

static bool apply_registration(void)
{
    const char *body = reg_txn.rx + reg_txn.resp.body_off;
    size_t len = reg_txn.rx_len - reg_txn.resp.body_off;
    double port, hb, chunk, lo, hi, thr, frame;
    if (!bps_json_number(body, len, "udp_timesync_port", &port) ||
        !bps_json_number(body, len, "heartbeat_interval_s", &hb) ||
        !bps_json_number(body, len, "chunk_secs", &chunk))
        return false;
    udp_port = (uint16_t)port;
    hb_interval_us = (uint64_t)(hb < 1 ? 1 : hb) * 1000000;
    /* The gate settings and chunk length apply from the first registration on. */
    if (!capturing) {
        chunk_len = (uint32_t)(chunk * cfg.sample_rate);
        bps_gate_config_default(&gate_cfg);
        if (bps_json_number(body, len, "band_low_hz", &lo) &&
            bps_json_number(body, len, "band_high_hz", &hi) &&
            bps_json_number(body, len, "threshold_db", &thr) &&
            bps_json_number(body, len, "frame_ms", &frame)) {
            gate_cfg.band_low_hz = (float)lo;
            gate_cfg.band_high_hz = (float)hi;
            gate_cfg.threshold_db = (float)thr;
            gate_cfg.frame_ms = (uint32_t)frame;
        }
        start_capture();
    }
    return true;
}

static void drop_connections(void)
{
    txn_reset(&reg_txn);
    txn_reset(&hb_txn);
    txn_reset(&pos_txn);
    if (up.state != UP_IDLE) {
        port_tcp_close(up.h);
        up.h = -1;
        up.state = UP_IDLE;
        up.has_prev = true;
        up.prev_end = up.next;
    }
    if (ts.h >= 0)
        port_udp_close(ts.h);
    ts.h = -1;
    ts.waiting = false;
}

static void start_run(uint64_t now)
{
    ts.h = port_udp_open(cfg.server_host, udp_port);
    if (ts.h < 0)
        port_log("timesync: cannot open UDP to %s:%u", cfg.server_host, (unsigned)udp_port);
    ts.waiting = false;
    ts.next_us = now;
    hb_next_us = now;
    set_state(ST_RUN);
}

static void register_poll(uint64_t now)
{
    if (reg_txn.state == TXN_IDLE) {
        if (now < reg_next_us)
            return;
        start_register();
    }
    txn_poll(&reg_txn);
    if (reg_txn.state != TXN_DONE && reg_txn.state != TXN_FAILED)
        return;
    bool ok = reg_txn.state == TXN_DONE && reg_txn.resp.status == 200 && apply_registration();
    int status = reg_txn.state == TXN_DONE ? reg_txn.resp.status : 0;
    txn_reset(&reg_txn);
    if (ok) {
        port_log("registered with %s:%u as %s", cfg.server_host, (unsigned)cfg.server_port, id_str);
        reg_delay_us = 1000000;
        start_run(now);
        return;
    }
    if (status)
        port_log("registration failed: server returned %d; retrying in %lu s", status,
                 (unsigned long)(reg_delay_us / 1000000));
    else
        port_log("registration failed: no answer from %s:%u; retrying in %lu s", cfg.server_host,
                 (unsigned)cfg.server_port, (unsigned long)(reg_delay_us / 1000000));
    reg_next_us = now + reg_delay_us;
    reg_delay_us = reg_delay_us * 2 > 30000000 ? 30000000 : reg_delay_us * 2;
}

/* ---- Clock measurement ---- */

static void timesync_handle(const uint8_t *buf, int n, uint64_t at_us)
{
    bps_ts_response r;
    if (!ts.waiting || !bps_ts_response_decode(buf, (size_t)n, &r) || r.seq != ts.seq)
        return; /* stale reply to an earlier, timed-out request */
    ts.waiting = false;
    ts.warned = false;
    bps_clock_count_request(&clk);
    /* t4 from the monotonic clock, like t1. */
    int64_t t4 = ts.t1 + (int64_t)(at_us - ts.sent_us) * 1000;
    if (!epoch_set) {
        int64_t off, rtt;
        bps_ts_offset_rtt(&r, t4, &off, &rtt);
        epoch_ns += off;
        epoch_set = true;
        port_log("clock: set from the server (round trip %ld us)", (long)(rtt / 1000));
        return;
    }
    bps_ts_sample s;
    if (bps_ts_sample_of(&r, t4, &s))
        bps_clock_add_sample(&clk, s);
}

static bool timesync_recv(void)
{
    uint8_t buf[64];
    int n = port_udp_recv(ts.h, buf, sizeof buf);
    if (n <= 0)
        return false;
    timesync_handle(buf, n, port_mono_us());
    return true;
}

static void timesync_poll(uint64_t now)
{
    if (ts.h < 0)
        return;
    if (ts.waiting) {
        while (timesync_recv())
            ;
        if (ts.waiting && now - ts.sent_us > TIMESYNC_TIMEOUT_US) {
            ts.waiting = false;
            bps_clock_count_request(&clk);
            if (!ts.warned)
                port_log("timesync: no reply from %s:%u", cfg.server_host, (unsigned)udp_port);
            ts.warned = true;
        }
    }
    if (ts.waiting || now < ts.next_us)
        return;
    ts.next_us = now - ts.next_us > TIMESYNC_INTERVAL_US ? now + TIMESYNC_INTERVAL_US
                                                         : ts.next_us + TIMESYNC_INTERVAL_US;
    while (timesync_recv()) /* drain late replies */
        ;
    bps_ts_request req = {.seq = ++ts.seq};
    memcpy(req.client_id, client_id, 16);
    uint64_t m = port_mono_us();
    req.t1 = local_ns(m);
    uint8_t pkt[BPS_TS_REQUEST_LEN];
    bps_ts_request_encode(&req, pkt);
    ts.t1 = req.t1;
    ts.sent_us = m;
    if (port_udp_send(ts.h, pkt, sizeof pkt) < 0)
        return;
    ts.waiting = true;
    while (ts.waiting && port_mono_us() - ts.sent_us < TIMESYNC_SPIN_US)
        timesync_recv();
}

/* ---- Heartbeats ---- */

static uint32_t dropouts_total(void) { return LOAD(&cap.dropouts) + stats.overruns; }

static void start_heartbeat(uint64_t now)
{
    static char body[768];
    char path[96];
    bps_json j;
    bps_json_init(&j, body, sizeof body);
    bps_json_raw(&j, "{");
    bps_json_key(&j, "clock");
    bps_clock_status st;
    if (bps_clock_status_take(&clk, local_ns(now), &st)) {
        bps_json_raw(&j, "{");
        bps_json_key(&j, "offset_ns");
        bps_json_i64(&j, st.offset_ns);
        bps_json_key(&j, "rtt_ns");
        bps_json_i64(&j, st.rtt_ns);
        bps_json_key(&j, "measured_at");
        bps_json_i64(&j, st.measured_at);
        bps_json_key(&j, "error_ns");
        bps_json_i64(&j, st.error_ns);
        bps_json_key(&j, "detail");
        bps_json_raw(&j, "{");
        bps_json_key(&j, "corrects_timestamps");
        bps_json_bool(&j, st.corrects_timestamps);
        bps_json_key(&j, "clock_offset_ns");
        bps_json_i64(&j, st.clock_offset_ns);
        bps_json_key(&j, "drift_ppm");
        bps_json_fixed(&j, st.drift_ppm, 4);
        bps_json_key(&j, "fit_points");
        bps_json_i64(&j, st.fit_points);
        bps_json_key(&j, "requests");
        bps_json_i64(&j, st.requests);
        bps_json_key(&j, "replies");
        bps_json_i64(&j, st.replies);
        const char *keys[3] = {"rtt_min_ns", "rtt_median_ns", "rtt_max_ns"};
        int64_t vals[3] = {st.rtt_min_ns, st.rtt_median_ns, st.rtt_max_ns};
        for (int i = 0; i < 3; i++) {
            bps_json_key(&j, keys[i]);
            if (st.has_rtts)
                bps_json_i64(&j, vals[i]);
            else
                bps_json_raw(&j, "null");
        }
        bps_json_raw(&j, "}}");
    } else {
        bps_json_raw(&j, "null");
    }
    stats.dropouts_sending = dropouts_total();
    bps_json_key(&j, "uptime_s");
    bps_json_i64(&j, (int64_t)((now - boot_us) / 1000000));
    bps_json_key(&j, "chunks_sent");
    bps_json_i64(&j, (int64_t)stats.chunks_sent);
    bps_json_key(&j, "chunks_gated");
    bps_json_i64(&j, (int64_t)stats.chunks_gated);
    bps_json_key(&j, "queue_len");
    bps_json_i64(&j, 0);
    bps_json_key(&j, "capture_dropouts");
    bps_json_i64(&j, stats.dropouts_sending - stats.dropouts_reported);
    bps_json_raw(&j, "}");
    path_for(path, sizeof path, "heartbeat");
    txn_start(&hb_txn, "a heartbeat", path, body, j.overflow ? 0 : j.len);
}

static void heartbeat_poll(uint64_t now)
{
    if (hb_txn.state == TXN_IDLE && now >= hb_next_us) {
        hb_next_us = now - hb_next_us > hb_interval_us ? now + hb_interval_us
                                                       : hb_next_us + hb_interval_us;
        start_heartbeat(now);
    }
    txn_poll(&hb_txn);
    if (hb_txn.state != TXN_DONE && hb_txn.state != TXN_FAILED)
        return;
    if (txn_ok(&hb_txn)) {
        /* Dropouts are reported once the server has them. */
        stats.dropouts_reported = stats.dropouts_sending;
        if (hb_failing)
            port_log("heartbeat: delivered again");
        hb_failing = false;
    } else if (txn_unknown(&hb_txn)) {
        port_log("server forgot this client; re-registering");
        need_register = true;
    } else if (!hb_failing) {
        if (hb_txn.state == TXN_DONE)
            port_log("heartbeat: server returned %d", hb_txn.resp.status);
        else
            port_log("heartbeat: no answer");
        hb_failing = true;
    }
    txn_reset(&hb_txn);
}

/* ---- Positioning button ---- */

static void position_poll(uint64_t now)
{
    bool raw = port_button();
    if (raw != button.raw) {
        button.raw = raw;
        button.changed_us = now;
    } else if (raw != button.pressed && now - button.changed_us > 30000) {
        button.pressed = raw;
        if (raw && pos_txn.state == TXN_IDLE) {
            char path[96];
            path_for(path, sizeof path, "position-request");
            txn_start(&pos_txn, "a positioning request", path, "", 0);
        }
    }
    txn_poll(&pos_txn);
    if (pos_txn.state != TXN_DONE && pos_txn.state != TXN_FAILED)
        return;
    if (txn_ok(&pos_txn)) {
        port_log(">> Positioning requested; set this client's position in the web UI.");
    } else if (txn_unknown(&pos_txn)) {
        port_log(">> Positioning request failed: server does not know this client");
        need_register = true;
    } else {
        port_log(">> Positioning request failed");
    }
    txn_reset(&pos_txn);
}

/* ---- Audio uploads ---- */

static void upload_finish(void)
{
    port_tcp_close(up.h);
    up.h = -1;
    up.state = UP_IDLE;
    up.has_prev = true;
}

static void upload_abort(const char *why)
{
    uint32_t sent = up.next - up.start;
    port_log("upload: aborted (%s) after %lu of %lu ms of audio; nothing written for %lu ms", why,
             (unsigned long)((uint64_t)sent * 1000 / cfg.sample_rate),
             (unsigned long)((uint64_t)chunk_len * 1000 / cfg.sample_rate),
             (unsigned long)((port_mono_us() - up.progress_us) / 1000));
    up.prev_end = up.next;
    upload_finish();
}

/* Starts uploading `chunk_len` samples from ring index `start`. */
static void upload_begin(uint32_t start, uint64_t now)
{
    uint32_t w = bps_ring_written(&ring);
    uint32_t keep = ring.capacity - 4096 - cfg.sample_rate * RING_SLACK_MS / 1000;
    if (bps_idx_diff(w, start) > (int32_t)keep)
        start = w - keep;
    /* A chunk must not span a timeline jump: start after the latest one. */
    int64_t mono_ts;
    bool has_next;
    uint32_t next_idx;
    for (;;) {
        if (!bps_ring_locate(&ring, start, &mono_ts, &has_next, &next_idx)) {
            up.has_prev = true; /* older than any remembered timeline: skip it */
            up.prev_end = w;
            return;
        }
        if (!has_next || bps_idx_diff(next_idx, start) >= (int32_t)chunk_len)
            break;
        start = next_idx;
    }
    int64_t len_ns = bps_samples_to_ns(chunk_len, cfg.sample_rate);
    int64_t start_ns = bps_clock_correct(&clk, mono_ts + epoch_ns, len_ns);

    char extra[64], path[96];
    bps_json x;
    bps_json_init(&x, extra, sizeof extra);
    bps_json_raw(&x, BPS_HEADER_START_NS ": ");
    bps_json_i64(&x, start_ns);
    bps_json_raw(&x, "\r\n");
    path_for(path, sizeof path, "audio");
    size_t head = bps_http_head((char *)up.buf, sizeof up.buf - BPS_WAV_HEADER_LEN, "POST",
                                cfg.server_host, cfg.server_port, path, "audio/wav",
                                BPS_WAV_HEADER_LEN + 2 * (size_t)chunk_len, extra);
    if (!head)
        return;
    bps_wav_header(up.buf + head, cfg.sample_rate, chunk_len);
    up.len = head + BPS_WAV_HEADER_LEN;
    up.off = 0;
    up.h = connect_server("an upload");
    if (up.h < 0) {
        port_log("upload: cannot connect to %s:%u", cfg.server_host, (unsigned)cfg.server_port);
        up.retry_us = now + 1000000;
        return;
    }
    up.start = start;
    up.end = start + chunk_len;
    up.next = start;
    up.segs = LOAD(&ring.n_segs);
    up.lost = 0;
    up.rx_len = 0;
    up.progress_us = now;
    up.state = UP_SEND;
}

static void upload_pump(uint64_t now)
{
    for (int i = 0; i < 16; i++) {
        if (up.off == up.len) {
            if (up.next == up.end) {
                if (up.lost) {
                    stats.overruns++;
                    port_log("upload: the network stalled; %lu ms of this chunk were lost and "
                             "sent as silence",
                             (unsigned long)((uint64_t)up.lost * 1000 / cfg.sample_rate));
                }
                up.state = UP_RESP;
                up.deadline_us = now + TXN_TIMEOUT_US;
                return;
            }
            int32_t avail = bps_idx_diff(bps_ring_written(&ring), up.next);
            if (avail <= 0)
                break;
            uint32_t k = sizeof up.buf / 2;
            if ((uint32_t)avail < k)
                k = (uint32_t)avail;
            if (up.end - up.next < k)
                k = up.end - up.next;
            if (LOAD(&ring.n_segs) != up.segs) {
                int64_t t;
                bool has_next;
                uint32_t nx;
                if (!bps_ring_locate(&ring, up.start, &t, &has_next, &nx) ||
                    (has_next && bps_idx_diff(nx, up.end) < 0)) {
                    upload_abort("capture timeline jumped");
                    return;
                }
                up.segs = LOAD(&ring.n_segs);
            }
            /* WAV is little-endian, like every chip this runs on. */
            if (!bps_ring_read(&ring, up.next, up.pcm, k)) {
                /* The network stalled for longer than the ring holds. Send
                 * the lost stretch as silence: the chunk keeps its length and
                 * every later sample its exact time (the server zero-fills
                 * gaps in a client's audio the same way). */
                int32_t lost = bps_idx_diff(bps_ring_oldest(&ring), up.next);
                if (lost <= 0)
                    break; /* raced the capture core; try again */
                if ((uint32_t)lost < k)
                    k = (uint32_t)lost;
                memset(up.pcm, 0, k * sizeof up.pcm[0]);
                up.lost += k;
            }
            up.len = 2 * k;
            up.off = 0;
            up.next += k;
        }
        int n = port_tcp_write(up.h, up.buf + up.off, up.len - up.off);
        if (n < 0) {
            upload_abort("connection lost");
            return;
        }
        if (n == 0)
            break;
        up.off += (size_t)n;
        up.progress_us = now;
    }
    if (now - up.progress_us > UPLOAD_STALL_US)
        upload_abort("stalled");
}

static void upload_poll(uint64_t now)
{
    if (up.state == UP_SEND) {
        upload_pump(now);
        return;
    }
    if (up.state == UP_RESP) {
        int r = read_response(up.h, up.rx, sizeof up.rx, &up.rx_len, &up.resp);
        if (r == 0 && now < up.deadline_us)
            return;
        up.prev_end = up.end;
        upload_finish();
        if (r > 0 && up.resp.status >= 200 && up.resp.status < 300) {
            stats.chunks_sent++;
        } else if (r > 0 && up.resp.status == 404) {
            port_log("server forgot this client; re-registering");
            need_register = true;
        } else if (r > 0) {
            port_log("upload: server returned %d", up.resp.status);
        } else {
            port_log("upload: no answer");
        }
        return;
    }

    /* Idle: has the gate opened? Uploads wait for the first clock fit. */
    if (!epoch_set || !bps_est_fit(&clk.est) || now < up.retry_us || !LOAD(&cap.pass_count))
        return;
    uint32_t p = LOAD(&cap.last_pass_end);
    uint32_t w = bps_ring_written(&ring);
    if (bps_idx_diff(w, p) > (int32_t)(ring.capacity / 2) && !up.has_prev) {
        /* Gated long ago (before the clock was ready): too old to send. */
        up.has_prev = true;
        up.prev_end = w;
        return;
    }
    uint32_t start = p - cap.gate.frame_len - lookback;
    if (up.has_prev) {
        /* Nothing new unless it passed near the end of the last chunk or after. */
        if (bps_idx_diff(p, up.prev_end - lookback) <= 0)
            return;
        if (bps_idx_diff(start, up.prev_end) < 0)
            start = up.prev_end; /* still open: continue without a gap */
    }
    upload_begin(start, now);
}

/* Silent stretches count as gated chunks, so the stats compare with bsp-client's. */
static void count_gated(void)
{
    if (up.state != UP_IDLE || chunk_len == 0) {
        stats.idle_mark = up.end;
        return;
    }
    uint32_t w = bps_ring_written(&ring);
    while (bps_idx_diff(w, stats.idle_mark) >= (int32_t)chunk_len) {
        stats.chunks_gated++;
        stats.idle_mark += chunk_len;
    }
}

/* ---- Status ---- */

static bool synced(void)
{
    const bps_ts_fit *f = bps_est_fit(&clk.est);
    return epoch_set && f && f->error_ns <= SYNCED_ERROR_NS;
}

static void update_led(uint64_t now)
{
    uint64_t ms = now / 1000;
    bool on;
    if (state != ST_RUN)
        on = (ms / 250) % 2;
    else if (!synced())
        on = ms % 1000 < 100;
    else if (up.state != UP_IDLE)
        on = ms % 500 >= 50;
    else
        on = true;
    if (on != led_on) {
        led_on = on;
        port_led(on);
    }
}

static void log_status(uint64_t now)
{
    /* The lowest free heap seen, sampled once a second (a falling value means a leak). */
    static size_t heap_low = SIZE_MAX;
    static uint64_t heap_next_us;
    if (now >= heap_next_us) {
        heap_next_us = now + 1000000;
        size_t h = port_free_heap();
        if (h < heap_low)
            heap_low = h;
    }
    char a[16], b[16];
    if (capturing && !levels_logged && LOAD(&cap.levels_ready)) {
        levels_logged = true;
        port_log("mic level: left %s dBFS, right %s dBFS (using %s; set MIC_CHANNEL to change)",
                 centi(a, sizeof a, LOAD(&cap.level_cdb[0])),
                 centi(b, sizeof b, LOAD(&cap.level_cdb[1])), cfg.mic_channel ? "right" : "left");
    }
    if (now < status_next_us)
        return;
    status_next_us = now + STATUS_LOG_US;
    const bps_ts_fit *f = bps_est_fit(&clk.est);
    port_log("status: %s, clock error %ld us (best round trip %ld us), drift %s ppm, sample rate "
             "%s ppm, Wi-Fi %d dBm, %lu sent, %lu gated, %lu dropouts, %lu bytes free (lowest %lu)",
             synced() ? "synced" : "not synced", f ? (long)(f->error_ns / 1000) : -1L,
             f ? (long)(f->rtt_ns / 1000) : -1L,
             centi(a, sizeof a, f ? (int32_t)(bps_fit_drift_ppm(f) * 100) : 0),
             centi(b, sizeof b, LOAD(&cap.rate_err_ppb) / 10000), port_wifi_rssi(),
             (unsigned long)stats.chunks_sent, (unsigned long)stats.chunks_gated,
             (unsigned long)dropouts_total(), (unsigned long)port_free_heap(),
             (unsigned long)heap_low);
}

/* ---- Entry points ---- */

void bps_client_setup(const bps_settings *s)
{
    cfg = *s;
    boot_us = port_mono_us();
    status_next_us = boot_us + STATUS_LOG_US;
    if (!cfg.client_id[0] || !bps_uuid_parse(cfg.client_id, client_id)) {
        uint8_t chip[8];
        port_unique_id(chip);
        bps_uuid_from_chip(chip, client_id);
    }
    bps_uuid_format(client_id, id_str);
    snprintf(hostname, sizeof hostname, "bps-%s-%.8s", cfg.board, id_str);
    bps_clock_init(&clk, true);
    reg_txn.h = hb_txn.h = pos_txn.h = -1;
    port_log("bps client %s on %s, id %s, %lu bytes free", cfg.version, cfg.board, id_str,
             (unsigned long)port_free_heap());
    port_wifi_begin(cfg.wifi_ssid, cfg.wifi_password, hostname);
    set_state(ST_WIFI);
}

void bps_client_loop(void)
{
    uint64_t now = port_mono_us();
    if (state != ST_WIFI && !port_wifi_up()) {
        port_log("wifi: disconnected");
        drop_connections();
        set_state(ST_WIFI);
    }
    switch (state) {
    case ST_WIFI:
        if (port_wifi_up()) {
            port_log("wifi: connected");
            reg_next_us = now;
            reg_delay_us = 1000000;
            set_state(ST_REGISTER);
        } else if (now - state_since_us > WIFI_RETRY_US) {
            port_log("wifi: still not connected to \"%s\"; retrying", cfg.wifi_ssid);
            port_wifi_begin(cfg.wifi_ssid, cfg.wifi_password, hostname);
            state_since_us = now;
        }
        break;
    case ST_REGISTER:
        register_poll(now);
        break;
    case ST_RUN:
        timesync_poll(now);
        heartbeat_poll(now);
        position_poll(now);
        upload_poll(now);
        count_gated();
        if (need_register) {
            need_register = false;
            drop_connections();
            reg_next_us = now;
            set_state(ST_REGISTER);
        }
        break;
    }
    update_led(now);
    log_status(now);
}
