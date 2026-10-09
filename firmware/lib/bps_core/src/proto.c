#include "proto.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- HTTP ---- */

size_t bps_http_head(char *out, size_t cap, const char *method, const char *host, uint16_t port,
                     const char *path, const char *content_type, size_t content_len,
                     const char *extra)
{
    int n = snprintf(out, cap,
                     "%s %s HTTP/1.1\r\nHost: %s:%u\r\nConnection: close\r\n"
                     "%s%s%sContent-Length: %lu\r\n%s\r\n",
                     method, path, host, (unsigned)port, content_type ? "Content-Type: " : "",
                     content_type ? content_type : "", content_type ? "\r\n" : "",
                     (unsigned long)content_len, extra ? extra : "");
    return n > 0 && (size_t)n < cap ? (size_t)n : 0;
}

static bool ci_prefix(const char *s, const char *end, const char *prefix)
{
    for (; *prefix; s++, prefix++) {
        if (s >= end)
            return false;
        char a = *s, b = *prefix;
        if (a >= 'A' && a <= 'Z')
            a += 'a' - 'A';
        if (a != b)
            return false;
    }
    return true;
}

bool bps_http_parse(const char *buf, size_t len, bps_http_response *out)
{
    const char *end = buf + len, *head_end = NULL;
    for (const char *p = buf; p + 3 < end; p++)
        if (p[0] == '\r' && p[1] == '\n' && p[2] == '\r' && p[3] == '\n') {
            head_end = p + 4;
            break;
        }
    if (!head_end || len < 12 || memcmp(buf, "HTTP/1.", 7) != 0)
        return false;
    out->status = atoi(buf + 9);
    out->body_off = (size_t)(head_end - buf);
    out->content_length = -1;
    for (const char *p = buf; p < head_end; p++) {
        if (*p != '\n')
            continue;
        if (ci_prefix(p + 1, head_end, "content-length:"))
            out->content_length = strtol(p + 1 + 15, NULL, 10);
    }
    return true;
}

/* ---- JSON ---- */

void bps_json_init(bps_json *j, char *buf, size_t cap)
{
    j->buf = buf;
    j->cap = cap;
    j->len = 0;
    j->overflow = cap == 0;
    if (cap)
        buf[0] = 0;
}

static void put(bps_json *j, const char *s, size_t n)
{
    if (j->len + n + 1 > j->cap) {
        j->overflow = true;
        return;
    }
    memcpy(j->buf + j->len, s, n);
    j->len += n;
    j->buf[j->len] = 0;
}

void bps_json_raw(bps_json *j, const char *s) { put(j, s, strlen(s)); }

void bps_json_key(bps_json *j, const char *key)
{
    if (j->len > 0 && j->buf[j->len - 1] != '{')
        put(j, ",", 1);
    bps_json_str(j, key);
    put(j, ":", 1);
}

void bps_json_str(bps_json *j, const char *s)
{
    put(j, "\"", 1);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            char e[2] = {'\\', (char)c};
            put(j, e, 2);
        } else if (c < 0x20) {
            char e[8];
            snprintf(e, sizeof e, "\\u%04x", c);
            put(j, e, 6);
        } else {
            put(j, (const char *)&c, 1);
        }
    }
    put(j, "\"", 1);
}

/* Decimal digits of `v`, without relying on printf's 64-bit support. */
static void put_u64(bps_json *j, uint64_t v)
{
    char d[20];
    int n = 0;
    do {
        d[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    char r[20];
    for (int i = 0; i < n; i++)
        r[i] = d[n - 1 - i];
    put(j, r, (size_t)n);
}

void bps_json_i64(bps_json *j, int64_t v)
{
    if (v < 0) {
        put(j, "-", 1);
        put_u64(j, (uint64_t)0 - (uint64_t)v);
    } else {
        put_u64(j, (uint64_t)v);
    }
}

void bps_json_fixed(bps_json *j, double v, int decimals)
{
    if (!isfinite(v)) {
        put(j, "0", 1);
        return;
    }
    uint64_t scale = 1;
    for (int i = 0; i < decimals; i++)
        scale *= 10;
    double r = round(fabs(v) * (double)scale);
    if (r >= 9.2e18) {
        put(j, "0", 1);
        return;
    }
    uint64_t u = (uint64_t)r;
    if (v < 0 && u > 0)
        put(j, "-", 1);
    put_u64(j, u / scale);
    if (decimals > 0) {
        put(j, ".", 1);
        uint64_t frac = u % scale;
        char f[20];
        for (int i = decimals - 1; i >= 0; i--) {
            f[i] = (char)('0' + frac % 10);
            frac /= 10;
        }
        put(j, f, (size_t)decimals);
    }
}

void bps_json_bool(bps_json *j, bool v) { bps_json_raw(j, v ? "true" : "false"); }

bool bps_json_number(const char *json, size_t len, const char *key, double *out)
{
    size_t klen = strlen(key);
    for (size_t i = 0; i + klen + 2 <= len; i++) {
        if (json[i] != '"' || memcmp(json + i + 1, key, klen) != 0 || json[i + klen + 1] != '"')
            continue;
        size_t p = i + klen + 2;
        while (p < len && (json[p] == ' ' || json[p] == ':'))
            p++;
        char num[32];
        size_t n = 0;
        while (p < len && n + 1 < sizeof num && strchr("+-.0123456789eE", json[p]))
            num[n++] = json[p++];
        num[n] = 0;
        if (n == 0)
            return false;
        char *end;
        *out = strtod(num, &end);
        return end != num;
    }
    return false;
}

/* ---- WAV ---- */

static void le32(uint8_t *b, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        b[i] = (uint8_t)(v >> (8 * i));
}

static void le16(uint8_t *b, uint16_t v)
{
    b[0] = (uint8_t)v;
    b[1] = (uint8_t)(v >> 8);
}

void bps_wav_header(uint8_t out[BPS_WAV_HEADER_LEN], uint32_t sample_rate, uint32_t samples)
{
    uint32_t data = samples * 2;
    memcpy(out, "RIFF", 4);
    le32(out + 4, 36 + data);
    memcpy(out + 8, "WAVEfmt ", 8);
    le32(out + 16, 16);
    le16(out + 20, 1); /* PCM */
    le16(out + 22, 1); /* mono */
    le32(out + 24, sample_rate);
    le32(out + 28, sample_rate * 2);
    le16(out + 32, 2);
    le16(out + 34, 16);
    memcpy(out + 36, "data", 4);
    le32(out + 40, data);
}

/* ---- UUIDs ---- */

static const char MARKER[7] = {'b', 'p', 's', '-', 'm', 'c', 'u'};

void bps_uuid_from_chip(const uint8_t c[8], uint8_t out[16])
{
    memcpy(out, c, 6);
    out[6] = (uint8_t)(0x80 | c[6] >> 4);              /* version 8 */
    out[7] = (uint8_t)(c[6] << 4 | c[7] >> 4);
    out[8] = (uint8_t)(0x80 | (c[7] & 0x0f));          /* RFC 9562 variant */
    memcpy(out + 9, MARKER, sizeof MARKER);
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

bool bps_uuid_parse(const char *s, uint8_t out[16])
{
    int n = 0;
    for (size_t i = 0; s[i] && n < 32; i++) {
        if (s[i] == '-')
            continue;
        int v = hexval(s[i]);
        if (v < 0)
            return false;
        if (n % 2 == 0)
            out[n / 2] = (uint8_t)(v << 4);
        else
            out[n / 2] |= (uint8_t)v;
        n++;
    }
    return n == 32;
}

void bps_uuid_format(const uint8_t id[16], char out[37])
{
    static const char hex[] = "0123456789abcdef";
    int p = 0;
    for (int i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10)
            out[p++] = '-';
        out[p++] = hex[id[i] >> 4];
        out[p++] = hex[id[i] & 15];
    }
    out[p] = 0;
}
