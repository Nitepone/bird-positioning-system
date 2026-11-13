/*
 * Over-the-air firmware updates, the portable part: what the build is (sent
 * at registration), the server's offer of another build (in heartbeat
 * replies), whether the battery allows taking it, and how the client reports
 * an offer it did not take, or one that failed (in heartbeats). The download
 * and flashing are the board's (port_ota_update()); client.c runs the steps.
 */
#ifndef BPS_OTA_H
#define BPS_OTA_H

#include "bps_common.h"
#include "proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The build's identity, as embedded in the image (src/build_info.c). */
typedef struct {
    const char *hardware_id;
    const char *tag; /* NULL (or "") unless HEAD was exactly at a tag on a clean tree */
    const char *commit;
    bool dirty;
    const char *describe;
    const char *built_at; /* "YYYY-MM-DDTHH:MM:SSZ" */
    const char *info;     /* the JSON object embedded in the image, for the log; or NULL */
} bps_build;

/* `"build":{...}` for the registration. */
void bps_build_json(bps_json *j, const bps_build *b);

/* An offer of another build, from a heartbeat reply's "firmware_offer". */
typedef struct {
    int64_t build_id;
    char url[128];      /* a path on the server's client port */
    int64_t size;       /* bytes; 0 if not given */
    char md5[33];       /* "" if not given */
    char describe[64];  /* "" if not given (or too long) */
} bps_ota_offer;

/* False when the reply offers nothing (or nothing usable: no positive id, or
 * a url that is not a path). */
bool bps_ota_offer_parse(const char *json, size_t len, bps_ota_offer *out);

/* The fuel gauge's latest reading. */
typedef struct {
    bool present; /* a gauge with a battery reading; false: no gauge, or external power */
    int32_t centi_percent;
    bool charging;
} bps_battery_reading;

/* Whether an update may start now: a battery that is not charging needs at
 * least `min_percent`. Without a reading it may. When not, `detail` says why
 * ("battery 34%"). */
bool bps_ota_battery_ok(const bps_battery_reading *b, int min_percent, char *detail, size_t cap);

#define BPS_OTA_DETAIL_MAX 120

/* `"firmware_status":{...}` for a heartbeat; `detail` is cut to
 * BPS_OTA_DETAIL_MAX characters, and left out if empty or NULL. */
void bps_ota_status_json(bps_json *j, int64_t build_id, const char *state, const char *detail);

#ifdef __cplusplus
}
#endif

#endif
