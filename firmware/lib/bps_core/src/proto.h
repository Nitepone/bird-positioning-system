/*
 * Small wire helpers: HTTP/1.1 request heads and response parsing, a JSON
 * writer plus number lookup, the WAV header, and client UUIDs.
 */
#ifndef BPS_PROTO_H
#define BPS_PROTO_H

#include "bps_common.h"

#define BPS_CLIENT_API_PREFIX "/api/v1/client"
#define BPS_HEADER_START_NS "x-bsp-start-ns"

/* ---- HTTP ---- */

/* Writes a request head (with `Connection: close`) for a body of
 * `content_len` bytes; `extra` is NULL or more header lines, each ending in
 * "\r\n". Returns its length, or 0 if it did not fit. */
size_t bps_http_head(char *out, size_t cap, const char *method, const char *host, uint16_t port,
                     const char *path, const char *content_type, size_t content_len,
                     const char *extra);

typedef struct {
    int status;
    size_t body_off;         /* where the body starts in the buffer */
    long content_length;     /* -1 if absent */
} bps_http_response;

/* True once the response head in `buf` is complete. */
bool bps_http_parse(const char *buf, size_t len, bps_http_response *out);

/* ---- JSON ---- */

typedef struct {
    char *buf;
    size_t cap, len;
    bool overflow;
} bps_json;

void bps_json_init(bps_json *j, char *buf, size_t cap);
void bps_json_raw(bps_json *j, const char *s);
/* `"key":`, preceded by a comma unless it opens an object. */
void bps_json_key(bps_json *j, const char *key);
void bps_json_str(bps_json *j, const char *s);
void bps_json_i64(bps_json *j, int64_t v);
void bps_json_fixed(bps_json *j, double v, int decimals);
void bps_json_bool(bps_json *j, bool v);

/* The number stored under `"key"` anywhere in `json` (keys must be unique). */
bool bps_json_number(const char *json, size_t len, const char *key, double *out);

/* ---- WAV ---- */

#define BPS_WAV_HEADER_LEN 44
/* Header of a 16-bit mono PCM WAV holding `samples` samples. */
void bps_wav_header(uint8_t out[BPS_WAV_HEADER_LEN], uint32_t sample_rate, uint32_t samples);

/* ---- UUIDs ---- */

/* A stable client id from the chip's 64-bit unique id: a version-8 UUID that
 * keeps all 64 bits and ends in the marker "bps-mcu". */
void bps_uuid_from_chip(const uint8_t chip[8], uint8_t out[16]);
bool bps_uuid_parse(const char *s, uint8_t out[16]);
void bps_uuid_format(const uint8_t id[16], char out[37]);

#endif
