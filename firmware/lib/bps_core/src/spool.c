#include "spool.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "port.h"
#include "record.h"

/* An upload making no progress for this long is given up. */
#define STALL_US 10000000
#define ANSWER_TIMEOUT_US 10000000
/* A chunk's .json is far shorter. */
#define META_MAX 512
/* A .flac this old without its .json was cut short by a restart. */
#define ORPHAN_AGE_NS (60 * BPS_NANOS_PER_SEC)

static bps_spool_ctx c;

static struct {
    bool known, orphans_checked;
    uint32_t items, saved_since_list, skipped;
    uint64_t bytes;
    bool has_new_oldest; /* a chunk saved since the latest listing */
    int64_t new_oldest;
} st;

void bps_spool_init(const bps_spool_ctx *ctx) { c = *ctx; }

void bps_spool_name(int64_t local_ns, char out[24])
{
    snprintf(out, 24, "%lld", (long long)(local_ns / BPS_NANOS_PER_MS));
}

static void path_of(char *out, size_t cap, const char *name, const char *ext)
{
    snprintf(out, cap, "%s/%s.%s", BPS_SPOOL_DIR, name, ext);
}

/* ---- Saving ---- */

static struct {
    bool active;
    char name[24], path[48];
    int64_t start_local, end_local, start_est, end_est;
    uint32_t sample_rate;
    uint64_t bytes, dropped;
} w;

static void w_write(const void *buf, size_t len, bool append)
{
    if (port_sd_write(w.path, buf, len, append))
        w.bytes += len;
    else
        w.dropped += len;
}

bool bps_spool_begin(int64_t start_local, int64_t end_local, int64_t start_est, int64_t end_est,
                     uint32_t sample_rate, uint32_t samples, const uint8_t *head, size_t len)
{
    (void)samples;
    w.active = false;
    port_sd_info info;
    port_sd_info_get(&info);
    if (!info.present || info.free_bytes < BPS_RECORD_MIN_FREE) {
        st.skipped++;
        return false;
    }
    bps_spool_name(start_local, w.name);
    path_of(w.path, sizeof w.path, w.name, "flac");
    w.start_local = start_local;
    w.end_local = end_local;
    w.start_est = start_est;
    w.end_est = end_est;
    w.sample_rate = sample_rate;
    w.bytes = w.dropped = 0;
    w.active = true;
    w_write(head, len, false);
    return true;
}

void bps_spool_frame(const uint8_t *frame, size_t len)
{
    if (w.active)
        w_write(frame, len, true);
}

void bps_spool_end(uint32_t samples, uint32_t lost)
{
    if (!w.active)
        return;
    w.active = false;
    port_sd_close();
    char line[384], path[48];
    bps_json j;
    bps_json_init(&j, line, sizeof line);
    bps_json_raw(&j, "{");
    bps_json_i64(&j, "epoch_ns", *c.epoch_ns);
    bps_json_i64(&j, "start", w.start_local);
    bps_json_i64(&j, "end", w.end_local);
    bps_json_i64(&j, "start_est", w.start_est);
    bps_json_i64(&j, "end_est", w.end_est);
    bps_json_i64(&j, "sample_rate", w.sample_rate);
    bps_json_i64(&j, "samples", samples);
    bps_json_i64(&j, "lost", lost);
    bps_json_i64(&j, "bytes", (int64_t)w.bytes);
    bps_json_i64(&j, "dropped", (int64_t)w.dropped);
    bps_json_raw(&j, "}\n");
    path_of(path, sizeof path, w.name, "json");
    if (j.overflow || !port_sd_write(path, line, j.len, false)) {
        port_sd_remove(w.path);
        st.skipped++;
        return;
    }
    port_sd_close();
    st.items++;
    st.saved_since_list++;
    st.bytes += w.bytes;
    if (!st.has_new_oldest) {
        st.has_new_oldest = true;
        st.new_oldest = w.start_local;
    }
}

void bps_spool_abort(void)
{
    if (!w.active)
        return;
    w.active = false;
    port_sd_close();
    port_sd_remove(w.path);
}

/* ---- Uploading ---- */

enum { F_IDLE, F_LISTING, F_ORPHANS, F_NEXT, F_META, F_HEAD, F_READING, F_DATA, F_RESP };

static struct {
    int state, h;
    bool again; /* kicked during a pass: make another */
    char list[PORT_SD_READ_MAX + 1];
    size_t pos;
    uint32_t total, listed;
    /* The chunk being uploaded. */
    char name[24];
    int64_t start_local;
    uint64_t bytes, off;
    const uint8_t *data;
    size_t data_len, data_off;
    char head[640];
    size_t head_len, head_off;
    char rx[256];
    size_t rx_len;
    bps_http_response resp;
    uint64_t progress_us, deadline_us;
    /* This pass. */
    uint64_t pass_us;
    uint32_t sent, refused, waiting;
    uint64_t sent_bytes;
    bool has_waiting_oldest, failing;
    int64_t waiting_oldest;
} f = {.h = -1};

static void remove_chunk(const char *name, uint64_t bytes)
{
    char path[48];
    path_of(path, sizeof path, name, "flac");
    port_sd_remove(path); /* first: a .json left alone is cleared up later */
    path_of(path, sizeof path, name, "json");
    port_sd_remove(path);
    if (st.items)
        st.items--;
    st.bytes = st.bytes > bytes ? st.bytes - bytes : 0;
}

static void close_conn(void)
{
    if (f.h >= 0)
        port_tcp_close(f.h);
    f.h = -1;
}

static void note_waiting(int64_t start_local)
{
    f.waiting++;
    if (!f.has_waiting_oldest || start_local < f.waiting_oldest) {
        f.has_waiting_oldest = true;
        f.waiting_oldest = start_local;
    }
}

static void list(void)
{
    if (!port_sd_list(BPS_SPOOL_DIR, ".json")) {
        f.state = F_IDLE;
        return;
    }
    /* Everything saved from here on is not in the listing. */
    st.saved_since_list = 0;
    st.has_new_oldest = false;
    f.has_waiting_oldest = false;
    f.waiting = 0;
    f.state = F_LISTING;
}

/* Once a boot, with a complete listing of the .json files in hand: lists the
 * .flac files, to remove those a restart left without one. */
static bool check_orphans(void)
{
    if (st.orphans_checked || f.failing || f.listed < f.total)
        return false;
    st.orphans_checked = true;
    if (!port_sd_list(BPS_SPOOL_DIR, ".flac"))
        return false;
    f.state = F_ORPHANS;
    return true;
}

static void remove_orphans(const char *names, size_t len)
{
    int64_t now_local = (int64_t)port_mono_us() * 1000 + *c.epoch_ns;
    uint32_t removed = 0;
    char json[32], path[48];
    for (size_t i = 0; i < len;) {
        const char *s = names + i;
        const char *nl = memchr(s, '\n', len - i);
        size_t n = nl ? (size_t)(nl - s) : len - i;
        i += n + 1;
        if (n <= 5 || n - 5 + 6 > sizeof json)
            continue;
        snprintf(json, sizeof json, "%.*s.json\n", (int)(n - 5), s);
        /* Listed, being written, or too recent to tell: not an orphan. */
        bool listed = strstr(f.list, json) != NULL &&
                      (strstr(f.list, json) == f.list || strstr(f.list, json)[-1] == '\n');
        if (listed || (w.active && strncmp(w.name, s, n - 5) == 0 && !w.name[n - 5]) ||
            strtoll(s, NULL, 10) * BPS_NANOS_PER_MS > now_local - ORPHAN_AGE_NS)
            continue;
        snprintf(path, sizeof path, "%s/%.*s", BPS_SPOOL_DIR, (int)n, s);
        port_sd_remove(path);
        removed++;
    }
    if (removed)
        port_log("spool: removed %lu chunks cut short by a restart", (unsigned long)removed);
}

static void pass_done(void)
{
    bool progressed = f.sent > 0;
    if (f.sent || f.refused)
        port_log("spool: uploaded %lu chunks (%lu KB) in %lu ms%s; %lu left on the card",
                 (unsigned long)f.sent, (unsigned long)(f.sent_bytes / 1024),
                 (unsigned long)((port_mono_us() - f.pass_us) / 1000),
                 f.refused ? ", the server refused some (deleted)" : "",
                 (unsigned long)st.items);
    f.sent = f.refused = 0;
    f.sent_bytes = 0;
    f.pass_us = port_mono_us();
    if (check_orphans())
        return;
    /* The listing missed some (it was full): list again once some are gone. */
    if (!f.failing && ((f.listed < f.total && progressed) || f.again)) {
        f.again = false;
        list();
        return;
    }
    f.state = F_IDLE;
}

static void fail(const char *why)
{
    port_log("spool: upload of %s failed (%s); trying again later", f.name, why);
    close_conn();
    note_waiting(f.start_local);
    f.failing = true;
    pass_done();
}

/* Puts the chunk on the server's clock, and its access point's: from the
 * anchors either side if it was recorded since this boot, else (they are
 * gone) the estimate made when it was recorded. False while there are no
 * anchors after it yet. */
static bool stamps(const char *meta, size_t len, int64_t *s0, int64_t *s1, char *tsf, size_t cap)
{
    int64_t epoch, l0, l1, e0, e1;
    if (!bps_json_int(meta, len, "epoch_ns", &epoch) || !bps_json_int(meta, len, "start", &l0) ||
        !bps_json_int(meta, len, "end", &l1) || !bps_json_int(meta, len, "start_est", &e0) ||
        !bps_json_int(meta, len, "end_est", &e1))
        return false;
    tsf[0] = 0;
    const bps_anchor *first = bps_anchors_first(c.anchors);
    if (epoch != *c.epoch_ns || !first || l0 < first->local_ns) {
        *s0 = e0;
        *s1 = e1;
        return true;
    }
    if (!bps_anchors_server(c.anchors, l0, s0) || !bps_anchors_server(c.anchors, l1, s1))
        return false;
    uint8_t b0[6], b1[6];
    int64_t t0, t1;
    if (bps_anchors_tsf(c.anchors, l0, b0, &t0) && bps_anchors_tsf(c.anchors, l1, b1, &t1) &&
        memcmp(b0, b1, 6) == 0)
        snprintf(tsf, cap, "%02x:%02x:%02x:%02x:%02x:%02x %lld %lld", b0[0], b0[1], b0[2], b0[3],
                 b0[4], b0[5], (long long)t0, (long long)t1);
    return true;
}

/* The next name in the listing, or false at its end. */
static bool next_name(void)
{
    while (f.list[f.pos]) {
        const char *s = f.list + f.pos;
        const char *nl = strchr(s, '\n');
        size_t n = nl ? (size_t)(nl - s) : strlen(s);
        f.pos += n + (nl != NULL);
        /* "<start>.json" */
        if (n <= 5 || n - 5 >= sizeof f.name || memcmp(s + n - 5, ".json", 5) != 0)
            continue;
        memcpy(f.name, s, n - 5);
        f.name[n - 5] = 0;
        return true;
    }
    return false;
}

/* Looks at the chunk's .json: uploads it, leaves it for later, or (when it
 * is damaged) removes it. */
static void start_chunk(const char *meta, size_t len)
{
    int64_t bytes, dropped = 0, s0, s1;
    f.start_local = strtoll(f.name, NULL, 10) * BPS_NANOS_PER_MS;
    bps_json_int(meta, len, "start", &f.start_local);
    bps_json_int(meta, len, "dropped", &dropped);
    if (!bps_json_int(meta, len, "bytes", &bytes) || bytes <= 0 || dropped > 0) {
        port_log("spool: %s is incomplete; deleted", f.name);
        remove_chunk(f.name, 0);
        f.state = F_NEXT;
        return;
    }
    char tsf[80], extra[192], path[96];
    if (!stamps(meta, len, &s0, &s1, tsf, sizeof tsf)) {
        note_waiting(f.start_local);
        f.state = F_NEXT;
        return;
    }
    bps_json x;
    bps_json_init(&x, extra, sizeof extra);
    bps_json_raw(&x, BPS_HEADER_START_NS ": ");
    bps_json_i64(&x, NULL, s0);
    bps_json_raw(&x, "\r\n" BPS_HEADER_END_NS ": ");
    bps_json_i64(&x, NULL, s1);
    bps_json_raw(&x, "\r\n");
    if (tsf[0]) {
        bps_json_raw(&x, BPS_HEADER_TSF ": ");
        bps_json_raw(&x, tsf);
        bps_json_raw(&x, "\r\n");
    }
    c.path_for(path, sizeof path, "audio");
    f.head_len = x.overflow ? 0
                            : bps_http_head(f.head, sizeof f.head, "POST", c.cfg->server_host,
                                            c.cfg->server_port, path, "audio/flac",
                                            (size_t)bytes, extra);
    if (!f.head_len) {
        remove_chunk(f.name, 0);
        f.state = F_NEXT;
        return;
    }
    f.h = c.connect_server("a stored upload");
    if (f.h < 0) {
        fail("cannot connect");
        return;
    }
    f.bytes = (uint64_t)bytes;
    f.off = 0;
    f.head_off = 0;
    f.rx_len = 0;
    f.progress_us = port_mono_us();
    f.state = F_HEAD;
}

static bool read_next(void)
{
    char path[48];
    path_of(path, sizeof path, f.name, "flac");
    uint64_t left = f.bytes - f.off;
    if (!port_sd_read(path, (uint32_t)f.off, left < PORT_SD_READ_MAX ? (size_t)left : PORT_SD_READ_MAX))
        return false;
    f.state = F_READING;
    return true;
}

/* Writes what is pending of `buf`; false once the connection failed. */
static bool send_some(const void *buf, size_t len, size_t *off, uint64_t now)
{
    while (*off < len) {
        int n = port_tcp_write(f.h, (const uint8_t *)buf + *off, len - *off);
        if (n < 0)
            return false;
        if (n == 0)
            break;
        *off += (size_t)n;
        f.progress_us = now;
    }
    return true;
}

static void finish_chunk(int r)
{
    close_conn();
    int status = r > 0 ? f.resp.status : 0;
    if (status >= 200 && status < 300) {
        (*c.chunks_sent)++;
        f.sent++;
        f.sent_bytes += f.bytes;
        remove_chunk(f.name, f.bytes);
    } else if (status == 404) {
        port_log("server forgot this client; re-registering");
        *c.need_register = true;
        note_waiting(f.start_local);
        f.failing = true;
        f.state = F_IDLE;
        return;
    } else if (status >= 400 && status < 500) {
        port_log("spool: the server refused %s (%d); deleted", f.name, status);
        f.refused++;
        remove_chunk(f.name, f.bytes);
    } else {
        fail(status ? "server error" : "no answer");
        return;
    }
    f.state = F_NEXT;
}

/* One step of the pass; true if it got anywhere. */
static bool step(uint64_t now)
{
    const void *data;
    uint32_t total;
    int r;
    switch (f.state) {
    case F_LISTING:
        r = port_sd_result(&data, &total);
        if (r == PORT_SD_PENDING)
            return false;
        f.pos = 0;
        f.list[0] = 0;
        f.total = f.listed = 0;
        if (r > 0) {
            memcpy(f.list, data, (size_t)r);
            f.list[r] = 0;
            f.total = total;
            for (int i = 0; i < r; i++)
                f.listed += f.list[i] == '\n';
        }
        if (!st.known && f.total)
            port_log("spool: %lu chunks on the card from before", (unsigned long)f.total);
        st.known = true;
        st.items = f.total + st.saved_since_list;
        f.state = F_NEXT;
        return true;
    case F_ORPHANS:
        r = port_sd_result(&data, &total);
        if (r == PORT_SD_PENDING)
            return false;
        if (r > 0)
            remove_orphans(data, (size_t)r);
        f.state = F_NEXT; /* at the end of the listing: the pass ends */
        return true;
    case F_NEXT:
        if (!next_name()) {
            pass_done();
            return true;
        }
        {
            char path[48];
            path_of(path, sizeof path, f.name, "json");
            if (!port_sd_read(path, 0, META_MAX)) {
                f.state = F_IDLE;
                return false;
            }
        }
        f.state = F_META;
        return true;
    case F_META:
        r = port_sd_result(&data, &total);
        if (r == PORT_SD_PENDING)
            return false;
        if (r <= 0) {
            remove_chunk(f.name, 0);
            f.state = F_NEXT;
        } else {
            static char meta[META_MAX + 1]; /* not on the stack: a Pico has little */
            memcpy(meta, data, (size_t)r);
            meta[r] = 0;
            start_chunk(meta, (size_t)r);
        }
        return true;
    case F_HEAD:
        if (!send_some(f.head, f.head_len, &f.head_off, now)) {
            fail("connection lost");
            return true;
        }
        if (f.head_off < f.head_len)
            break;
        if (!read_next()) {
            fail("card busy");
            return true;
        }
        return true;
    case F_READING:
        r = port_sd_result(&data, &total);
        if (r == PORT_SD_PENDING)
            break;
        if (r <= 0) {
            /* Shorter than its .json says: it cannot be sent whole. */
            close_conn();
            port_log("spool: %s cannot be read; deleted", f.name);
            remove_chunk(f.name, 0);
            f.state = F_NEXT;
            return true;
        }
        f.data = data;
        f.data_len = (size_t)r;
        f.data_off = 0;
        f.state = F_DATA;
        return true;
    case F_DATA:
        if (!send_some(f.data, f.data_len, &f.data_off, now)) {
            fail("connection lost");
            return true;
        }
        if (f.data_off < f.data_len)
            break;
        f.off += f.data_len;
        if (f.off >= f.bytes) {
            f.state = F_RESP;
            f.deadline_us = now + ANSWER_TIMEOUT_US;
            return true;
        }
        if (!read_next()) {
            fail("card busy");
            return true;
        }
        return true;
    case F_RESP:
        r = c.read_response(f.h, f.rx, sizeof f.rx, &f.rx_len, &f.resp);
        if (r == 0 && now < f.deadline_us)
            return false;
        finish_chunk(r);
        return true;
    default:
        return false;
    }
    if (now - f.progress_us > STALL_US)
        fail("stalled");
    return false;
}

void bps_spool_kick(void)
{
    if (f.state != F_IDLE) {
        f.again = true;
        return;
    }
    f.again = false;
    f.failing = false;
    f.sent = f.refused = 0;
    f.sent_bytes = 0;
    f.pass_us = port_mono_us();
    list();
}

void bps_spool_poll(uint64_t now)
{
    for (int i = 0; i < 8 && f.state != F_IDLE && step(now); i++)
        ;
}

bool bps_spool_busy(void) { return f.state != F_IDLE; }

void bps_spool_drop(void)
{
    if (f.state == F_IDLE)
        return;
    close_conn();
    if (f.state >= F_META)
        note_waiting(f.start_local);
    f.state = F_IDLE;
    f.again = false;
}

void bps_spool_status_get(bps_spool_status *out)
{
    memset(out, 0, sizeof *out);
    out->known = st.known;
    out->items = st.items;
    out->bytes = st.bytes;
    out->waiting = f.waiting;
    out->skipped = st.skipped;
    out->failing = f.failing;
    /* The earliest still to go: left waiting this pass, not reached yet, the
     * one going now, saved since the listing, or being saved. */
    bool has = f.has_waiting_oldest;
    int64_t oldest = f.waiting_oldest;
#define CONSIDER(t)                                                                                \
    do {                                                                                           \
        int64_t t_ = (t);                                                                          \
        if (!has || t_ < oldest) {                                                                 \
            has = true;                                                                            \
            oldest = t_;                                                                           \
        }                                                                                          \
    } while (0)
    for (const char *s = f.list + f.pos; *s;) {
        CONSIDER(strtoll(s, NULL, 10) * BPS_NANOS_PER_MS);
        const char *nl = strchr(s, '\n');
        if (!nl)
            break;
        s = nl + 1;
    }
    if (f.state >= F_HEAD)
        CONSIDER(f.start_local);
    if (st.has_new_oldest)
        CONSIDER(st.new_oldest);
    if (w.active)
        CONSIDER(w.start_local);
#undef CONSIDER
    out->has_oldest = has;
    out->oldest_local_ns = oldest;
}
