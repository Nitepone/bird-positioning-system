#include "client.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "anchor.h"
#include "beacon.h"
#include "capture.h"
#include "devices.h"
#include "logbuf.h"
#include "ota.h"
#include "power.h"
#include "port.h"
#include "proto.h"
#include "record.h"
#include "ring.h"
#include "spool.h"
#include "timesync.h"
#include "trigger.h"
#include "upload.h"
#include "validate.h"

#define LOAD(p) __atomic_load_n(p, __ATOMIC_ACQUIRE)
#define STORE(p, v) __atomic_store_n(p, v, __ATOMIC_RELEASE)

#define TIMESYNC_INTERVAL_US 500000
#define TIMESYNC_TIMEOUT_US 400000
/* Replies on a LAN take a few ms; waiting for them right after sending keeps
 * the rest of the loop out of the measured round trip. */
#define TIMESYNC_SPIN_US 20000
#define TXN_TIMEOUT_US 10000000
#define WIFI_RETRY_US 20000000
#define STATUS_LOG_US 60000000
/* The server's default `max_clock_offset_us`, for the LED only. */
#define SYNCED_ERROR_NS 2000000
/* Heap left for the network stack and Wi-Fi driver after the ring buffer. */
#define HEAP_RESERVE (32 * 1024)
/* The store on boards without PSRAM, unless the settings say otherwise, and
 * the least worth having. */
#define STORE_DEFAULT_BYTES (32 * 1024)
#define STORE_MIN_BYTES (16 * 1024)
/* Boards with PSRAM size the store to it: all that is free but this (the
 * card's write queue, the network stack, smaller buffers), the ring's way
 * back from internal RAM, and what the selected model needs. */
#define BIG_RESERVE (512 * 1024)
#define STORE_FIT_US 10000000
#define STORE_FIT_STEP (256 * 1024) /* resized only for at least this */
/* Internal RAM left free after a ring buffer there (low-power mode): Wi-Fi's
 * buffers and the tasks' stacks must come from it. The ring takes what is
 * free above this, up to the full look-back. */
#define INTERNAL_RESERVE (32 * 1024)
/* The shortest look-back worth moving the ring to internal RAM for. */
#define LOOKBACK_MIN_MS 250
/* How often a ring that did not fit in internal RAM tries again. */
#define RING_RETRY_US 60000000

/* Batch mode's contacts: the radio comes on, the client joins Wi-Fi and
 * registers, measures its clock afresh, uploads what it saved, sends a last
 * heartbeat, and the radio goes off again. */
#define CONTACT_CONNECT_US 30000000 /* to join and register, or wait for the next */
/* Clock measurements in the contact before its first anchor (6 s), and
 * beacons in the line through the access point's clock, if it is heard. */
#define CONTACT_SYNC_REPLIES 12
#define CONTACT_SYNC_BEACONS 24
/* Then the anchor is taken anyway, if there is any measurement. */
#define CONTACT_SYNC_MAX_US 30000000
/* The radio stays on at most this long per contact; what is left of the
 * card goes up next time. */
#define CONTACT_MAX_US 120000000
/* Contacts are at least this far apart, however long the last one took. */
#define CONTACT_MIN_GAP_US 5000000
/* Anchors are taken this often while in contact (all modes). */
#define ANCHOR_EVERY_US 10000000
/* Live mode: how often chunks left on the card are looked for again. */
#define LIVE_SPOOL_US 30000000
/* With the radio off, the loop rests this long between passes (the ring
 * buffer holds seconds; saving a chunk keeps up easily). */
#define OFF_IDLE_MS 10

/* Low-power live mode: connected, uploading as it records, but with the
 * radio in modem sleep between a burst of clock measurements this often
 * (with power saving off for it), the heartbeat (in every second burst's
 * wake) and uploads. The clock is measured against the server only: beacon
 * capture is off, so there is no access point clock. Fits through the
 * 64 s window (timesync.h) still see four or five bursts. */
#define LP_BURST_EVERY_US 15000000
#define LP_BURST_REQUESTS 8
/* From power saving going off to the first request: the access point learns
 * the radio is awake, and stops holding its packets. */
#define LP_SETTLE_US 30000
#define LP_BURST_GAP_US 30000
/* The server allows for it (bsp-proto's LOW_POWER_HEARTBEAT_S). */
#define LP_HEARTBEAT_US 30000000
/* Ultra-low-power mode (nights): as low-power mode, but the radio wakes once
 * every ULP_WAKE_US for a clock burst, the heartbeat (bsp-proto's
 * ULTRA_LOW_POWER_HEARTBEAT_S) and the recordings stored since, together;
 * sooner only once the store is ULP_FLUSH_PCT full. The clock fit keeps
 * ULP_CLOCK_WINDOW_NS of bursts, so drift still shows (best effort: a
 * timestamp is extrapolated up to a wake ahead). A sound must pass the gate
 * for ULP_MIN_RUN frames in a row to be recorded. */
#define ULP_WAKE_US 300000000ull
#define ULP_BURST_REQUESTS 16
#define ULP_CLOCK_WINDOW_NS (16 * 60 * BPS_NANOS_PER_SEC)
#define ULP_MIN_RUN 3
#define ULP_FLUSH_PCT 50

enum { ST_WIFI, ST_REGISTER, ST_RUN, ST_OFF };
enum { MODE_LIVE, MODE_BATCH, MODE_LOW_POWER, MODE_ULTRA_LOW };
static const char *const MODE_NAMES[] = {"live", "batch", "low_power", "ultra_low_power"};
/* The modes with the radio sleeping between wakes, connected. */
static bool lp_mode(int m) { return m == MODE_LOW_POWER || m == MODE_ULTRA_LOW; }
/* The phases of a contact, once registered. */
enum { C_SYNC, C_FLUSH, C_LAST_FLUSH, C_FINAL };
enum { TXN_IDLE, TXN_SENDING, TXN_RECEIVING, TXN_DONE, TXN_FAILED };

/* One buffered HTTP request and its (small) response.
 * Buffers here are static or in structs, never large on the stack: a Pico
 * gives each core only 2-4 KB of it. */
typedef struct {
    int h, state;
    char *tx; /* the request: head and body */
    size_t tx_cap, tx_len, tx_off;
    char *rx; /* the response */
    size_t rx_cap, rx_len;
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
/* Recordings waiting to be uploaded (store.h). store_fits: it is sized to
 * the free PSRAM (boards with some), resized as a model comes and goes. */
static bps_store store;
static bool store_fits, store_waiting;
static uint64_t store_fit_us;
/* Where the ring is: in PSRAM (where there is some), or in the chip's own
 * RAM in low-power mode, which leaves PSRAM idle more of the time. There it
 * is as big as fits, and the look-back shrinks to match.
 * ring_refused: the move last tried could not be made (-1: none), and
 * ring_retry_us when to try again; ring_note says why, for the log. */
static const bps_ring_mem RING_BIG = {port_big_alloc, port_big_free};
static const bps_ring_mem RING_INTERNAL = {port_internal_alloc, port_internal_free};
static bool ring_internal;
static int ring_refused = -1;
static uint64_t ring_retry_us;
static char ring_note[112];
static uint32_t ring_big_samples; /* the ring's size in PSRAM */
/* The connected access point's clock, from its beacons (where the radio can). */
static bps_beacon beacon;
static bool capturing;

/* From the latest registration. */
static uint16_t udp_port;
static uint64_t hb_interval_us;
/* lookback: in use, samples; lookback_full: as set, as far as the PSRAM ring allows. */
static uint32_t chunk_len, lookback, lookback_full;
/* The gate as the capture side runs it (the energy trigger). */
static bps_gate_config gate_cfg;

/* The trigger and validation: as the server last asked, and as run. This
 * build runs the energy trigger and no validation; other types fall back to
 * those. A new frame length waits for a restart (see bps_capture_set_gate). */
static struct {
    bps_trigger_config want, run;
    bps_validation_config want_v, run_v;
} trig;

static txn reg_txn, hb_txn, pos_txn, val_txn;
/* Heartbeats carry the clock, the devices' reports, the mode, and the trace
 * log's lines (as many as fit), and their replies maintenance commands; the
 * others are small. A heartbeat's body is written in place, after room for
 * the request head (txn_start_at). */
#define HB_HEAD_ROOM 320
static char reg_tx[1536], hb_tx[4096], pos_tx[512], val_tx[768];
static char reg_rx[768], hb_rx[1024], pos_rx[768], val_rx[768];
static uint64_t reg_next_us, reg_delay_us, hb_next_us, status_next_us;
static bool hb_failing;

static struct {
    int h;
    uint32_t seq;
    bool waiting, warned;
    int64_t t1;
    uint64_t sent_us, next_us;
    uint32_t burst_left; /* low-power mode: requests still to send in this burst */
} ts = {.h = -1};

static struct {
    uint64_t chunks_sent, chunks_gated, chunks_rejected;
    uint32_t kept_seen; /* bps_upload_kept() when count_gated() last looked */
    uint32_t reports_skipped; /* validation reports not sent: one still on its way */
    uint32_t overruns;
    uint32_t dropouts_reported, dropouts_sending;
    uint32_t idle_mark;
} stats;

static struct {
    bool raw, pressed;
    uint64_t changed_us;
} button;

/* The operation mode: as the server asks in heartbeat replies, and as run. */
static struct {
    int want, run;
    uint32_t interval_s;
    const char *note; /* why `run` is not `want` */
    /* A session is one connection: registered until disconnected. */
    uint32_t session;
    uint64_t session_us;
    uint32_t replies, beacons_heard; /* clock replies in the session; beacons before it */
    bool session_synced;             /* measured afresh: anchors can be taken */
    uint64_t anchor_us;              /* when the latest was */
    uint64_t live_spool_us;
    /* Batch mode. */
    uint64_t contact_us; /* when the radio came on for this contact */
    int phase;
    bool final_hb; /* the heartbeat in flight is the contact's last */
    uint64_t next_contact_us;
    /* The radio's share of the time since the mode started. */
    uint64_t since_us, on_us, on_mark_us;
    bool radio_on;
    /* The latest gap: how far the clock moved beyond its drift. */
    bool has_gap, gap_has_tsf;
    uint32_t gap_off_s;
    int64_t gap_clock_ns, gap_tsf_ns;
} mode = {.interval_s = 300, .radio_on = true};

/* Low-power live mode, as run (lp_poll()). */
static struct {
    bool on;
    int level; /* port_wifi_power()'s, as last set */
    uint64_t next_burst_us;
    /* Time with the radio awake (power saving off) since the mode started. */
    uint64_t since_us, awake_us, mark_us;
    bool awake;
    uint32_t bursts, wakes;
    /* Ultra-low-power mode's settings are in force; its stored recordings
     * may go up now (a wake, or the store filling). */
    bool ultra, flush;
} lp = {.level = PORT_WIFI_FULL};

/* The clock is measured in bursts, not every TIMESYNC_INTERVAL_US: low-power
 * mode, once this session's clock is measured. */
static bool lp_bursts(void) { return lp_mode(mode.run) && mode.session_synced; }

static bps_anchors anchors;

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

/* Sends the `len` bytes of request in the transaction's buffer. */
static void txn_send(txn *t, const char *what, size_t len)
{
    t->tx_len = len;
    t->tx_off = 0;
    t->rx_len = 0;
    t->h = connect_server(what);
    if (t->h < 0)
        return;
    t->state = TXN_SENDING;
    t->deadline_us = port_mono_us() + TXN_TIMEOUT_US;
}

static void txn_start(txn *t, const char *what, const char *path, const char *body,
                      size_t body_len)
{
    size_t head = bps_http_head(t->tx, t->tx_cap, "POST", cfg.server_host, cfg.server_port, path,
                                body_len ? "application/json" : NULL, body_len, NULL);
    t->state = TXN_FAILED;
    if (!head || head + body_len > t->tx_cap)
        return;
    memcpy(t->tx + head, body, body_len);
    txn_send(t, what, head + body_len);
}

/* txn_start() for a body already in the transaction's buffer, at `off`. */
static void txn_start_at(txn *t, const char *what, const char *path, size_t off, size_t body_len)
{
    static char head[HB_HEAD_ROOM];
    size_t n = bps_http_head(head, sizeof head, "POST", cfg.server_host, cfg.server_port, path,
                             body_len ? "application/json" : NULL, body_len, NULL);
    t->state = TXN_FAILED;
    if (!n || n > off || off + body_len > t->tx_cap)
        return;
    memmove(t->tx + n, t->tx + off, body_len);
    memcpy(t->tx, head, n);
    txn_send(t, what, n + body_len);
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
    int r = read_response(t->h, t->rx, t->rx_cap, &t->rx_len, &t->resp);
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

static bool ota_advertised(void);

static void start_register(void)
{
    static char body[1152];
    char path[64];
    bps_json j;
    bps_json_init(&j, body, sizeof body);
    bps_json_raw(&j, "{");
    bps_json_str(&j, "client_id", id_str);
    bps_json_str(&j, "hostname", hostname);
    bps_json_str(&j, "version", cfg.version);
    bps_json_str(&j, "api_version", BPS_API_VERSION);
    bps_json_obj(&j, "capabilities");
    bps_json_i64(&j, "sample_rate", cfg.sample_rate);
    bps_json_i64(&j, "channels", 1);
    bps_json_str(&j, "sample_format", "I32");
    bps_json_str(&j, "clock_source", "crystal (UDP timesync)");
    bps_json_str(&j, "device_name", cfg.device_name);
    bps_json_raw(&j, "}");
    if (cfg.name[0]) {
        bps_json_str(&j, "name", cfg.name);
    }
    if (cfg.build)
        bps_build_json(&j, cfg.build);
    /* Only where the board can update itself (not the Pico boards). */
    if (ota_advertised())
        bps_json_bool(&j, "ota", true);
    bps_json_raw(&j, "}");
    if (j.overflow)
        port_log("registration: too long for its buffer; not sent");
    path_for(path, sizeof path, NULL);
    txn_start(&reg_txn, "registration", path, body, j.overflow ? 0 : j.len);
}

/* ---- Trigger and validation ---- */

static void log_trigger(const char *what)
{
    char d[96];
    bps_trigger_describe(&trig.run, d, sizeof d);
    if (trig.run_v.type == BPS_VALIDATION_MODEL)
        port_log("%s: %s; validation: model %s", what, d, trig.run_v.model);
    else
        port_log("%s: %s; validation: %s", what, d, trig.run_v.name);
}

/* What runs for what the server asks: unknown types fall back. */
static void trigger_runnable(const bps_trigger_config *t, const bps_validation_config *v,
                             bps_trigger_config *run, bps_validation_config *run_v)
{
    if (t->type == BPS_TRIGGER_ENERGY)
        *run = *t;
    else
        bps_trigger_default(run);
    if (v->type != BPS_VALIDATION_UNKNOWN)
        *run_v = *v; /* a model: validate.h loads it, and says if it cannot */
    else
        bps_validation_default(run_v);
    if (t->type != BPS_TRIGGER_ENERGY)
        port_log("trigger: '%s' is not supported by this firmware; the energy trigger runs",
                 t->name);
    if (v->type == BPS_VALIDATION_UNKNOWN)
        port_log("validation: '%s' is not supported by this firmware, or names an unusable "
                 "model; every recording is uploaded",
                 v->name);
}

/* Before capture starts: everything applies, the frame length included. */
static void trigger_start(const bps_trigger_config *t, const bps_validation_config *v)
{
    trig.want = *t;
    trig.want_v = *v;
    trigger_runnable(t, v, &trig.run, &trig.run_v);
    gate_cfg = trig.run.gate;
    bps_validate_configure(&trig.run_v);
    log_trigger("trigger");
}

/* While capturing, from a heartbeat reply or a later registration. */
static void apply_trigger(const bps_trigger_config *t, const bps_validation_config *v)
{
    if (bps_trigger_equal(t, &trig.want) && bps_validation_equal(v, &trig.want_v))
        return;
    bps_trigger_config run;
    bps_validation_config run_v;
    trigger_runnable(t, v, &run, &run_v);
    if (run.gate.frame_ms != gate_cfg.frame_ms) {
        port_log("trigger: %lu ms frames apply after a restart; %lu ms meanwhile",
                 (unsigned long)run.gate.frame_ms, (unsigned long)gate_cfg.frame_ms);
        run.gate.frame_ms = gate_cfg.frame_ms;
    }
    const bps_gate_config *g = &run.gate;
    if (g->band_low_hz != gate_cfg.band_low_hz || g->band_high_hz != gate_cfg.band_high_hz ||
        g->threshold_db != gate_cfg.threshold_db) {
        bool same_band =
            g->band_low_hz == gate_cfg.band_low_hz && g->band_high_hz == gate_cfg.band_high_hz;
        if (!bps_capture_set_gate(&cap, g, same_band))
            return; /* the previous change is still on its way: next reply */
        gate_cfg = *g;
    }
    trig.want = *t;
    trig.want_v = *v;
    trig.run = run;
    trig.run_v = run_v;
    bps_validate_configure(&trig.run_v);
    log_trigger("trigger changed");
}

/* ---- Validation reports ---- */

static void on_validated(const bps_validate_result *r, uint64_t latency_us)
{
    const bps_model_config *m = bps_validate_model();
    char sc[16], th[16];
    port_log("validation: %s, score %s %s %s (%lu ms after the trigger; front end %lu ms of which "
             "resampling %lu, model %lu ms)",
             r->ok ? (r->upload ? "upload" : "rejected") : "check failed, upload",
             centi(sc, sizeof sc, (int32_t)lroundf(r->score * 100)), r->upload ? ">=" : "<",
             centi(th, sizeof th, (int32_t)lroundf(r->threshold * 100)),
             (unsigned long)(latency_us / 1000), (unsigned long)(r->frontend_us / 1000),
             (unsigned long)(r->resample_us / 1000), (unsigned long)(r->invoke_us / 1000));
    if (!m || !r->ok)
        return;
    if (state != ST_RUN || val_txn.state != TXN_IDLE) {
        stats.reports_skipped++;
        return;
    }
    static char body[512];
    char path[96];
    bps_json j;
    bps_json_init(&j, body, sizeof body);
    bps_json_raw(&j, "{");
    bps_json_str(&j, "model", m->name);
    bps_json_fixed(&j, "score", r->score, 4);
    bps_json_fixed(&j, "threshold", r->threshold, 4);
    bps_json_bool(&j, "upload", r->upload);
    int64_t mono_ts, next;
    bool has_next;
    uint32_t next_idx;
    (void)next;
    if (epoch_set && bps_ring_locate(&ring, r->window_start, &mono_ts, &has_next, &next_idx))
        bps_json_i64(&j, "window_start", bps_clock_at(&clk, mono_ts + epoch_ns));
    bps_json_i64(&j, "window_ms", r->window_ms);
    bps_json_i64(&j, "latency_ms", (int64_t)(latency_us / 1000));
    bps_json_fixed(&j, "frontend_ms", r->frontend_us / 1000.0, 2);
    bps_json_fixed(&j, "invoke_ms", r->invoke_us / 1000.0, 2);
    bps_json_i64(&j, "arena_bytes", (int64_t)bps_validate_arena_used());
    bps_json_raw(&j, "}");
    if (j.overflow)
        return;
    path_for(path, sizeof path, "validation");
    txn_start(&val_txn, "a validation report", path, body, j.len);
}

static void report_poll(void)
{
    txn_poll(&val_txn);
    if (val_txn.state == TXN_DONE || val_txn.state == TXN_FAILED)
        txn_reset(&val_txn);
}

/* The store's size to fit the PSRAM, leaving room for a model needing
 * `need` bytes more: at most STORE_KB if set, and never so small that chunks
 * would have to stream. */
static uint32_t store_target(size_t need)
{
    uint64_t avail = (uint64_t)port_big_free_size() + store.cap;
    uint64_t hold = BIG_RESERVE + (uint64_t)need;
    if (ring_internal)
        hold += (uint64_t)ring_big_samples * 2; /* its way back to PSRAM */
    avail = avail > hold ? avail - hold : 0;
    if (cfg.store_bytes && avail > cfg.store_bytes)
        avail = cfg.store_bytes;
    if (avail > UINT32_MAX)
        avail = UINT32_MAX;
    uint32_t least = bps_upload_store_least(chunk_len);
    if (avail < least)
        avail = least;
    return (uint32_t)avail / 4096 * 4096;
}

static bool store_resize(uint32_t to, const char *why)
{
    uint32_t from = store.cap;
    if (!bps_store_resize(&store, to, port_store_realloc))
        return false;
    port_log("audio: store resized from %lu to %lu KB%s; %lu KB of PSRAM free",
             (unsigned long)from / 1024, (unsigned long)to / 1024, why,
             (unsigned long)port_big_free_size() / 1024);
    return true;
}

/* Validation, before loading a model: the store shrinks to make room (once
 * what it holds fits the smaller size). */
static bool store_room_for(size_t need)
{
    if (store_fits) {
        uint32_t to = store_target(need);
        if (to < store.cap && !store_resize(to, " to make room for the model")) {
            if (!store_waiting)
                port_log("audio: the model waits for the store to shrink to %lu KB (%lu KB held)",
                         (unsigned long)to / 1024, (unsigned long)store.used / 1024);
            store_waiting = true;
            return false;
        }
    }
    store_waiting = false;
    return port_big_free_size() >= need;
}

/* Every STORE_FIT_US: once no model is being loaded or let go, the store
 * grows into what is free (or shrinks, if something else took it). A buffer
 * that cannot grow where it is is made anew once the store is empty. */
static void store_fit_poll(uint64_t now)
{
    if (!store_fits || !capturing || now < store_fit_us)
        return;
    store_fit_us = now + STORE_FIT_US;
    if (bps_validate_settling())
        return;
    uint32_t to = store_target(0);
    if (to + STORE_FIT_STEP <= store.cap) {
        store_resize(to, "");
        return;
    }
    if (to < store.cap + STORE_FIT_STEP || store_resize(to, ""))
        return;
    if (bps_store_count(&store) || store.open)
        return;
    uint32_t from = store.cap;
    port_big_free(store.buf);
    uint8_t *buf = port_store_realloc(NULL, to);
    if (!buf)
        buf = port_store_realloc(NULL, to = from);
    store.buf = buf;
    store.cap = buf ? to : 0;
    store.used = 0;
    port_log("audio: store made anew: %lu KB (was %lu KB); %lu KB of PSRAM free",
             (unsigned long)store.cap / 1024, (unsigned long)from / 1024,
             (unsigned long)port_big_free_size() / 1024);
}

/* The store: sized to the free PSRAM where there is some, else as big as
 * asked for, or as fits (leaving the heap's reserve). */
static void start_store(void)
{
    store_fits = port_has_psram();
    store_fit_us = 0;
    store_waiting = false;
    uint8_t *buf = NULL;
    uint32_t want;
    if (store_fits) {
        want = store_target(0);
        for (; want >= STORE_MIN_BYTES && !(buf = port_store_realloc(NULL, want));
             want = want / 4 * 3 / 4096 * 4096)
            ;
        bps_store_init(&store, buf, buf ? want : 0);
        port_log("audio: %lu KB store for recordings (the PSRAM free%s); %lu KB of PSRAM free",
                 (unsigned long)store.cap / 1024, cfg.store_bytes ? ", at most STORE_KB" : "",
                 (unsigned long)port_big_free_size() / 1024);
        return;
    }
    want = cfg.store_bytes ? cfg.store_bytes : STORE_DEFAULT_BYTES;
    size_t free_bytes = port_free_heap();
    if (free_bytes < want + HEAP_RESERVE)
        want = free_bytes > HEAP_RESERVE ? (uint32_t)(free_bytes - HEAP_RESERVE) : 0;
    for (; want >= STORE_MIN_BYTES && !(buf = port_big_alloc(want)); want = want / 4 * 3)
        ;
    bps_store_init(&store, buf, buf ? want : 0);
    uint32_t asked = cfg.store_bytes ? cfg.store_bytes : STORE_DEFAULT_BYTES;
    port_log("audio: %lu KB store for recordings%s", (unsigned long)store.cap / 1024,
             store.cap < asked ? " (less than asked for: that is what fit)" : "");
}

/* Samples a ring holds besides its look-back: a gate frame, and the slack
 * the encoder may fall behind by (as start_capture() keeps clear). */
static uint32_t ring_overhead(void)
{
    uint32_t run = LOAD(&cap.min_run);
    uint32_t frame = cfg.sample_rate * gate_cfg.frame_ms / 1000 * (run ? run : 1);
    return frame + cfg.sample_rate * BPS_UPLOAD_SLACK_MS / 1000 + 4096;
}

static uint32_t whole_blocks_up(uint32_t samples)
{
    return (samples + BPS_RING_BLOCK - 1) / BPS_RING_BLOCK * BPS_RING_BLOCK;
}

static uint32_t samples_ms(uint32_t samples)
{
    return (uint32_t)((uint64_t)samples * 1000 / cfg.sample_rate);
}

/* The look-back a ring of `capacity` samples allows, at most the one set. */
static void set_lookback(uint32_t capacity)
{
    uint32_t o = ring_overhead();
    uint32_t lb = capacity > o ? capacity - o : 0;
    lookback = lb < lookback_full ? lb : lookback_full;
    bps_upload_set_lookback(lookback);
}

static void ring_refuse(bool want, uint64_t now)
{
    ring_refused = want;
    ring_retry_us = now + RING_RETRY_US;
    port_log("audio: %s", ring_note);
}

/* Moves the ring to internal RAM in low-power mode, as big as fits there
 * above INTERNAL_RESERVE (at most the full look-back's), and back to PSRAM in
 * the others. The capture core is held off meanwhile (a few ms: no audio is
 * lost), and the ring keeps its newest audio. Every loop. */
static void ring_place_poll(uint64_t now)
{
    if (!capturing)
        return;
    bool want = lp_mode(mode.run);
    if (want == ring_internal) {
        ring_refused = -1;
        ring_note[0] = '\0';
        return;
    }
    if (ring_refused == (int)want && now < ring_retry_us)
        return;
    uint32_t samples = ring_big_samples, least = ring_big_samples;
    if (want) {
        uint32_t full = whole_blocks_up(lookback_full + ring_overhead());
        uint32_t min_lb = cfg.sample_rate * LOOKBACK_MIN_MS / 1000;
        least = whole_blocks_up((min_lb < lookback_full ? min_lb : lookback_full) + ring_overhead());
        size_t internal = port_internal_free_heap();
        size_t room = internal > INTERNAL_RESERVE ? (internal - INTERNAL_RESERVE) / 2 : 0;
        samples = room < full ? (uint32_t)room / BPS_RING_BLOCK * BPS_RING_BLOCK : full;
        if (samples < least) {
            snprintf(ring_note, sizeof ring_note,
                     "no room for the ring buffer in internal RAM (%lu KB free, %lu KB needed); "
                     "it stays in PSRAM",
                     (unsigned long)internal / 1024,
                     (unsigned long)((size_t)least * 2 + INTERNAL_RESERVE) / 1024);
            ring_refuse(want, now);
            return;
        }
    }
    if (!port_audio_hold(true)) {
        snprintf(ring_note, sizeof ring_note,
                 "could not pause capture to move the ring buffer to %s; it stays",
                 want ? "internal RAM" : "PSRAM");
        ring_refuse(want, now);
        return;
    }
    /* Free RAM can be in pieces: a block fewer at a time, down to the least. */
    bool ok = false;
    for (; samples >= least && samples >= BPS_RING_BLOCK; samples -= BPS_RING_BLOCK)
        if ((ok = bps_ring_move(&ring, samples, want ? &RING_INTERNAL : &RING_BIG)))
            break;
    port_audio_hold(false);
    if (!ok) {
        snprintf(ring_note, sizeof ring_note, "could not move the ring buffer to %s; it stays",
                 want ? "internal RAM" : "PSRAM");
        ring_refuse(want, now);
        return;
    }
    ring_internal = want;
    ring_refused = -1;
    ring_note[0] = '\0';
    set_lookback(ring.capacity);
    port_log("audio: ring buffer moved to %s: %lu ms, look-back %lu ms (%lu ms set); %lu KB of "
             "internal RAM free",
             want ? "internal RAM" : "PSRAM", (unsigned long)samples_ms(ring.capacity),
             (unsigned long)samples_ms(lookback), (unsigned long)samples_ms(lookback_full),
             (unsigned long)port_internal_free_heap() / 1024);
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
    uint32_t got = bps_ring_alloc_in(&ring, cfg.sample_rate, want, &RING_BIG);
    ring_big_samples = got;
    if (got < cfg.ring_bytes / 2 / BPS_RING_BLOCK * BPS_RING_BLOCK)
        port_log("audio: %lu of the %lu KB asked for fit in the ring buffer",
                 (unsigned long)got / 512, (unsigned long)cfg.ring_bytes / 1024);
    uint32_t slack = ring_overhead();
    uint32_t max_lb = got > slack ? got - slack : 0;
    lookback = (uint32_t)((uint64_t)cfg.lookback_ms * cfg.sample_rate / 1000);
    if (lookback > max_lb) {
        port_log("audio: look-back limited to %lu ms by the ring buffer",
                 (unsigned long)samples_ms(max_lb));
        lookback = max_lb;
    }
    lookback_full = lookback;
    port_log("audio: %lu ms ring buffer, %lu ms look-back, %lu bytes free",
             (unsigned long)((uint64_t)got * 1000 / cfg.sample_rate),
             (unsigned long)((uint64_t)lookback * 1000 / cfg.sample_rate),
             (unsigned long)port_free_heap());
    start_store();
    bps_capture_init(&cap, &ring, cfg.sample_rate, cfg.mic_channel, &gate_cfg);
    bps_upload_ctx u = {
        .cfg = &cfg, .ring = &ring, .cap = &cap, .clk = &clk, .beacon = &beacon,
        .epoch_ns = &epoch_ns, .epoch_set = &epoch_set,
        .chunk_len = chunk_len, .lookback = lookback, .store = &store,
        .chunks_sent = &stats.chunks_sent, .overruns = &stats.overruns,
        .need_register = &need_register,
        .connect_server = connect_server, .path_for = path_for,
        .read_response = read_response, .centi = centi,
        .on_validated = on_validated, .chunks_rejected = &stats.chunks_rejected,
    };
    bps_upload_init(&u);
    stats.idle_mark = bps_ring_written(&ring);
    stats.kept_seen = bps_upload_kept();
    port_audio_start(cfg.sample_rate);
    capturing = true;
}

static bool apply_registration(void)
{
    const char *body = reg_txn.rx + reg_txn.resp.body_off;
    size_t len = reg_txn.rx_len - reg_txn.resp.body_off;
    double port, hb, chunk;
    if (!bps_json_number(body, len, "udp_timesync_port", &port) ||
        !bps_json_number(body, len, "heartbeat_interval_s", &hb) ||
        !bps_json_number(body, len, "chunk_secs", &chunk))
        return false;
    udp_port = (uint16_t)port;
    hb_interval_us = (uint64_t)((hb < 1 ? 1 : hb) * 1e6);
    bps_trigger_config t;
    bps_validation_config v;
    if (!bps_trigger_parse(body, len, &t))
        bps_trigger_default(&t);
    if (!bps_validation_parse(body, len, &v))
        bps_validation_default(&v);
    /* The chunk length applies from the first registration on; the trigger
     * and validation follow the server (as from heartbeat replies). */
    if (!capturing) {
        chunk_len = (uint32_t)(chunk * cfg.sample_rate);
        trigger_start(&t, &v);
        start_capture();
    } else {
        apply_trigger(&t, &v);
    }
    return true;
}

static void drop_connections(void)
{
    txn_reset(&reg_txn);
    txn_reset(&hb_txn);
    txn_reset(&pos_txn);
    txn_reset(&val_txn);
    bps_upload_drop();
    bps_spool_drop();
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
    ts.burst_left = 0;
    hb_next_us = now;
    mode.session++;
    mode.session_us = now;
    mode.replies = 0;
    mode.beacons_heard = beacon.heard;
    mode.session_synced = false;
    mode.final_hb = false;
    if (mode.run == MODE_BATCH)
        mode.phase = C_SYNC;
    mode.live_spool_us = now; /* live: the first look waits for the clock */
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
        port_log("registered with %s:%u as %s", cfg.server_host, (unsigned)cfg.server_port,
                 id_str);
        reg_delay_us = 1000000;
        start_run(now);
        return;
    }
    if (status == 409) /* the server refuses this API version, which is its major's */
        port_log("registration refused: API " BPS_API_VERSION " is not compatible with the "
                 "server's; retrying in %lu s", (unsigned long)(reg_delay_us / 1000000));
    else if (status)
        port_log("registration failed: server returned %d; retrying in %lu s", status,
                 (unsigned long)(reg_delay_us / 1000000));
    else
        port_log("registration failed: no answer from %s:%u; retrying in %lu s", cfg.server_host,
                 (unsigned)cfg.server_port, (unsigned long)(reg_delay_us / 1000000));
    reg_next_us = now + reg_delay_us;
    reg_delay_us = reg_delay_us * 2 > 30000000 ? 30000000 : reg_delay_us * 2;
}

/* ---- Network diagnostics (with BPS_NET_DEBUG) ---- */

#ifdef BPS_NET_DEBUG
/* Parts of the loop that take longer hold up everything else: log them. */
#define LOOP_SLOW_US 300000
#define NET_MAX_RTTS 128

/* Over one status interval. */
static struct {
    uint16_t rtt_idle[NET_MAX_RTTS], rtt_up[NET_MAX_RTTS]; /* time sync round trips, 0.1 ms */
    uint32_t n_idle, n_up, late, timeouts;
    uint64_t part_max_us;
    const char *part_max;
} net;
static bool net_uploading;  /* when the pending time sync request went out */
static uint64_t net_part_us; /* when the current loop part started */

static void diag_ts_sent(void) { net_uploading = !bps_upload_idle(); }

static void diag_ts_timeout(void) { net.timeouts++; }

/* A reply read `wait_us` after its request went out. */
static void diag_ts_reply(uint64_t wait_us, int64_t rtt_ns)
{
    /* Picked up after the wait: the round trip includes loop delay. */
    if (wait_us > TIMESYNC_SPIN_US)
        net.late++;
    int64_t tenths = rtt_ns / 100000;
    uint16_t v = (uint16_t)(tenths > UINT16_MAX ? UINT16_MAX : tenths < 0 ? 0 : tenths);
    if (net_uploading && net.n_up < NET_MAX_RTTS)
        net.rtt_up[net.n_up++] = v;
    else if (!net_uploading && net.n_idle < NET_MAX_RTTS)
        net.rtt_idle[net.n_idle++] = v;
}

static void diag_loop_start(void) { net_part_us = port_mono_us(); }

/* Ends one part of the loop, noting the slowest: it holds up everything else. */
static void diag_loop_part(const char *what)
{
    uint64_t us = port_mono_us() - net_part_us;
    if (us > net.part_max_us) {
        net.part_max_us = us;
        net.part_max = what;
    }
    if (us >= LOOP_SLOW_US)
        port_log("loop: %s took %lu ms", what, (unsigned long)(us / 1000));
    net_part_us = port_mono_us();
}

/* "min/median/max ms (count)" of round trips in tenths of a ms. */
static const char *rtt_summary(char *buf, size_t cap, uint16_t *v, uint32_t n)
{
    if (n == 0) {
        snprintf(buf, cap, "none");
        return buf;
    }
    for (uint32_t i = 1; i < n; i++)
        for (uint32_t j = i; j > 0 && v[j - 1] > v[j]; j--) {
            uint16_t t = v[j];
            v[j] = v[j - 1];
            v[j - 1] = t;
        }
    snprintf(buf, cap, "%u.%u/%u.%u/%u.%u ms (%lu)", v[0] / 10, v[0] % 10, v[n / 2] / 10,
             v[n / 2] % 10, v[n - 1] / 10, v[n - 1] % 10, (unsigned long)n);
    return buf;
}

static void diag_log_net(void)
{
    char idle[48], uploading[48], wifi[128];
    port_log("net: time sync round trips idle %s, during uploads %s; %lu picked up after the %lu "
             "ms wait, %lu unanswered; slowest loop part %s %lu ms",
             rtt_summary(idle, sizeof idle, net.rtt_idle, net.n_idle),
             rtt_summary(uploading, sizeof uploading, net.rtt_up, net.n_up),
             (unsigned long)net.late, (unsigned long)(TIMESYNC_SPIN_US / 1000),
             (unsigned long)net.timeouts, net.part_max ? net.part_max : "-",
             (unsigned long)(net.part_max_us / 1000));
    port_wifi_diag(wifi, sizeof wifi);
    port_log("wifi: %s, %d dBm", wifi, port_wifi_rssi());
    memset(&net, 0, sizeof net);
}
#else
static inline void diag_ts_sent(void) {}
static inline void diag_ts_timeout(void) {}
static inline void diag_ts_reply(uint64_t wait_us, int64_t rtt_ns)
{
    (void)wait_us;
    (void)rtt_ns;
}
static inline void diag_loop_start(void) {}
static inline void diag_loop_part(const char *what) { (void)what; }
static inline void diag_log_net(void) {}
#endif

/* ---- Clock measurement ---- */

static void timesync_handle(const uint8_t *buf, int n, uint64_t at_us)
{
    bps_ts_response r;
    if (!ts.waiting || !bps_ts_response_decode(buf, (size_t)n, &r) || r.seq != ts.seq)
        return; /* stale reply to an earlier, timed-out request */
    ts.waiting = false;
    ts.warned = false;
    mode.replies++;
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
    if (!bps_ts_sample_of(&r, t4, &s))
        return;
    bps_clock_add_sample(&clk, s);
    diag_ts_reply(at_us - ts.sent_us, s.rtt_ns);
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
            diag_ts_timeout();
            if (!ts.warned)
                port_log("timesync: no reply from %s:%u", cfg.server_host, (unsigned)udp_port);
            ts.warned = true;
        }
    }
    if (ts.waiting || now < ts.next_us)
        return;
    if (lp_bursts()) {
        if (ts.burst_left == 0)
            return; /* lp_poll() starts the next burst */
        ts.burst_left--;
        ts.next_us = now + LP_BURST_GAP_US;
    } else {
        ts.next_us = now - ts.next_us > TIMESYNC_INTERVAL_US ? now + TIMESYNC_INTERVAL_US
                                                             : ts.next_us + TIMESYNC_INTERVAL_US;
    }
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
    diag_ts_sent();
    if (port_udp_send(ts.h, pkt, sizeof pkt) < 0)
        return;
    ts.waiting = true;
    while (ts.waiting && port_mono_us() - ts.sent_us < TIMESYNC_SPIN_US)
        timesync_recv();
}

/* ---- Operation modes ---- */

static bool synced(void);
static bool closing_down(void);

static void radio_account(uint64_t now, bool on)
{
    if (mode.radio_on)
        mode.on_us += now - mode.on_mark_us;
    mode.radio_on = on;
    mode.on_mark_us = now;
}

static void mode_started(uint64_t now)
{
    mode.since_us = mode.on_mark_us = now;
    mode.on_us = 0;
    mode.radio_on = true;
}

/* Where the clock stands now against the server's, and the access point's. */
static bool take_anchor(uint64_t now, bps_anchor *a)
{
    const bps_ts_fit *f = bps_est_fit(&clk.est);
    if (!f || !epoch_set)
        return false;
    int64_t mono = (int64_t)now * 1000;
    memset(a, 0, sizeof *a);
    a->local_ns = local_ns(now);
    a->server_ns = bps_clock_at(&clk, a->local_ns);
    a->rate = f->rate;
    a->has_tsf = bps_beacon_ready(&beacon, mono);
    if (a->has_tsf) {
        memcpy(a->bssid, beacon.bssid, 6);
        a->tsf_ns = bps_beacon_tsf_at(&beacon, mono);
        a->tsf_rate = beacon.rate;
    }
    a->session = mode.session;
    return true;
}

static void push_anchor(uint64_t now)
{
    bps_anchor a;
    if (!mode.session_synced || !take_anchor(now, &a))
        return;
    const bps_anchor *last = bps_anchors_last(&anchors);
    if (last && last->session != a.session && mode.run == MODE_BATCH) {
        /* The first of a contact: how far the clock moved while the radio
         * was off, beyond what its drift predicted. */
        mode.has_gap = true;
        mode.gap_off_s = (uint32_t)((a.local_ns - last->local_ns) / BPS_NANOS_PER_SEC);
        mode.gap_clock_ns = bps_anchor_gap_ns(last, &a);
        mode.gap_has_tsf = bps_anchor_gap_tsf_ns(last, &a, &mode.gap_tsf_ns);
        char tsf[48] = "";
        if (mode.gap_has_tsf)
            snprintf(tsf, sizeof tsf, ", against the access point %ld us",
                     (long)(mode.gap_tsf_ns / 1000));
        port_log("contact: after %lu s, the clock was %ld us from where its drift put it%s",
                 (unsigned long)mode.gap_off_s, (long)(mode.gap_clock_ns / 1000), tsf);
    }
    bps_anchors_push(&anchors, &a);
    mode.anchor_us = now;
}

/* Once the clock has been measured afresh this session (and the access
 * point's, if it is heard), anchors can be taken. */
static void sync_poll(uint64_t now)
{
    if (mode.session_synced)
        return;
    bool heard = beacon.heard != mode.beacons_heard;
    bool tsf_ok = !heard || (bps_beacon_ready(&beacon, (int64_t)now * 1000) &&
                             beacon.fit_points >= CONTACT_SYNC_BEACONS);
    bool clock_ok = mode.replies >= CONTACT_SYNC_REPLIES && synced();
    bool late = now - mode.session_us > CONTACT_SYNC_MAX_US && mode.replies >= 3 &&
                bps_est_fit(&clk.est);
    if (!((clock_ok && tsf_ok) || late))
        return;
    if (!(clock_ok && tsf_ok))
        port_log("contact: clock still not measured well after %lu s; going on anyway",
                 (unsigned long)(CONTACT_SYNC_MAX_US / 1000000));
    mode.session_synced = true;
    push_anchor(now);
}

static bool card_has_room(void)
{
    port_sd_info info;
    port_sd_info_get(&info);
    return info.present && info.free_bytes >= BPS_RECORD_MIN_FREE;
}

/* The card is closed for a low battery (power_poll()): batch mode keeps the
 * radio off rather than going live, which would only drain the cell faster;
 * the audio meanwhile is not kept. */
static bool card_held(void)
{
    port_sd_info info;
    port_sd_info_get(&info);
    return info.held;
}

/* Turns the radio off until the next contact. */
static void go_off(uint64_t now)
{
    drop_connections();
    if (!port_wifi_off()) {
        port_log("mode: this board cannot turn its radio off; staying live");
        mode.note = "this board cannot turn its radio off";
        mode.run = MODE_LIVE;
        bps_upload_set_spool(false);
        set_state(ST_WIFI);
        return;
    }
    radio_account(now, false);
    uint64_t next = mode.contact_us + (uint64_t)mode.interval_s * 1000000;
    if (next < now + CONTACT_MIN_GAP_US)
        next = now + CONTACT_MIN_GAP_US;
    mode.next_contact_us = next;
    bps_spool_status sp;
    bps_spool_status_get(&sp);
    port_log("contact: radio off after %lu s; next contact in %lu s (%lu chunks on the card)",
             (unsigned long)((now - mode.contact_us) / 1000000),
             (unsigned long)((next - now) / 1000000), (unsigned long)sp.items);
    set_state(ST_OFF);
}

static void wake(uint64_t now)
{
    mode.contact_us = now;
    mode.phase = C_SYNC;
    radio_account(now, true);
    /* The line through the beacons from before would bridge the gap; the
     * anchors do that, from fresh lines on either side. */
    bps_beacon_forget(&beacon);
    port_wifi_begin(cfg.wifi_ssid, cfg.wifi_password, hostname);
    set_state(ST_WIFI);
}

/* A contact's steps once registered (batch mode). */
static void contact_poll(uint64_t now)
{
    if (mode.run != MODE_BATCH || closing_down())
        return; /* for an update the radio stays on */
    bool over = now - mode.contact_us > CONTACT_MAX_US;
    if (!card_has_room() && !card_held()) {
        /* Audio would be lost with the radio off: upload it as it comes. */
        port_log("mode: the storage card is gone or full; live until it is back");
        mode.note = "batch mode needs a storage card with room";
        mode.run = MODE_LIVE;
        bps_upload_set_spool(false);
        mode_started(now);
        hb_next_us = now;
        return;
    }
    switch (mode.phase) {
    case C_SYNC:
        if (!mode.session_synced && !over)
            return;
        bps_spool_kick();
        mode.phase = C_FLUSH;
        break;
    case C_FLUSH:
        if (bps_spool_busy() && !over)
            return;
        /* What was saved during the contact needs an anchor after it. */
        push_anchor(now);
        bps_spool_kick();
        mode.phase = C_LAST_FLUSH;
        break;
    case C_LAST_FLUSH:
        if (bps_spool_busy() && !over)
            return;
        bps_spool_drop();
        push_anchor(now); /* the last before the radio goes off */
        mode.phase = C_FINAL;
        hb_next_us = now; /* say so, and hear whether the mode changed */
        break;
    case C_FINAL:
        /* Waits for that heartbeat (heartbeat_poll), however it goes. */
        if (now - mode.contact_us > CONTACT_MAX_US + 2 * TXN_TIMEOUT_US)
            go_off(now);
        break;
    }
}

/* A contact that cannot get as far as registering waits for the next. */
static void contact_connect_poll(uint64_t now)
{
    if (mode.run == MODE_BATCH && !closing_down() && now - mode.contact_us > CONTACT_CONNECT_US) {
        port_log("contact: could not reach the server; trying again at the next contact");
        go_off(now);
    }
}

/* The mode the server asks for, from a heartbeat reply. */
static void apply_mode(int want, uint32_t interval_s, uint64_t now)
{
    if (want == mode.run && interval_s == mode.interval_s && !mode.note)
        return;
    if (want == MODE_BATCH && interval_s != mode.interval_s && mode.run == MODE_BATCH)
        port_log("mode: batch, now a contact every %lu s", (unsigned long)interval_s);
    mode.want = want;
    mode.interval_s = interval_s;
    if (want != MODE_BATCH) {
        const char *note = NULL;
        if (lp_mode(want) && !lp_mode(mode.run)) {
            if (port_wifi_power(PORT_WIFI_AWAKE)) {
                lp.level = PORT_WIFI_AWAKE;
            } else {
                want = MODE_LIVE;
                note = "this board cannot save power on its radio";
                if (!mode.note)
                    port_log("mode: low power is not supported on this board; staying live");
            }
        }
        if (want == MODE_ULTRA_LOW) {
            /* Its recordings wait in the store: one must hold whole chunks. */
            bps_upload_status us;
            bps_upload_status_get(&us);
            if (us.streaming || !us.cap) {
                want = MODE_LOW_POWER;
                note = "ultra-low-power mode needs PSRAM to keep recordings: low power instead";
                if (!mode.note)
                    port_log("mode: ultra-low-power needs a store that holds whole recordings; "
                             "low power instead");
            }
        }
        mode.note = note;
        if (mode.run == want)
            return;
        if (mode.run == MODE_BATCH) {
            bps_upload_set_spool(false);
            bps_spool_kick(); /* what is still on the card */
        }
        port_log(want == MODE_LIVE        ? "mode: live (the radio stays on)"
                 : want == MODE_LOW_POWER ? "mode: low-power live (the radio sleeps between clock "
                                            "measurements, heartbeats and uploads)"
                                          : "mode: ultra-low-power (the radio wakes every 5 min "
                                            "for the clock, a heartbeat and the recordings since)");
        mode.run = want;
        mode_started(now);
        return;
    }
    if (mode.run == MODE_BATCH)
        return;
    if (!card_has_room() && !card_held()) {
        if (!mode.note)
            port_log("mode: batch needs a storage card with room; staying live");
        mode.note = "batch mode needs a storage card with room";
        return;
    }
    mode.note = NULL;
    port_log("mode: batch, a contact every %lu s (the radio is off in between)",
             (unsigned long)interval_s);
    mode.run = MODE_BATCH;
    bps_upload_set_spool(true);
    mode_started(now);
    /* This connection is the first contact. */
    mode.contact_us = now;
    mode.phase = C_SYNC;
}

/* ---- Maintenance: commands from the server ----
 *
 * Heartbeat replies carry commands (a reboot, trace logging), each with an id
 * the server gives in increasing order, until the client acknowledges it in a
 * heartbeat. A command runs once: only ids above the highest run so far,
 * which the board keeps across restarts (port_keep_command()), so a reply
 * replayed or repeated, or the reboot command still in the first reply after
 * the reboot, runs nothing again. A reboot is acknowledged after the restart,
 * with the reason, `requested`. */

#define MAX_ACKS 4
#define MAX_COMMANDS 8
/* A reboot lets the upload in progress (or the pass over the card's stored
 * chunks) finish for this long, then gives it up; then waits this long at
 * most for the card's queue to be written. */
#define REBOOT_UPLOAD_WAIT_US 15000000
#define REBOOT_CARD_WAIT_US 5000000
#define LOG_MAX_S 86400
/* The most of a heartbeat the trace log takes, and how soon the next
 * heartbeat goes while lines are left over. */
#define LOG_HB_BYTES 2048
#define LOG_DRAIN_US 1000000

static const char *const BOOT_NAMES[] = {"unknown",  "power_on", "requested", "watchdog",
                                         "brownout", "panic",    "other"};

typedef struct {
    int64_t id;
    const char *result; /* "done", "unsupported" */
} ack;

static struct {
    int64_t last_run; /* the highest command id run */
    ack acks[MAX_ACKS];  /* to send */
    uint32_t n_acks, acks_sending;
    ack answered[MAX_ACKS]; /* the latest answers, to repeat if asked again */
    uint32_t n_answered;
    int boot_reason;
    bool boot_reported, boot_sending;
    bool rebooting; /* closing down: to restart, or (`updating`) to install an update */
    bool updating;
    int reboot_phase; /* 0: letting an upload finish; 1: closing the card; 2: restarting */
    uint64_t reboot_us;
    uint64_t log_until_us;
    bps_log_batch log_sending;
} maint;

/* "1762950000123", without printf's 64-bit support. */
static const char *id_text(char out[24], int64_t v)
{
    char d[24];
    int n = 0;
    uint64_t u = v < 0 ? 0 : (uint64_t)v;
    do {
        d[n++] = (char)('0' + u % 10);
        u /= 10;
    } while (u);
    for (int i = 0; i < n; i++)
        out[i] = d[n - 1 - i];
    out[n] = 0;
    return out;
}

static void ack_add(int64_t id, const char *result)
{
    for (uint32_t i = 0; i < maint.n_acks; i++)
        if (maint.acks[i].id == id)
            return;
    if (maint.n_acks == MAX_ACKS) { /* the oldest goes: it is asked again if still pending */
        memmove(maint.acks, maint.acks + 1, sizeof maint.acks[0] * (MAX_ACKS - 1));
        maint.n_acks--;
        if (maint.acks_sending)
            maint.acks_sending--;
    }
    maint.acks[maint.n_acks++] = (ack){id, result};
}

static void remember_answer(int64_t id, const char *result)
{
    if (maint.n_answered == MAX_ACKS) {
        memmove(maint.answered, maint.answered + 1, sizeof maint.answered[0] * (MAX_ACKS - 1));
        maint.n_answered--;
    }
    maint.answered[maint.n_answered++] = (ack){id, result};
    ack_add(id, result);
}

/* A command is run: it is never run again, even after a restart. */
static void command_ran(int64_t id, const char *result, bool restarting)
{
    maint.last_run = id;
    port_keep_command(id, restarting);
    if (!restarting)
        remember_answer(id, result);
}

static void maint_setup(void)
{
    port_boot b;
    memset(&maint, 0, sizeof maint);
    port_boot_info(&b);
    maint.boot_reason = b.reason >= 0 && b.reason <= BPS_BOOT_OTHER ? b.reason : BPS_BOOT_UNKNOWN;
    maint.last_run = b.last_command > 0 ? b.last_command : 0;
    char id[24];
    if (maint.boot_reason == BPS_BOOT_REQUESTED && maint.last_run) {
        port_log("boot: restarted as the server asked (command %s)", id_text(id, maint.last_run));
        remember_answer(maint.last_run, "done");
    } else {
        port_log("boot: started (%s)", BOOT_NAMES[maint.boot_reason]);
    }
}

static void logging_set(bool on, uint32_t secs, uint64_t now)
{
    if (on) {
        if (secs < 1)
            secs = 1;
        if (secs > LOG_MAX_S)
            secs = LOG_MAX_S;
        maint.log_until_us = now + (uint64_t)secs * 1000000;
        bps_log_set_capture(true);
        port_log("trace logging: on for %lu s", (unsigned long)secs);
    } else if (bps_log_capturing()) {
        port_log("trace logging: off");
        bps_log_set_capture(false);
    }
}

/* The trace log turns itself off when its time is up, whether or not the
 * server is heard from. */
static void logging_poll(uint64_t now)
{
    if (bps_log_capturing() && now >= maint.log_until_us) {
        port_log("trace logging: time is up; off");
        bps_log_set_capture(false);
    }
}

/* The low-battery cutoff for the storage card (power.h): checked once a
 * second (the gauge reads every 10 s), in every state, on or off the air. */
#define POWER_POLL_US 1000000
static struct {
    bps_power p;
    uint64_t next_us;
} power;

static void power_poll(uint64_t now)
{
    if (now < power.next_us)
        return;
    power.next_us = now + POWER_POLL_US;
    const bps_power_cfg pc = {(uint32_t)(cfg.sd_cut_mv > 0 ? cfg.sd_cut_mv : 0),
                              (uint32_t)(cfg.sd_resume_mv > 0 ? cfg.sd_resume_mv : 0)};
    uint32_t mv = 0;
    bool have = bps_devices_cell_mv(&mv);
    switch (bps_power_update(&power.p, &pc, have, mv)) {
    case BPS_POWER_WENT_LOW:
        port_log("power: battery %lu mV, under %lu: closing the storage card until it is back "
                 "at %lu mV",
                 (unsigned long)mv, (unsigned long)pc.cut_mv, (unsigned long)pc.resume_mv);
        port_sd_hold(true);
        break;
    case BPS_POWER_RECOVERED:
        if (have)
            port_log("power: battery %lu mV: opening the storage card again", (unsigned long)mv);
        else
            port_log("power: no battery now: opening the storage card again");
        port_sd_hold(false);
        break;
    }
}

static void reboot_start(int64_t id, uint64_t now)
{
    char t[24];
    command_ran(id, "done", true);
    maint.rebooting = true;
    maint.reboot_phase = 0;
    maint.reboot_us = now;
    port_log("reboot: asked by the server (command %s); restarting once the upload in progress "
             "is done",
             id_text(t, id));
}

/* One command of a heartbeat reply: the object at `obj`. */
static void command_run(const char *obj, size_t len, int64_t id, uint64_t now)
{
    char cmd[24], t[24];
    if (id <= maint.last_run) {
        /* Run already: the server has not had the answer (or this is a
         * reply replayed). Answered again, unless restarting for it. */
        if (maint.rebooting)
            return;
        const char *result = "done";
        for (uint32_t i = 0; i < maint.n_answered; i++)
            if (maint.answered[i].id == id)
                result = maint.answered[i].result;
        ack_add(id, result);
        return;
    }
    if (!bps_json_string(obj, len, "command", cmd, sizeof cmd))
        cmd[0] = 0;
    if (strcmp(cmd, "reboot") == 0) {
        reboot_start(id, now);
    } else if (strcmp(cmd, "logging") == 0) {
        char level[16] = "on";
        double secs = 0;
        bps_json_string(obj, len, "log_level", level, sizeof level);
        bps_json_number(obj, len, "log_duration_s", &secs);
        /* One level here: any but "off" captures everything. */
        bool on = strcmp(level, "off") != 0;
        logging_set(on, secs < 1 ? 1 : secs > LOG_MAX_S ? LOG_MAX_S : (uint32_t)secs, now);
        command_ran(id, "done", false);
    } else {
        port_log("command %s: '%s' is not supported by this firmware", id_text(t, id), cmd);
        command_ran(id, "unsupported", false);
    }
}

/* The commands in a heartbeat reply, by increasing id. */
static void commands_reply(const char *body, size_t len, uint64_t now)
{
    struct {
        int64_t id;
        size_t obj, len;
    } c[MAX_COMMANDS];
    int n = 0;
    size_t it = 0, obj, olen;
    while (n < MAX_COMMANDS && bps_json_array_next(body, len, "commands", &it, &obj, &olen)) {
        int64_t id;
        if (!bps_json_int(body + obj, olen, "command_id", &id) || id <= 0)
            continue;
        int k = n++;
        for (; k > 0 && c[k - 1].id > id; k--)
            c[k] = c[k - 1];
        c[k].id = id;
        c[k].obj = obj;
        c[k].len = olen;
    }
    for (int i = 0; i < n && !maint.rebooting; i++)
        command_run(body + c[i].obj, c[i].len, c[i].id, now);
}

static bool ota_install(uint64_t now);

/* Restarting or updating: the radio stays on, and no new work starts. */
static bool closing_down(void) { return maint.rebooting; }

/* Restarting, or installing an update, once asked: true while the client is
 * closing down, and the rest of the loop is left alone. */
static bool reboot_poll(uint64_t now)
{
    if (!maint.rebooting)
        return false;
    const char *what = maint.updating ? "update" : "reboot";
    if (maint.reboot_phase == 0) {
        /* Recordings in the store would be lost: they go first (if they can). */
        bool busy = !bps_upload_idle() || bps_spool_busy();
        if (busy && now - maint.reboot_us < REBOOT_UPLOAD_WAIT_US)
            return false; /* the loop goes on meanwhile */
        if (busy)
            port_log("%s: giving up the upload in progress", what);
        drop_connections();
        port_sd_close();
        maint.reboot_phase = 1;
        maint.reboot_us = now;
    }
    if (maint.reboot_phase == 1) {
        if (!port_sd_idle() && now - maint.reboot_us < REBOOT_CARD_WAIT_US)
            return true;
        if (!port_sd_idle())
            port_log("%s: the SD card's queue is still being written; going on anyway", what);
        if (maint.updating) {
            if (ota_install(now))
                maint.reboot_phase = 2; /* restarting into the new firmware */
            return true;
        }
        port_log("reboot: restarting");
        maint.reboot_phase = 2;
        port_restart();
    }
    return true;
}

/* ---- Firmware updates over the air ----
 *
 * Heartbeat replies offer another build while the server wants this client
 * to run it (ota.h). The client takes the offer unless its battery is low
 * (then it says so, `deferred`, and the server offers again), closes down as
 * for a reboot (the upload in progress, the card), and the board downloads
 * the build into its other app slot and restarts into it (port_ota_update()).
 * A new build runs on trial: it confirms itself once registered with audio
 * flowing; if the board restarts before that, the bootloader goes back to the
 * old one, which then reports `rolled_back`. A failed download is reported
 * `failed`. Each status goes in every heartbeat until one is delivered. */

static struct {
    bool supported;
    bool trial; /* running build `trial_id`, which has yet to confirm itself */
    int64_t trial_id;
    bps_ota_offer offer; /* being installed */
    /* The status to report, and which of them the heartbeat in flight has. */
    bool has_status;
    uint32_t gen, sending_gen;
    int64_t status_id;
    char state[16];
    char detail[BPS_OTA_DETAIL_MAX + 1];
    /* An offer of the build whose failure was just delivered is not taken
     * (the server stops offering it once it has the failure). */
    int64_t skip_id;
    /* The latest deferral logged. */
    int64_t deferred_id;
    char deferred[32];
} ota;

static bool ota_advertised(void) { return ota.supported; }

static void ota_status(int64_t id, const char *state, const char *detail)
{
    ota.has_status = true;
    ota.gen++;
    ota.status_id = id;
    snprintf(ota.state, sizeof ota.state, "%s", state);
    snprintf(ota.detail, sizeof ota.detail, "%s", detail ? detail : "");
}

static void ota_setup(void)
{
    char t[24];
    memset(&ota, 0, sizeof ota);
    ota.supported = port_ota_supported();
    if (!ota.supported)
        return;
    port_ota_state st;
    port_ota_boot_info(&st);
    if (st.rolled_back) {
        char detail[BPS_OTA_DETAIL_MAX + 1];
        snprintf(detail, sizeof detail, "did not confirm itself; back on %s", cfg.version);
        port_log("update: build %s did not confirm itself; the board went back to this firmware",
                 id_text(t, st.build_id));
        ota_status(st.build_id, "rolled_back", detail);
    } else if (st.trial) {
        ota.trial = true;
        ota.trial_id = st.build_id;
        port_log("update: running build %s on trial; it is kept once registered with audio "
                 "flowing",
                 id_text(t, st.build_id));
    }
}

/* Once registered with audio flowing, a new build is kept. */
static void ota_confirm_poll(void)
{
    if (!ota.trial || state != ST_RUN || !capturing ||
        bps_ring_written(&ring) < cfg.sample_rate)
        return;
    char t[24];
    port_ota_confirm();
    ota.trial = false;
    port_log("update: build %s is registered with audio flowing; keeping it",
             id_text(t, ota.trial_id));
}

static void ota_json(bps_json *j)
{
    ota.sending_gen = ota.has_status ? ota.gen : 0;
    if (ota.has_status)
        bps_ota_status_json(j, ota.status_id, ota.state, ota.detail);
}

/* The server has the heartbeat (2xx). */
static void ota_delivered(void)
{
    if (!ota.has_status || !ota.sending_gen || ota.sending_gen != ota.gen)
        return;
    ota.has_status = false;
    if (strcmp(ota.state, "deferred") != 0)
        ota.skip_id = ota.status_id;
    if (strcmp(ota.state, "rolled_back") == 0)
        port_ota_forget();
}

/* An offer in a heartbeat reply. */
static void ota_reply(const char *body, size_t len, uint64_t now)
{
    int64_t skip = ota.skip_id;
    ota.skip_id = 0;
    bps_ota_offer o;
    if (!bps_ota_offer_parse(body, len, &o))
        return;
    if (!ota.supported || maint.rebooting || o.build_id == skip ||
        (ota.trial && o.build_id == ota.trial_id))
        return;
    char t[24], detail[32];
    bps_battery_reading b = {0};
    int32_t rate = 0;
    b.present = bps_devices_battery(&b.centi_percent, &rate);
    b.charging = rate > 0;
    if (!bps_ota_battery_ok(&b, cfg.ota_min_battery, detail, sizeof detail)) {
        if (o.build_id != ota.deferred_id || strcmp(detail, ota.deferred) != 0)
            port_log("update: build %s (%s) offered; not now: %s, under the %d%% needed",
                     id_text(t, o.build_id), o.describe[0] ? o.describe : "?", detail,
                     cfg.ota_min_battery);
        ota.deferred_id = o.build_id;
        snprintf(ota.deferred, sizeof ota.deferred, "%s", detail);
        ota_status(o.build_id, "deferred", detail);
        return;
    }
    ota.deferred_id = 0;
    ota.offer = o;
    maint.rebooting = true;
    maint.updating = true;
    maint.reboot_phase = 0;
    maint.reboot_us = now;
    port_log("update: build %s (%s, %ld bytes) offered; installing once the upload in progress "
             "is done",
             id_text(t, o.build_id), o.describe[0] ? o.describe : "?", (long)o.size);
}

/* Downloads and installs the offered build: true if the board is restarting
 * into it, else the client goes on with this firmware. */
static bool ota_install(uint64_t now)
{
    char t[24], err[BPS_OTA_DETAIL_MAX + 1] = "";
    const bps_ota_offer *o = &ota.offer;
    if (!port_wifi_up()) {
        port_log("update: Wi-Fi is down; build %s waits for a later offer",
                 id_text(t, o->build_id));
    } else {
        port_log("update: downloading build %s from %s:%u%s", id_text(t, o->build_id),
                 cfg.server_host, (unsigned)cfg.server_port, o->url);
        if (port_ota_update(cfg.server_host, cfg.server_port, o->url, o->build_id, err,
                            sizeof err))
            return true;
        if (!err[0])
            snprintf(err, sizeof err, "update failed");
        port_log("update: build %s failed: %s", id_text(t, o->build_id), err);
        ota_status(o->build_id, "failed", err);
    }
    /* Back to work: connect again (a batch contact starts over). */
    maint.rebooting = false;
    maint.updating = false;
    maint.reboot_phase = 0;
    drop_connections();
    if (mode.run == MODE_BATCH)
        mode.contact_us = now;
    set_state(ST_WIFI);
    return false;
}

/* What the heartbeat says of maintenance: acknowledgements, why the board
 * started (until delivered), and the trace log. Last in the heartbeat: the
 * log's lines take what room is left. */
static void maint_json(bps_json *j, uint64_t now)
{
    maint.acks_sending = maint.n_acks;
    if (maint.n_acks) {
        char t[24];
        bps_json_arr(j, "acks");
        for (uint32_t i = 0; i < maint.n_acks; i++) {
            if (i)
                bps_json_raw(j, ",");
            bps_json_raw(j, "{\"command_id\":");
            bps_json_raw(j, id_text(t, maint.acks[i].id));
            bps_json_str(j, "result", maint.acks[i].result);
            bps_json_raw(j, "}");
        }
        bps_json_raw(j, "]");
    }
    maint.boot_sending = !maint.boot_reported;
    if (maint.boot_sending) {
        bps_json_str(j, "reboot_reason", BOOT_NAMES[maint.boot_reason]);
    }
    memset(&maint.log_sending, 0, sizeof maint.log_sending);
    bool on = bps_log_capturing();
    /* The log object's own fields, and the heartbeat's end. */
    if ((!on && !bps_log_pending()) || j->overflow || j->cap - j->len < 96)
        return;
    bps_json_obj(j, "log");
    bps_json_str(j, "level", on ? "on" : "off");
    if (on) {
        uint64_t left = maint.log_until_us > now ? maint.log_until_us - now : 0;
        bps_json_i64(j, "remaining_s", (int64_t)((left + 999999) / 1000000));
    }
    bps_json_i64(j, "now_ms", (int64_t)(now / 1000));
    size_t room = j->cap - j->len, end = 1 + 1 + 1; /* "}" "}" and the NUL */
    size_t budget = room > end ? room - end : 0;
    bps_log_json(j, budget < LOG_HB_BYTES ? budget : LOG_HB_BYTES, now / 1000, &maint.log_sending);
    bps_json_raw(j, "}");
}

/* The server has the heartbeat (2xx). */
static void maint_delivered(uint64_t now)
{
    uint32_t n = maint.acks_sending < maint.n_acks ? maint.acks_sending : maint.n_acks;
    memmove(maint.acks, maint.acks + n, sizeof maint.acks[0] * (maint.n_acks - n));
    maint.n_acks -= n;
    maint.acks_sending = 0;
    if (maint.boot_sending)
        maint.boot_reported = true;
    bps_log_delivered(&maint.log_sending);
    if (maint.log_sending.valid && maint.log_sending.more && now + LOG_DRAIN_US < hb_next_us)
        hb_next_us = now + LOG_DRAIN_US; /* more lines are waiting */
    memset(&maint.log_sending, 0, sizeof maint.log_sending);
}

static void heartbeat_reply(uint64_t now)
{
    const char *body = hb_txn.rx + hb_txn.resp.body_off;
    size_t len = hb_txn.rx_len - hb_txn.resp.body_off;
    bps_trigger_config t = trig.want;
    bps_validation_config v = trig.want_v;
    bool has_t = bps_trigger_parse(body, len, &t), has_v = bps_validation_parse(body, len, &v);
    if (capturing && (has_t || has_v))
        apply_trigger(&t, &v);
    commands_reply(body, len, now);
    ota_reply(body, len, now);
    char m[16];
    double interval = mode.interval_s;
    if (!bps_json_string(body, len, "mode", m, sizeof m))
        return; /* a server without modes */
    bps_json_number(body, len, "interval_s", &interval);
    if (interval < 30)
        interval = 30;
    if (interval > 3600)
        interval = 3600;
    int want = strcmp(m, "batch") == 0             ? MODE_BATCH
               : strcmp(m, "low_power") == 0       ? MODE_LOW_POWER
               : strcmp(m, "ultra_low_power") == 0 ? MODE_ULTRA_LOW
                                                   : MODE_LIVE;
    apply_mode(want, (uint32_t)interval, now);
}

/* Live modes: chunks left on the card (from batch mode) go up as well. */
static void live_spool_poll(uint64_t now)
{
    if (mode.run == MODE_BATCH || !mode.session_synced || bps_spool_busy() ||
        now < mode.live_spool_us)
        return;
    mode.live_spool_us = now + LIVE_SPOOL_US;
    bps_spool_status sp;
    bps_spool_status_get(&sp);
    if (!sp.known || sp.items)
        bps_spool_kick();
}

static void anchor_poll(uint64_t now)
{
    if (mode.session_synced && now - mode.anchor_us >= ANCHOR_EVERY_US)
        push_anchor(now);
}

/* ---- Low-power live mode ---- */

static void lp_account(uint64_t now, bool awake)
{
    if (lp.awake)
        lp.awake_us += now - lp.mark_us;
    else if (awake)
        lp.wakes++;
    lp.awake = awake;
    lp.mark_us = now;
}

static double lp_awake_pct(uint64_t now)
{
    uint64_t awake = lp.awake_us + (lp.awake ? now - lp.mark_us : 0);
    uint64_t all = now - lp.since_us;
    return all ? 100.0 * (double)awake / (double)all : 100.0;
}

/* Whether the radio must be awake now: a clock burst, a request or upload
 * on its way (or a stored recording ready to go), or the client closing down
 * (an update or reboot). */
static bool lp_busy(uint64_t now)
{
    return ts.burst_left > 0 || ts.waiting || hb_txn.state != TXN_IDLE ||
           reg_txn.state != TXN_IDLE || pos_txn.state != TXN_IDLE || val_txn.state != TXN_IDLE ||
           bps_upload_sending() || bps_upload_ready(now) || bps_spool_busy() || closing_down();
}

/* Sets the CPU's speed and the radio's power saving for the mode, and starts
 * each clock burst. Every loop, in every state. Until the session's clock is
 * measured (as live mode does, every TIMESYNC_INTERVAL_US), and while
 * joining or registering, the radio stays awake. */
/* Ultra-low-power mode's settings: the gate's run, the clock's window and
 * (with the run) the look-back. */
static void ulp_settings(bool ultra, uint64_t now)
{
    lp.ultra = ultra;
    lp.flush = false;
    STORE(&cap.min_run, ultra ? ULP_MIN_RUN : 1u);
    clk.est.window_ns = ultra ? ULP_CLOCK_WINDOW_NS : BPS_CLOCK_WINDOW_NS;
    if (capturing)
        set_lookback(ring.capacity);
    if (ultra)
        lp.next_burst_us = now; /* a wake now, then every ULP_WAKE_US */
}

/* Ultra-low-power mode: stored recordings go up in a wake, until none is
 * ready; sooner once the store is filling, or when the client closes down
 * (a reboot or update waits for the store). Otherwise they wait. */
static void ulp_flush_poll(uint64_t now, bool held)
{
    if (!lp.ultra || !held) {
        bps_upload_hold(false);
        return;
    }
    bps_upload_status us;
    bps_upload_status_get(&us);
    if (!lp.flush && (closing_down() || (uint64_t)us.bytes * 100 >= (uint64_t)us.cap * ULP_FLUSH_PCT ||
                      us.queued >= BPS_STORE_ITEMS * 3 / 4))
        lp.flush = true;
    bps_upload_hold(false);
    if (lp.flush && !bps_upload_sending() && !bps_upload_ready(now))
        lp.flush = false;
    bps_upload_hold(!lp.flush);
}

static void lp_poll(uint64_t now)
{
    bool on = lp_mode(mode.run);
    bool ultra = mode.run == MODE_ULTRA_LOW;
    if (ultra != lp.ultra)
        ulp_settings(ultra, now);
    if (on != lp.on) {
        lp.on = on;
        port_cpu_low(on);
        if (on) {
            lp.since_us = lp.mark_us = now;
            lp.awake_us = 0;
            lp.awake = true;
            lp.bursts = lp.wakes = 0;
            lp.next_burst_us = now;
        }
    }
    int level = PORT_WIFI_FULL;
    if (on) {
        level = PORT_WIFI_AWAKE;
        if (state == ST_RUN && lp_bursts()) {
            uint64_t every = ultra ? ULP_WAKE_US : LP_BURST_EVERY_US;
            if (now >= lp.next_burst_us) {
                lp.next_burst_us = now + every;
                lp.bursts++;
                ts.burst_left = ultra ? ULP_BURST_REQUESTS : LP_BURST_REQUESTS;
                ts.next_us = now + LP_SETTLE_US;
                /* A heartbeat due before the next burst goes in this wake. */
                if (hb_next_us < now + every / 2)
                    hb_next_us = now;
                lp.flush = true; /* and what is stored */
            }
            ulp_flush_poll(now, true);
            if (!lp_busy(now))
                level = PORT_WIFI_SAVE;
        } else {
            ulp_flush_poll(now, false);
        }
    } else {
        ulp_flush_poll(now, false);
    }
    if (level != lp.level) {
        port_wifi_power(level);
        lp.level = level;
    }
    if (on && (level == PORT_WIFI_SAVE) == lp.awake)
        lp_account(now, level != PORT_WIFI_SAVE);
}

static void mode_json(bps_json *j, uint64_t now)
{
    bps_spool_status sp;
    bps_spool_status_get(&sp);
    bps_json_obj(j, "mode");
    bps_json_str(j, "mode", MODE_NAMES[mode.run]);
    bps_json_i64(j, "interval_s", mode.interval_s);
    bps_json_i64(j, "backlog", sp.items);
    bps_json_i64(j, "backlog_bytes", (int64_t)sp.bytes);
    if (mode.run == MODE_BATCH) {
        /* Everything before the oldest chunk still on the card, or (none)
         * before a chunk starting now would start, has been delivered. */
        int64_t local = sp.has_oldest
                            ? sp.oldest_local_ns
                            : local_ns(now) - bps_samples_to_ns(lookback + chunk_len / 4,
                                                                cfg.sample_rate);
        bps_json_i64(j, "delivered_through", bps_clock_at(&clk, local));
        if (mode.phase == C_FINAL) {
            uint64_t next = mode.contact_us + (uint64_t)mode.interval_s * 1000000;
            uint64_t in = next > now + CONTACT_MIN_GAP_US ? next - now : CONTACT_MIN_GAP_US;
            bps_json_i64(j, "next_contact_s", (int64_t)(in / 1000000));
        }
        uint64_t on = mode.on_us + (mode.radio_on ? now - mode.on_mark_us : 0);
        uint64_t all = now - mode.since_us;
        bps_json_fixed(j, "radio_on_pct", all ? 100.0 * (double)on / (double)all : 100.0, 1);
        if (mode.has_gap) {
            bps_json_obj(j, "gap");
            bps_json_i64(j, "off_s", mode.gap_off_s);
            bps_json_i64(j, "clock_ns", mode.gap_clock_ns);
            if (mode.gap_has_tsf) {
                bps_json_i64(j, "tsf_ns", mode.gap_tsf_ns);
            }
            bps_json_raw(j, "}");
        }
    }
    if (lp_mode(mode.run) && lp.on)
        bps_json_fixed(j, "radio_on_pct", lp_awake_pct(now), 1);
    if (mode.run == MODE_ULTRA_LOW) {
        /* Recordings wait for a wake: everything before the oldest still
         * held, or (none) before a chunk starting now would start, is in. */
        int64_t through;
        if (!bps_upload_oldest_start(&through))
            through = bps_clock_at(&clk, local_ns(now) - bps_samples_to_ns(lookback + chunk_len / 4,
                                                                           cfg.sample_rate));
        bps_json_i64(j, "delivered_through", through);
    }
    if (capturing) {
        /* Where the ring buffer is, and the look-back it leaves room for. */
        bps_json_str(j, "ring_memory", ring_internal ? "internal" : "psram");
        bps_json_i64(j, "ring_ms", samples_ms(ring.capacity));
        bps_json_i64(j, "lookback_ms", samples_ms(lookback));
        bps_json_i64(j, "lookback_set_ms", samples_ms(lookback_full));
        bps_json_i64(j, "internal_free_bytes", (int64_t)port_internal_free_heap());
        bps_json_i64(j, "store_bytes", store.cap);
    }
    if (mode.note) {
        bps_json_str(j, "note", mode.note);
    }
    bps_json_raw(j, "}");
}

/* ---- Heartbeats ---- */

static uint32_t dropouts_total(void) { return LOAD(&cap.dropouts) + stats.overruns; }

static void start_heartbeat(uint64_t now)
{
    char path[96];
    bps_json j;
    bps_json_init(&j, hb_tx + HB_HEAD_ROOM, sizeof hb_tx - HB_HEAD_ROOM);
    bps_json_raw(&j, "{");
    bps_json_key(&j, "clock");
    bps_clock_status st;
    if (bps_clock_status_take(&clk, local_ns(now), &st)) {
        bps_json_raw(&j, "{");
        bps_json_i64(&j, "offset_ns", st.offset_ns);
        bps_json_i64(&j, "rtt_ns", st.rtt_ns);
        bps_json_i64(&j, "measured_at", st.measured_at);
        bps_json_i64(&j, "error_ns", st.error_ns);
        bps_json_obj(&j, "detail");
        bps_json_bool(&j, "corrects_timestamps", st.corrects_timestamps);
        bps_json_i64(&j, "clock_offset_ns", st.clock_offset_ns);
        bps_json_fixed(&j, "drift_ppm", st.drift_ppm, 4);
        bps_json_i64(&j, "fit_points", st.fit_points);
        bps_json_i64(&j, "requests", st.requests);
        bps_json_i64(&j, "replies", st.replies);
        const char *keys[3] = {"rtt_min_ns", "rtt_median_ns", "rtt_max_ns"};
        int64_t vals[3] = {st.rtt_min_ns, st.rtt_median_ns, st.rtt_max_ns};
        for (int i = 0; i < 3; i++) {
            bps_json_key(&j, keys[i]);
            if (st.has_rtts)
                bps_json_i64(&j, NULL, vals[i]);
            else
                bps_json_raw(&j, "null");
        }
        bps_json_raw(&j, "}");
        /* The access point's clock against the server's, both now: what the
         * server pools across the AP's clients to convert its TSF. */
        const bps_ts_fit *f = bps_est_fit(&clk.est);
        int64_t mono = (int64_t)now * 1000;
        if (f && clk.corrects && bps_beacon_ready(&beacon, mono)) {
            char bssid[18];
            bps_beacon_bssid(&beacon, bssid);
            bps_json_obj(&j, "beacon");
            bps_json_str(&j, "bssid", bssid);
            bps_json_i64(&j, "tsf_ns", bps_beacon_tsf_at(&beacon, mono));
            bps_json_i64(&j, "server_ns", bps_clock_at(&clk, mono + epoch_ns));
            bps_json_i64(&j, "server_error_ns", f->error_ns);
            bps_json_i64(&j, "scatter_ns", beacon.scatter_ns);
            bps_json_fixed(&j, "drift_ppm", beacon.rate * -1e6, 4);
            bps_json_i64(&j, "beacons", beacon.fit_points);
            bps_json_raw(&j, "}");
        }
        bps_json_raw(&j, "}");
    } else {
        bps_json_raw(&j, "null");
    }
    stats.dropouts_sending = dropouts_total();
    bps_json_i64(&j, "uptime_s", (int64_t)((now - boot_us) / 1000000));
    bps_json_i64(&j, "chunks_sent", (int64_t)stats.chunks_sent);
    bps_json_i64(&j, "chunks_gated", (int64_t)stats.chunks_gated);
    bps_json_i64(&j, "queue_len", 0);
    bps_json_i64(&j, "capture_dropouts", stats.dropouts_sending - stats.dropouts_reported);
    bps_devices_heartbeat(&j);
    mode_json(&j, now);
    if (capturing) {
        bps_validation_config running;
        bps_validate_running(&running);
        bps_trigger_json(&j, &trig.run);
        bps_validation_json(&j, &running);
        const char *note = bps_validate_note();
        if (note) {
            bps_json_str(&j, "validation_note", note);
        }
        bps_json_i64(&j, "chunks_rejected", (int64_t)stats.chunks_rejected);
    }
    ota_json(&j);
    maint_json(&j, now);
    bps_json_raw(&j, "}");
    mode.final_hb = mode.run == MODE_BATCH && mode.phase == C_FINAL;
    if (j.overflow)
        port_log("heartbeat: too long for its buffer; not sent");
    path_for(path, sizeof path, "heartbeat");
    txn_start_at(&hb_txn, "a heartbeat", path, HB_HEAD_ROOM, j.overflow ? 0 : j.len);
}

static void heartbeat_poll(uint64_t now)
{
    if (hb_txn.state == TXN_IDLE && now >= hb_next_us) {
        uint64_t period = mode.run == MODE_ULTRA_LOW   ? ULP_WAKE_US
                          : mode.run == MODE_LOW_POWER ? LP_HEARTBEAT_US
                                                       : hb_interval_us;
        hb_next_us = now - hb_next_us > period ? now + period : hb_next_us + period;
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
        maint_delivered(now);
        ota_delivered();
        heartbeat_reply(now);
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
    /* The contact's last heartbeat: the radio goes off, unless the reply
     * asked for live mode. */
    if (mode.final_hb) {
        mode.final_hb = false;
        if (mode.run == MODE_BATCH && mode.phase == C_FINAL && !need_register && !closing_down())
            go_off(now);
    }
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

/* ---- Gated chunks ---- */

/* Silent stretches count as gated chunks, so the stats compare with bsp-client's.
 * Each is corrected too, as bsp-client corrects before gating: the clock status
 * then describes the latest chunk, not one sent hours ago, which the current
 * fit (a minute of measurements) cannot be extrapolated back to. */
static void count_gated(void)
{
    if (chunk_len == 0)
        return;
    /* A chunk being encoded may yet be rejected, or given up: once it is,
     * its stretch counts from where counting stopped. One kept is not gated. */
    if (bps_upload_encoding())
        return;
    if (bps_upload_kept() != stats.kept_seen) {
        stats.kept_seen = bps_upload_kept();
        stats.idle_mark = bps_upload_end();
    }
    uint32_t w = bps_ring_written(&ring);
    while (bps_idx_diff(w, stats.idle_mark) >= (int32_t)chunk_len) {
        int64_t mono_ts;
        bool has_next;
        uint32_t next_idx;
        if (bps_ring_locate(&ring, stats.idle_mark, &mono_ts, &has_next, &next_idx))
            bps_clock_correct(&clk, mono_ts + epoch_ns,
                              bps_samples_to_ns(chunk_len, cfg.sample_rate));
        stats.chunks_gated++;
        stats.idle_mark += chunk_len;
    }
}

/* Chunks that leave the ring unpolled (the network is down: count_gated() and
 * uploads wait for ST_RUN) count as gated as they go, as count_gated() would
 * later; a mark left 2^31 samples behind (12.4 h at 48 kHz) would compare as
 * the future and stop the count for as long again. Uploads' marks likewise. */
static void keep_marks_recent(void)
{
    if (!capturing)
        return;
    /* Past what count_gated() leaves behind, in ST_RUN, by a chunk. */
    if (!bps_upload_encoding())
        stats.chunks_gated +=
            bps_ring_catch_up(&ring, &stats.idle_mark, chunk_len, ring.capacity + chunk_len);
    bps_upload_keep_recent();
}

/* Chunks are encoded (into the store, or onto the card in batch mode)
 * whatever the network does: every state. */
static void audio_poll(uint64_t now)
{
    if (capturing) {
        bps_upload_poll(now);
        count_gated();
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
    if (state == ST_OFF || (state == ST_RUN && lp_bursts()))
        on = ms % 10000 < 20; /* a glimmer now and then: the LED costs power */
    else if (state != ST_RUN)
        on = (ms / 250) % 2;
    else if (!synced())
        on = ms % 1000 < 100;
    else if (!bps_upload_idle())
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
    /* Each jump's size: whole DMA buffers lost, or a timeline that was off? */
    static uint32_t dropouts_logged;
    uint32_t d = LOAD(&cap.dropouts);
    if (d != dropouts_logged) {
        dropouts_logged = d;
        long us = (long)LOAD(&cap.predict_ns) / 1000;
        port_log("capture: timeline jumped %ld us %s", us < 0 ? -us : us,
                 us >= 0 ? "late (samples missing)" : "early");
    }
    if (now < status_next_us)
        return;
    status_next_us = now + STATUS_LOG_US;
    const bps_ts_fit *f = bps_est_fit(&clk.est);
    /* The error is half the best round trip plus the scatter around the fit. */
    port_log("status: %s, clock error %ld us (best round trip %ld us, scatter %ld us), drift %s "
             "ppm, sample rate %s ppm (timeline scatter %ld ns, last point %ld ns off), %lu sent, "
             "%lu gated, %lu rejected, %lu dropouts, %lu bytes free (lowest %lu)",
             synced() ? "synced" : "not synced", f ? (long)(f->error_ns / 1000) : -1L,
             f ? (long)(f->rtt_ns / 1000) : -1L,
             f ? (long)((f->error_ns - f->rtt_ns / 2) / 1000) : -1L,
             centi(a, sizeof a, f ? (int32_t)(bps_fit_drift_ppm(f) * 100) : 0),
             centi(b, sizeof b, LOAD(&cap.rate_err_ppb) / 10000), (long)LOAD(&cap.scatter_ns),
             (long)LOAD(&cap.predict_ns),
             (unsigned long)stats.chunks_sent, (unsigned long)stats.chunks_gated,
             (unsigned long)stats.chunks_rejected, (unsigned long)dropouts_total(),
             (unsigned long)port_free_heap(),
             (unsigned long)heap_low);
    char devices[256];
    bps_devices_describe(devices, sizeof devices);
    if (devices[0])
        port_log("devices: %s", devices);
    /* Power saving, for the trace log (a health metric of its own later). */
    char pw[192], lpi[128] = "", bat[64] = "";
    port_power_diag(pw, sizeof pw);
    if (lp.on)
        snprintf(lpi, sizeof lpi, "; radio awake %s%% of %lu s, %lu wakes, %lu clock bursts",
                 centi(a, sizeof a, (int32_t)lround(lp_awake_pct(now) * 100)),
                 (unsigned long)((now - lp.since_us) / 1000000), (unsigned long)lp.wakes,
                 (unsigned long)lp.bursts);
    int32_t pct, rate;
    uint32_t mv;
    if (bps_devices_battery(&pct, &rate) && bps_devices_cell_mv(&mv))
        snprintf(bat, sizeof bat, "; battery %lu mV, %s%%, %s %%/h", (unsigned long)mv,
                 centi(a, sizeof a, pct), centi(b, sizeof b, rate));
    port_log("power status: mode %s; %s; led %s; ring buffer in %s (%lu ms, look-back %lu ms of "
             "%lu; internal RAM %lu KB free)%s%s%s%s",
             MODE_NAMES[mode.run], pw[0] ? pw : "no board details",
             state == ST_OFF || (state == ST_RUN && lp_bursts()) ? "glimmer" : "on/blinking",
             ring_internal ? "internal RAM" : "PSRAM", (unsigned long)samples_ms(ring.capacity),
             (unsigned long)samples_ms(lookback), (unsigned long)samples_ms(lookback_full),
             (unsigned long)port_internal_free_heap() / 1024, ring_note[0] ? ": " : "", ring_note,
             lpi, bat);
    if (capturing) {
        bps_upload_status us;
        bps_upload_status_get(&us);
        char oldest[48] = "";
        if (us.oldest_us)
            snprintf(oldest, sizeof oldest, ", the oldest stored %lu s ago",
                     (unsigned long)((now - us.oldest_us) / 1000000));
        port_log("store: %lu chunks, %lu of %lu KB (at most %lu KB so far)%s; %lu dropped unsent%s",
                 (unsigned long)us.queued, (unsigned long)us.bytes / 1024,
                 (unsigned long)us.cap / 1024, (unsigned long)us.peak_bytes / 1024, oldest,
                 (unsigned long)us.dropped,
                 us.streaming ? "; too small for a chunk: chunks stream as encoded" : "");
    }
    if (mode.run == MODE_BATCH) {
        bps_spool_status sp;
        bps_spool_status_get(&sp);
        uint64_t on = mode.on_us + (mode.radio_on ? now - mode.on_mark_us : 0);
        uint64_t all = now - mode.since_us;
        port_log("mode: batch, a contact every %lu s; radio on %lu%% of the time; %lu chunks on "
                 "the card (%lu waiting for the clock), %lu not saved",
                 (unsigned long)mode.interval_s, (unsigned long)(all ? on * 100 / all : 100),
                 (unsigned long)sp.items, (unsigned long)sp.waiting, (unsigned long)sp.skipped);
    }
    if (bps_beacon_ready(&beacon, (int64_t)now * 1000)) {
        char bssid[18];
        bps_beacon_bssid(&beacon, bssid);
        port_log("beacons: following %s, %lu in the line, scatter %ld ns, drift %s ppm against "
                 "it, %lu restarts",
                 bssid, (unsigned long)beacon.fit_points, (long)beacon.scatter_ns,
                 centi(a, sizeof a, (int32_t)lround(beacon.rate * -1e8)),
                 (unsigned long)beacon.restarts);
    }
    if (state == ST_RUN)
        diag_log_net();
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
    /* Named by the id's bytes 3-5: on an ESP32, the last half of the MAC, the part that
     * differs from board to board (boards from one batch share the first four bytes). */
    snprintf(hostname, sizeof hostname, "bps-%s-%02x%02x%02x", cfg.board, client_id[3],
             client_id[4], client_id[5]);
    bps_clock_init(&clk, true);
    bps_anchors_init(&anchors);
    bps_spool_ctx sc = {
        .cfg = &cfg, .anchors = &anchors, .epoch_ns = &epoch_ns,
        .chunks_sent = &stats.chunks_sent, .need_register = &need_register,
        .connect_server = connect_server, .path_for = path_for, .read_response = read_response,
    };
    bps_spool_init(&sc);
    mode_started(boot_us);
    bps_validate_init(&ring, cfg.sample_rate);
    bps_validate_set_room(store_room_for);
    reg_txn.h = hb_txn.h = pos_txn.h = val_txn.h = -1;
    val_txn.tx = val_tx, val_txn.tx_cap = sizeof val_tx;
    reg_txn.tx = reg_tx, reg_txn.tx_cap = sizeof reg_tx;
    hb_txn.tx = hb_tx, hb_txn.tx_cap = sizeof hb_tx;
    pos_txn.tx = pos_tx, pos_txn.tx_cap = sizeof pos_tx;
    val_txn.rx = val_rx, val_txn.rx_cap = sizeof val_rx;
    reg_txn.rx = reg_rx, reg_txn.rx_cap = sizeof reg_rx;
    hb_txn.rx = hb_rx, hb_txn.rx_cap = sizeof hb_rx;
    pos_txn.rx = pos_rx, pos_txn.rx_cap = sizeof pos_rx;
    port_log("bps client %s on %s, id %s, %lu bytes free", cfg.version, cfg.board, id_str,
             (unsigned long)port_free_heap());
    if (cfg.build && cfg.build->info)
        port_log("build: %s", cfg.build->info);
    else if (cfg.build)
        port_log("build: %s, commit %s%s, built %s, for %s", cfg.build->describe,
                 cfg.build->commit, cfg.build->dirty ? " (dirty)" : "", cfg.build->built_at,
                 cfg.build->hardware_id);
    maint_setup();
    ota_setup();
    bps_devices_setup(&cfg.buses);
    port_wifi_begin(cfg.wifi_ssid, cfg.wifi_password, hostname);
    set_state(ST_WIFI);
}

void bps_client_loop(void)
{
    uint64_t now = port_mono_us();
    if (reboot_poll(now))
        return;
    logging_poll(now);
    power_poll(now);
    port_beacon pb;
    for (int i = 0; i < 16 && port_beacon_take(&pb); i++)
        bps_beacon_push(&beacon, pb.bssid, pb.tsf_us, pb.mono_ns);
    keep_marks_recent();
    if (state != ST_WIFI && state != ST_OFF && !port_wifi_up()) {
        port_log("wifi: disconnected");
        drop_connections();
        set_state(ST_WIFI);
    }
    switch (state) {
    case ST_WIFI:
        audio_poll(now);
        if (port_wifi_up()) {
            char wifi[128];
            port_wifi_diag(wifi, sizeof wifi);
            port_log("wifi: connected (%s, %d dBm)", wifi, port_wifi_rssi());
            reg_next_us = now;
            reg_delay_us = 1000000;
            set_state(ST_REGISTER);
        } else if (now - state_since_us > WIFI_RETRY_US) {
            port_log("wifi: still not connected to \"%s\"; retrying", cfg.wifi_ssid);
            port_wifi_begin(cfg.wifi_ssid, cfg.wifi_password, hostname);
            state_since_us = now;
        }
        if (capturing)
            contact_connect_poll(now);
        break;
    case ST_REGISTER:
        audio_poll(now);
        register_poll(now);
        if (state == ST_REGISTER)
            contact_connect_poll(now);
        break;
    case ST_RUN:
        diag_loop_start();
        timesync_poll(now);
        diag_loop_part("time sync");
        heartbeat_poll(now);
        diag_loop_part("heartbeat");
        position_poll(now);
        diag_loop_part("positioning");
        audio_poll(now);
        bps_upload_send_poll(now);
        diag_loop_part("upload");
        sync_poll(now);
        anchor_poll(now);
        /* The card's reads are one at a time: the spool's pass and a model
         * loading take turns (card.h). */
        live_spool_poll(now);
        bps_spool_poll(now);
        report_poll();
        ota_confirm_poll();
        diag_loop_part("stored uploads");
        if (state == ST_RUN)
            contact_poll(now);
        if (need_register && state == ST_RUN) {
            need_register = false;
            drop_connections();
            reg_next_us = now;
            set_state(ST_REGISTER);
        }
        break;
    case ST_OFF:
        /* Chunks are saved on the card meanwhile. */
        audio_poll(now);
        /* Early if the card went: what is recorded cannot be kept. */
        if (now >= mode.next_contact_us || (!card_has_room() && !card_held()))
            wake(now);
        break;
    }
    if (capturing)
        bps_validate_poll(now);
    bps_devices_poll(now);
    lp_poll(now);
    ring_place_poll(now);
    store_fit_poll(now);
    update_led(now);
    log_status(now);
    if (state == ST_OFF || (lp.on && lp.level == PORT_WIFI_SAVE))
        port_idle_ms(OFF_IDLE_MS);
}
