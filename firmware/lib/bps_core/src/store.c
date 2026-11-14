#include "store.h"

#include <string.h>

void bps_store_init(bps_store *s, uint8_t *buf, uint32_t bytes)
{
    memset(s, 0, sizeof *s);
    s->buf = buf;
    s->cap = buf ? bytes : 0;
}

static uint32_t slot_of(const bps_store *s, uint32_t k) { return (s->first + k) % BPS_STORE_ITEMS; }

/* Where the next byte goes: just past the back item, or the buffer's start
 * when nothing is held. */
static uint32_t back_end(const bps_store *s)
{
    if (s->count == 0)
        return 0;
    const bps_store_item *b = &s->items[slot_of(s, s->count - 1)];
    return (uint32_t)(((uint64_t)b->pos + (b->len - b->base)) % s->cap);
}

int bps_store_open(bps_store *s)
{
    if (s->open || s->count == BPS_STORE_ITEMS || s->cap == 0)
        return -1;
    uint32_t pos = back_end(s);
    uint32_t slot = slot_of(s, s->count);
    s->items[slot].pos = pos;
    s->items[slot].base = 0;
    s->items[slot].len = 0;
    s->count++;
    s->open = true;
    return (int)slot;
}

bool bps_store_append(bps_store *s, const void *p, uint32_t n)
{
    if (!s->open || n > s->cap - s->used)
        return false;
    bps_store_item *it = &s->items[slot_of(s, s->count - 1)];
    uint32_t at = (uint32_t)(((uint64_t)it->pos + (it->len - it->base)) % s->cap);
    uint32_t k = s->cap - at < n ? s->cap - at : n;
    memcpy(s->buf + at, p, k);
    memcpy(s->buf, (const uint8_t *)p + k, n - k);
    it->len += n;
    s->used += n;
    return true;
}

void bps_store_close(bps_store *s) { s->open = false; }

void bps_store_cancel(bps_store *s)
{
    if (!s->open)
        return;
    bps_store_item *it = &s->items[slot_of(s, s->count - 1)];
    s->used -= it->len - it->base;
    s->count--;
    s->open = false;
}

uint32_t bps_store_count(const bps_store *s) { return s->count; }

int bps_store_at(const bps_store *s, uint32_t k)
{
    return k < s->count ? (int)slot_of(s, k) : -1;
}

bool bps_store_is_open(const bps_store *s, int slot)
{
    return s->open && s->count && slot == (int)slot_of(s, s->count - 1);
}

uint32_t bps_store_len(const bps_store *s, int slot) { return s->items[slot].len; }

uint32_t bps_store_read(const bps_store *s, int slot, uint32_t off, void *out, uint32_t n)
{
    const bps_store_item *it = &s->items[slot];
    if (off < it->base || off >= it->len)
        return 0;
    if (n > it->len - off)
        n = it->len - off;
    uint32_t at = (uint32_t)(((uint64_t)it->pos + (off - it->base)) % s->cap);
    uint32_t k = s->cap - at < n ? s->cap - at : n;
    memcpy(out, s->buf + at, k);
    memcpy((uint8_t *)out + k, s->buf, n - k);
    return n;
}

void bps_store_release(bps_store *s, uint32_t off)
{
    if (s->count == 0)
        return;
    bps_store_item *it = &s->items[s->first];
    if (off > it->len)
        off = it->len;
    if (off <= it->base)
        return;
    uint32_t n = off - it->base;
    it->pos = (it->pos + n) % s->cap;
    it->base = off;
    s->used -= n;
}

void bps_store_pop(bps_store *s)
{
    if (s->count == 0)
        return;
    if (s->count == 1)
        s->open = false;
    bps_store_item *it = &s->items[s->first];
    s->used -= it->len - it->base;
    s->first = (s->first + 1) % BPS_STORE_ITEMS;
    s->count--;
}

uint32_t bps_store_free(const bps_store *s) { return s->cap - s->used; }

static void reverse(uint8_t *a, uint32_t n)
{
    for (uint32_t i = 0, j = n; i + 1 < j; i++, j--) {
        uint8_t t = a[i];
        a[i] = a[j - 1];
        a[j - 1] = t;
    }
}

bool bps_store_resize(bps_store *s, uint32_t cap, void *(*re)(void *p, size_t n))
{
    if (cap == 0 || s->used > cap)
        return false;
    if (s->count && s->cap) {
        uint32_t p = s->items[s->first].pos;
        if (p) {
            if (p + s->used <= s->cap) {
                memmove(s->buf, s->buf + p, s->used);
            } else { /* wraps: rotate the whole buffer left by p */
                reverse(s->buf, p);
                reverse(s->buf + p, s->cap - p);
                reverse(s->buf, s->cap);
            }
            for (uint32_t k = 0; k < s->count; k++) {
                bps_store_item *it = &s->items[slot_of(s, k)];
                it->pos = (it->pos + s->cap - p) % s->cap;
            }
        }
    }
    void *nb = re(s->buf, cap);
    if (!nb)
        return false;
    s->buf = nb;
    s->cap = cap;
    return true;
}
