#include "ota.h"

#include <stdio.h>
#include <string.h>

void bps_build_json(bps_json *j, const bps_build *b)
{
    bps_json_obj(j, "build");
    bps_json_str(j, "hardware_id", b->hardware_id ? b->hardware_id : "");
    if (b->tag && b->tag[0])
        bps_json_str(j, "tag", b->tag);
    else {
        bps_json_key(j, "tag");
        bps_json_raw(j, "null");
    }
    bps_json_str(j, "commit", b->commit ? b->commit : "");
    bps_json_bool(j, "dirty", b->dirty);
    bps_json_str(j, "describe", b->describe ? b->describe : "");
    bps_json_str(j, "built_at", b->built_at ? b->built_at : "");
    bps_json_raw(j, "}");
}

bool bps_ota_offer_parse(const char *json, size_t len, bps_ota_offer *out)
{
    memset(out, 0, sizeof *out);
    /* Every key of the offer is unique in the reply (the contract), so they
     * are looked up anywhere; an offer needs its id and url. */
    if (!bps_json_int(json, len, "firmware_build_id", &out->build_id) || out->build_id <= 0 ||
        !bps_json_string(json, len, "firmware_url", out->url, sizeof out->url) ||
        out->url[0] != '/') {
        memset(out, 0, sizeof *out);
        return false;
    }
    if (!bps_json_int(json, len, "firmware_size", &out->size) || out->size < 0)
        out->size = 0;
    if (!bps_json_string(json, len, "firmware_md5", out->md5, sizeof out->md5))
        out->md5[0] = 0;
    if (!bps_json_string(json, len, "firmware_describe", out->describe, sizeof out->describe))
        out->describe[0] = 0;
    return true;
}

bool bps_ota_battery_ok(const bps_battery_reading *b, int min_percent, char *detail, size_t cap)
{
    if (cap)
        detail[0] = 0;
    if (!b || !b->present || b->charging || b->centi_percent >= (int32_t)min_percent * 100)
        return true;
    long pct = b->centi_percent < 0 ? 0 : (long)(b->centi_percent / 100);
    snprintf(detail, cap, "battery %ld%%", pct);
    return false;
}

void bps_ota_status_json(bps_json *j, int64_t build_id, const char *state, const char *detail)
{
    bps_json_obj(j, "firmware_status");
    bps_json_i64(j, "build_id", build_id);
    bps_json_str(j, "state", state);
    if (detail && detail[0]) {
        char d[BPS_OTA_DETAIL_MAX + 1];
        snprintf(d, sizeof d, "%s", detail);
        bps_json_str(j, "detail", d);
    }
    bps_json_raw(j, "}");
}
