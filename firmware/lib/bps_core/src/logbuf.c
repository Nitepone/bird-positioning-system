#include "logbuf.h"

#include <string.h>

#include "port.h"

#define SIZE BPS_LOG_RING_BYTES
#define MASK (SIZE - 1u)
/* Each line: its length (2 bytes), its time in ms (4 bytes, low 32 bits),
 * then its text. */
#define HDR 6
/* A producer that finds the ring locked tries this often before dropping. */
#define TRIES 64

_Static_assert((SIZE & MASK) == 0 && SIZE >= 1024, "BPS_LOG_RING_BYTES: a power of two");
_Static_assert(HDR + BPS_LOG_LINE_MAX < SIZE / 2, "a line must fit in the ring");

static uint8_t ring[SIZE];
/* Under the lock: free-running byte positions, and line numbers, of the next
 * line to write and the oldest kept. */
static uint32_t head, tail, head_seq, tail_seq;
/* Lines dropped because the ring was locked. Counted without the lock, as a
 * plain load and store: the Pico's cores have no atomic read-modify-write, so
 * two producers dropping at the same instant may count once. */
static uint32_t contended;
static bool on;
/* The reader's own (the client's loop): what has been delivered. */
static uint32_t acked_seq, acked_pos, contended_acked;

static void ring_put(uint32_t pos, const uint8_t *src, size_t n)
{
    for (size_t i = 0; i < n; i++)
        ring[(pos + i) & MASK] = src[i];
}

static uint8_t ring_at(uint32_t pos) { return ring[pos & MASK]; }

/* What a logged byte is kept as: printable ASCII as is. */
static uint8_t clean(uint8_t c)
{
    if (c >= 0x80)
        return '?';
    if (c < 0x20 || c == 0x7f)
        return ' ';
    return c;
}

void bps_log_capture(uint64_t mono_us, const char *text)
{
    if (!__atomic_load_n(&on, __ATOMIC_ACQUIRE))
        return;
    size_t len = strlen(text);
    if (len > BPS_LOG_LINE_MAX)
        len = BPS_LOG_LINE_MAX;
    int tries = 0;
    while (!port_log_trylock()) {
        if (++tries >= TRIES) {
            __atomic_store_n(&contended, __atomic_load_n(&contended, __ATOMIC_RELAXED) + 1,
                             __ATOMIC_RELAXED);
            return;
        }
    }
    uint32_t need = HDR + (uint32_t)len;
    /* Room: the oldest lines go, delivered or not. */
    while (head - tail + need > SIZE) {
        uint32_t n = ring_at(tail) | (uint32_t)ring_at(tail + 1) << 8;
        tail += HDR + n;
        tail_seq++;
    }
    uint32_t ms = (uint32_t)(mono_us / 1000);
    uint8_t hdr[HDR] = {(uint8_t)len, (uint8_t)(len >> 8), (uint8_t)ms, (uint8_t)(ms >> 8),
                        (uint8_t)(ms >> 16), (uint8_t)(ms >> 24)};
    ring_put(head, hdr, HDR);
    for (size_t i = 0; i < len; i++)
        ring[(head + HDR + i) & MASK] = clean((uint8_t)text[i]);
    head += need;
    __atomic_store_n(&head_seq, head_seq + 1, __ATOMIC_RELEASE);
    port_log_unlock();
}

void bps_log_set_capture(bool v) { __atomic_store_n(&on, v, __ATOMIC_RELEASE); }

bool bps_log_capturing(void) { return __atomic_load_n(&on, __ATOMIC_ACQUIRE); }

/* ---- Reading ---- */

/* Appends `n` bytes, as proto.c's writer does: on overflow nothing, and the
 * writer is marked (never here: sizes are worked out first). */
static void jput(bps_json *j, const char *s, size_t n)
{
    if (j->len + n + 1 > j->cap) {
        j->overflow = true;
        return;
    }
    memcpy(j->buf + j->len, s, n);
    j->len += n;
    j->buf[j->len] = 0;
}

static size_t digits(uint64_t v)
{
    size_t n = 1;
    while (v >= 10)
        v /= 10, n++;
    return n;
}

/* The fixed part: `,"dropped":N,"lines":[` and the closing `]`. */
#define FIXED (1 + 10 + 10 + 10 + 1)
/* Below this, lines are left for a heartbeat with more room. */
#define MIN_BUDGET (FIXED + 64)

void bps_log_json(bps_json *j, size_t budget, uint64_t now_ms, bps_log_batch *b)
{
    memset(b, 0, sizeof *b);
    if (budget < MIN_BUDGET || j->overflow || !port_log_trylock())
        return;
    uint32_t seq = acked_seq, pos = acked_pos;
    if ((int32_t)(tail_seq - seq) > 0) { /* the oldest undelivered lines went */
        seq = tail_seq;
        pos = tail;
    }
    uint32_t c = __atomic_load_n(&contended, __ATOMIC_RELAXED);
    uint32_t dropped = (seq - acked_seq) + (c - contended_acked);
    size_t start = j->len;
    bps_json_i64(j, "dropped", dropped);
    bps_json_arr(j, "lines");
    size_t left = budget - (j->len - start) - 1; /* the closing ']' */
    uint32_t first_seq = seq;
    while (seq != head_seq) {
        uint32_t len = ring_at(pos) | (uint32_t)ring_at(pos + 1) << 8;
        uint32_t ms32 = ring_at(pos + 2) | (uint32_t)ring_at(pos + 3) << 8 |
                        (uint32_t)ring_at(pos + 4) << 16 | (uint32_t)ring_at(pos + 5) << 24;
        /* In full: the latest time with these low 32 bits, up to now. */
        uint64_t ms = now_ms - (uint32_t)((uint32_t)now_ms - ms32);
        size_t esc = 0;
        for (uint32_t i = 0; i < len; i++) {
            uint8_t ch = ring_at(pos + HDR + i);
            esc += ch == '"' || ch == '\\' ? 2 : 1;
        }
        size_t frame = (seq != first_seq) + 1 + digits(ms) + 2 + 2; /* ,[ms,"..."] */
        size_t keep = len;
        if (frame + esc > left) {
            if (seq != first_seq || frame + 8 > left) {
                b->more = true;
                break;
            }
            /* A first line too long for the room: what fits of it. */
            size_t room = left - frame;
            esc = 0;
            for (keep = 0; keep < len; keep++) {
                uint8_t ch = ring_at(pos + HDR + keep);
                size_t w = ch == '"' || ch == '\\' ? 2 : 1;
                if (esc + w > room)
                    break;
                esc += w;
            }
        }
        if (seq != first_seq)
            jput(j, ",", 1);
        jput(j, "[", 1);
        bps_json_i64(j, NULL, (int64_t)ms);
        jput(j, ",\"", 2);
        for (uint32_t i = 0; i < keep; i++) {
            char ch = (char)ring_at(pos + HDR + i);
            if (ch == '"' || ch == '\\')
                jput(j, "\\", 1);
            jput(j, &ch, 1);
        }
        jput(j, "\"]", 2);
        left -= frame + esc;
        pos += HDR + len;
        seq++;
        b->lines++;
    }
    jput(j, "]", 1);
    b->end_seq = seq;
    b->end_pos = pos;
    b->contended = c;
    b->valid = !j->overflow;
    port_log_unlock();
}

void bps_log_delivered(const bps_log_batch *b)
{
    if (!b->valid)
        return;
    acked_seq = b->end_seq;
    acked_pos = b->end_pos;
    contended_acked = b->contended;
}

bool bps_log_pending(void)
{
    return __atomic_load_n(&head_seq, __ATOMIC_ACQUIRE) != acked_seq ||
           __atomic_load_n(&contended, __ATOMIC_RELAXED) != contended_acked;
}

void bps_log_reset(void)
{
    bps_log_set_capture(false);
    head = tail = head_seq = tail_seq = 0;
    acked_seq = acked_pos = contended_acked = contended = 0;
}
