/*
 * The recording trigger (what opens a recording) and validation (how a
 * triggered recording is checked before it is uploaded) that the server asks
 * for, as `bsp_proto::TriggerConfig` and `ValidationConfig`. Registration and
 * heartbeat replies carry them; this build runs the energy trigger (the noise
 * gate) and no validation.
 */
#ifndef BPS_TRIGGER_H
#define BPS_TRIGGER_H

#include "bps_common.h"
#include "gate.h"
#include "proto.h"

typedef enum {
    BPS_TRIGGER_ENERGY,
    BPS_TRIGGER_UNKNOWN, /* a type this build does not run */
} bps_trigger_type;

typedef enum {
    BPS_VALIDATION_NONE,
    BPS_VALIDATION_UNKNOWN,
} bps_validation_type;

typedef struct {
    bps_trigger_type type;
    bps_gate_config gate; /* BPS_TRIGGER_ENERGY */
    char name[16];        /* the type as sent */
} bps_trigger_config;

typedef struct {
    bps_validation_type type;
    char name[16];
} bps_validation_config;

void bps_trigger_default(bps_trigger_config *t);
void bps_validation_default(bps_validation_config *v);

/* The trigger in a server reply: `trigger_type` with its settings, or, from a
 * server that predates triggers, the registration's gate settings alone.
 * False (and `out` untouched) if the reply has neither. */
bool bps_trigger_parse(const char *json, size_t len, bps_trigger_config *out);
/* False (and `out` untouched) if the reply has no `validation_type`. */
bool bps_validation_parse(const char *json, size_t len, bps_validation_config *out);

bool bps_trigger_equal(const bps_trigger_config *a, const bps_trigger_config *b);
bool bps_validation_equal(const bps_validation_config *a, const bps_validation_config *b);

/* `"trigger":{...}` and `"validation":{...}` members, for heartbeats. */
void bps_trigger_json(bps_json *j, const bps_trigger_config *t);
void bps_validation_json(bps_json *j, const bps_validation_config *v);

/* One line for the log, e.g. "energy, 10.0 dB above the floor, 1000-10000 Hz, 100 ms frames". */
void bps_trigger_describe(const bps_trigger_config *t, char *out, size_t cap);

#endif
