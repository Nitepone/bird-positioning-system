#include "trigger.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

void bps_trigger_default(bps_trigger_config *t)
{
    memset(t, 0, sizeof *t);
    t->type = BPS_TRIGGER_ENERGY;
    bps_gate_config_default(&t->gate);
    strcpy(t->name, "energy");
}

void bps_validation_default(bps_validation_config *v)
{
    memset(v, 0, sizeof *v);
    v->type = BPS_VALIDATION_NONE;
    strcpy(v->name, "none");
}

/* The gate's four settings, all present; kept within what the gate handles. */
static bool parse_gate(const char *json, size_t len, bps_gate_config *g)
{
    double lo, hi, thr, frame;
    if (!bps_json_number(json, len, "band_low_hz", &lo) ||
        !bps_json_number(json, len, "band_high_hz", &hi) ||
        !bps_json_number(json, len, "threshold_db", &thr) ||
        !bps_json_number(json, len, "frame_ms", &frame))
        return false;
    if (!(lo > 0 && hi > lo && thr >= 0 && frame >= 1 && frame <= 10000))
        return false;
    g->band_low_hz = (float)lo;
    g->band_high_hz = (float)hi;
    g->threshold_db = (float)thr;
    g->frame_ms = (uint32_t)frame;
    return true;
}

bool bps_trigger_parse(const char *json, size_t len, bps_trigger_config *out)
{
    bps_trigger_config t;
    bps_trigger_default(&t);
    char name[sizeof t.name];
    if (bps_json_string(json, len, "trigger_type", name, sizeof name)) {
        strcpy(t.name, name);
        if (strcmp(name, "energy") != 0)
            t.type = BPS_TRIGGER_UNKNOWN;
        else if (!parse_gate(json, len, &t.gate))
            return false; /* an energy trigger without usable settings */
    } else if (!parse_gate(json, len, &t.gate)) {
        return false; /* neither a trigger nor (an older server) a gate */
    }
    *out = t;
    return true;
}

bool bps_validation_parse(const char *json, size_t len, bps_validation_config *out)
{
    bps_validation_config v;
    bps_validation_default(&v);
    char name[sizeof v.name];
    if (!bps_json_string(json, len, "validation_type", name, sizeof name))
        return false;
    strcpy(v.name, name);
    if (strcmp(name, "none") != 0)
        v.type = BPS_VALIDATION_UNKNOWN;
    *out = v;
    return true;
}

bool bps_trigger_equal(const bps_trigger_config *a, const bps_trigger_config *b)
{
    if (a->type != b->type || strcmp(a->name, b->name) != 0)
        return false;
    if (a->type != BPS_TRIGGER_ENERGY)
        return true;
    const bps_gate_config *x = &a->gate, *y = &b->gate;
    return x->band_low_hz == y->band_low_hz && x->band_high_hz == y->band_high_hz &&
           x->threshold_db == y->threshold_db && x->frame_ms == y->frame_ms;
}

bool bps_validation_equal(const bps_validation_config *a, const bps_validation_config *b)
{
    return a->type == b->type && strcmp(a->name, b->name) == 0;
}

void bps_trigger_json(bps_json *j, const bps_trigger_config *t)
{
    bps_json_obj(j, "trigger");
    bps_json_str(j, "trigger_type", t->name);
    if (t->type == BPS_TRIGGER_ENERGY) {
        bps_json_fixed(j, "band_low_hz", t->gate.band_low_hz, 1);
        bps_json_fixed(j, "band_high_hz", t->gate.band_high_hz, 1);
        bps_json_fixed(j, "threshold_db", t->gate.threshold_db, 2);
        bps_json_i64(j, "frame_ms", t->gate.frame_ms);
    }
    bps_json_raw(j, "}");
}

void bps_validation_json(bps_json *j, const bps_validation_config *v)
{
    bps_json_obj(j, "validation");
    bps_json_str(j, "validation_type", v->name);
    bps_json_raw(j, "}");
}

void bps_trigger_describe(const bps_trigger_config *t, char *out, size_t cap)
{
    if (t->type != BPS_TRIGGER_ENERGY) {
        snprintf(out, cap, "%s", t->name);
        return;
    }
    /* In integers: the core's logs avoid printf's floating point. */
    long tenths = lroundf(t->gate.threshold_db * 10.0f);
    snprintf(out, cap, "energy, %ld.%ld dB above the floor, %ld-%ld Hz, %lu ms frames",
             tenths / 10, tenths % 10, lroundf(t->gate.band_low_hz),
             lroundf(t->gate.band_high_hz), (unsigned long)t->gate.frame_ms);
}
