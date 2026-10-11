#include "flac.h"

#include <string.h>

/* Rice partitions per frame at most: 2^4 = 16 of 64 samples. */
#define MAX_PARTITION_ORDER 4
#define MAX_RICE_PARAM 14 /* 15 means "escape" with 4-bit parameters */

/* ---- Bits, most significant first ---- */

typedef struct {
    uint8_t *p;
    size_t pos, cap;
    uint32_t acc;
    unsigned n; /* bits in `acc` not yet written: always < 8 between calls */
    bool overflow;
} bitw;

/* Writes the low `bits` (at most 24) bits of `v`. */
static void put(bitw *w, uint32_t v, unsigned bits)
{
    w->acc = (w->acc << bits) | (v & ((1u << bits) - 1));
    w->n += bits;
    while (w->n >= 8) {
        w->n -= 8;
        if (w->pos < w->cap)
            w->p[w->pos] = (uint8_t)(w->acc >> w->n);
        else
            w->overflow = true;
        w->pos++;
    }
}

static void put_zeros(bitw *w, uint32_t count)
{
    for (; count > 24; count -= 24)
        put(w, 0, 24);
    put(w, 0, count);
}

static void put_rice(bitw *w, uint32_t u, unsigned k)
{
    uint32_t q = u >> k;
    if (q + 1 + k <= 24) { /* q zeros, a one, then the low k bits */
        put(w, (1u << k) | (u & ((1u << k) - 1)), q + 1 + k);
        return;
    }
    put_zeros(w, q);
    put(w, 1, 1);
    put(w, u, k);
}

static void align(bitw *w)
{
    if (w->n)
        put(w, 0, 8 - w->n);
}

/* ---- Checksums ---- */

static uint8_t crc8(const uint8_t *b, size_t n)
{
    uint8_t c = 0;
    for (size_t i = 0; i < n; i++) {
        c ^= b[i];
        for (int k = 0; k < 8; k++)
            c = (uint8_t)(c & 0x80 ? (c << 1) ^ 0x07 : c << 1);
    }
    return c;
}

static uint16_t crc16(const uint8_t *b, size_t n)
{
    uint16_t c = 0;
    for (size_t i = 0; i < n; i++) {
        c ^= (uint16_t)(b[i] << 8);
        for (int k = 0; k < 8; k++)
            c = (uint16_t)(c & 0x8000 ? (c << 1) ^ 0x8005 : c << 1);
    }
    return c;
}

/* ---- Stream header ---- */

void bps_flac_header(uint8_t out[BPS_FLAC_HEADER_LEN], uint32_t sample_rate,
                     uint64_t total_samples)
{
    memcpy(out, "fLaC", 4);
    bitw w = {out + 4, 0, BPS_FLAC_HEADER_LEN - 4, 0, 0, false};
    put(&w, 0x80, 8); /* last metadata block; type 0, STREAMINFO */
    put(&w, 34, 24);
    put(&w, BPS_FLAC_BLOCK, 16); /* min and max block size (the last may be shorter) */
    put(&w, BPS_FLAC_BLOCK, 16);
    put(&w, 0, 24); /* min and max frame size: unknown */
    put(&w, 0, 24);
    put(&w, sample_rate >> 4, 16);
    put(&w, sample_rate & 0xf, 4);
    put(&w, 0, 3);  /* channels - 1 */
    put(&w, 15, 5); /* bits per sample - 1 */
    put(&w, (uint32_t)(total_samples >> 32) & 0xf, 4);
    put(&w, (uint32_t)(total_samples >> 16) & 0xffff, 16);
    put(&w, (uint32_t)total_samples & 0xffff, 16);
    for (int i = 0; i < 16; i++) /* MD5 of the samples: not computed */
        put(&w, 0, 8);
}

/* ---- Frames ---- */

static unsigned rate_code(uint32_t rate)
{
    switch (rate) {
    case 88200: return 1;
    case 176400: return 2;
    case 192000: return 3;
    case 8000: return 4;
    case 16000: return 5;
    case 22050: return 6;
    case 24000: return 7;
    case 32000: return 8;
    case 44100: return 9;
    case 48000: return 10;
    case 96000: return 11;
    default: return 0; /* see STREAMINFO */
    }
}

/* The frame number in FLAC's extended UTF-8. */
static void put_utf8(bitw *w, uint32_t v)
{
    if (v < 0x80) {
        put(w, v, 8);
        return;
    }
    int extra = v < 0x800 ? 1 : v < 0x10000 ? 2 : v < 0x200000 ? 3 : v < 0x4000000 ? 4 : 5;
    uint32_t marker = (0xff00u >> (extra + 1)) & 0xff; /* extra + 1 one bits, then a zero */
    put(w, marker | (v >> (6 * extra)), 8);
    for (int i = extra - 1; i >= 0; i--)
        put(w, 0x80 | ((v >> (6 * i)) & 0x3f), 8);
}

static int32_t residual(const int16_t *x, uint32_t i, unsigned order)
{
    switch (order) {
    case 0: return x[i];
    case 1: return x[i] - x[i - 1];
    case 2: return x[i] - 2 * x[i - 1] + x[i - 2];
    case 3: return x[i] - 3 * x[i - 1] + 3 * x[i - 2] - x[i - 3];
    default: return x[i] - 4 * x[i - 1] + 6 * x[i - 2] - 4 * x[i - 3] + x[i - 4];
    }
}

static uint32_t zigzag(int32_t r) { return ((uint32_t)r << 1) ^ (uint32_t)(r >> 31); }

/* The fixed predictor order with the smallest residuals. */
static unsigned best_order(const int16_t *x, uint32_t n)
{
    if (n <= 4)
        return 0;
    uint32_t sum[5] = {0};
    for (uint32_t i = 4; i < n; i++)
        for (unsigned o = 0; o < 5; o++) {
            int32_t r = residual(x, i, o);
            sum[o] += (uint32_t)(r < 0 ? -r : r);
        }
    unsigned best = 0;
    for (unsigned o = 1; o < 5; o++)
        if (sum[o] < sum[best])
            best = o;
    return best;
}

/* Estimated bits for `c` residuals summing to `s` (zigzagged) with parameter k. */
static uint32_t rice_bits(uint32_t c, uint32_t s, unsigned k) { return c * (k + 1) + (s >> k); }

static unsigned best_param(uint32_t c, uint32_t s, uint32_t *bits)
{
    unsigned best = 0;
    *bits = rice_bits(c, s, 0);
    for (unsigned k = 1; k <= MAX_RICE_PARAM; k++) {
        uint32_t b = rice_bits(c, s, k);
        if (b < *bits) {
            *bits = b;
            best = k;
        }
    }
    return best;
}

typedef struct {
    unsigned order, porder;
    uint8_t param[1 << MAX_PARTITION_ORDER];
    uint32_t bits; /* estimated, whole subframe */
} fixed_plan;

/* Picks the partition order and Rice parameters for a predictor order. */
static void plan_fixed(const int16_t *x, uint32_t n, unsigned order, fixed_plan *plan)
{
    unsigned pmax = 0;
    while (pmax < MAX_PARTITION_ORDER && n % (2u << pmax) == 0 && (n >> (pmax + 1)) > order)
        pmax++;
    /* Sums over the finest partitions, merged pairwise for coarser ones. */
    uint32_t sums[1 << MAX_PARTITION_ORDER];
    uint32_t psize = n >> pmax;
    for (uint32_t j = 0; j < (1u << pmax); j++) {
        uint32_t s = 0;
        for (uint32_t i = j == 0 ? order : j * psize; i < (j + 1) * psize; i++)
            s += zigzag(residual(x, i, order));
        sums[j] = s;
    }
    plan->order = order;
    plan->bits = UINT32_MAX;
    for (int p = (int)pmax; p >= 0; p--) {
        if (p < (int)pmax)
            for (uint32_t j = 0; j < (1u << p); j++)
                sums[j] = sums[2 * j] + sums[2 * j + 1];
        uint32_t size = n >> p, total = 8 + 16 * order + 2 + 4;
        uint8_t params[1 << MAX_PARTITION_ORDER];
        for (uint32_t j = 0; j < (1u << p); j++) {
            uint32_t c = j == 0 ? size - order : size, b;
            params[j] = (uint8_t)best_param(c, sums[j], &b);
            total += 4 + b;
        }
        if (total < plan->bits) {
            plan->bits = total;
            plan->porder = (unsigned)p;
            memcpy(plan->param, params, 1u << p);
        }
    }
}

static void put_fixed(bitw *w, const int16_t *x, uint32_t n, const fixed_plan *plan)
{
    put(w, (0x08 | plan->order) << 1, 8); /* 0, type 001ooo, no wasted bits */
    for (unsigned i = 0; i < plan->order; i++)
        put(w, (uint16_t)x[i], 16);
    put(w, 0, 2); /* Rice coding, 4-bit parameters */
    put(w, plan->porder, 4);
    uint32_t size = n >> plan->porder;
    for (uint32_t j = 0; j < (1u << plan->porder); j++) {
        unsigned k = plan->param[j];
        put(w, k, 4);
        for (uint32_t i = j == 0 ? plan->order : j * size; i < (j + 1) * size; i++)
            put_rice(w, zigzag(residual(x, i, plan->order)), k);
    }
}

static void put_verbatim(bitw *w, const int16_t *x, uint32_t n)
{
    put(w, 0x02, 8); /* 0, type 000001, no wasted bits */
    for (uint32_t i = 0; i < n; i++)
        put(w, (uint16_t)x[i], 16);
}

static size_t encode(uint8_t *out, const int16_t *x, uint32_t n, uint32_t frame_number,
                     uint32_t sample_rate, bool verbatim)
{
    bitw w = {out, 0, BPS_FLAC_MAX_FRAME, 0, 0, false};
    put(&w, 0xfff8, 16); /* sync, fixed block size */
    unsigned bs = n == BPS_FLAC_BLOCK ? 10 : n <= 256 ? 6 : 7;
    put(&w, bs, 4);
    put(&w, rate_code(sample_rate), 4);
    put(&w, 0x08, 8); /* mono, 16 bits per sample */
    put_utf8(&w, frame_number);
    if (bs == 6)
        put(&w, n - 1, 8);
    else if (bs == 7)
        put(&w, n - 1, 16);
    put(&w, crc8(out, w.pos), 8);

    bool constant = true;
    for (uint32_t i = 1; i < n && constant; i++)
        constant = x[i] == x[0];
    if (constant) {
        put(&w, 0x00, 8); /* 0, type 000000, no wasted bits */
        put(&w, (uint16_t)x[0], 16);
    } else if (verbatim) {
        put_verbatim(&w, x, n);
    } else {
        fixed_plan plan;
        plan_fixed(x, n, best_order(x, n), &plan);
        if (plan.bits < 8 + 16 * n)
            put_fixed(&w, x, n, &plan);
        else
            put_verbatim(&w, x, n);
    }
    align(&w);
    uint16_t crc = crc16(out, w.pos);
    put(&w, crc, 16);
    return w.overflow ? 0 : w.pos;
}

size_t bps_flac_frame(uint8_t *out, const int16_t *pcm, uint32_t n, uint32_t frame_number,
                      uint32_t sample_rate)
{
    size_t len = encode(out, pcm, n, frame_number, sample_rate, false);
    /* The estimate let the residuals grow past verbatim size: send verbatim. */
    return len ? len : encode(out, pcm, n, frame_number, sample_rate, true);
}
