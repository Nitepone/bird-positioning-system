/*
 * Validation: before a triggered recording is uploaded, a model on the
 * storage card checks whether a bird is calling in it (trigger.h's
 * BPS_VALIDATION_MODEL). The model is a folder in the card's /models/, as
 * trigger-models/ lays it out: model.tflite and model.json.
 *
 * Loading reads the card from the client's loop (holding the card's reads
 * meanwhile: card.h); setting up TFLite Micro and every check run on the
 * port's worker task. Anything that goes wrong leaves
 * validation off, so every recording is uploaded, and says why
 * (bps_validate_note).
 */
#ifndef BPS_VALIDATE_H
#define BPS_VALIDATE_H

#include "bps_common.h"
#include "frontend.h"
#include "ring.h"
#include "trigger.h"

/* A model's model.json. */
typedef struct {
    char name[BPS_MODEL_NAME_MAX];
    char file[32];
    uint32_t model_bytes;
    bps_fe_config fe;
    bool output_logit; /* else probabilities */
    uint32_t bird_first, bird_count;
    float threshold;
    uint32_t arena_bytes;
} bps_model_config;

/* False, with the reason in `why`, if a setting is missing or unusable. */
bool bps_model_config_parse(const char *json, size_t len, bps_model_config *out, const char **why);

typedef struct {
    bool ok;       /* false: the check failed (the recording is uploaded) */
    bool upload;   /* score >= threshold, or the check failed */
    float score, threshold;
    uint32_t window_start; /* ring index of the first 48 kHz sample read */
    uint32_t window_ms;
    uint32_t frontend_us, invoke_us;
    uint32_t resample_us; /* of frontend_us */
} bps_validate_result;

void bps_validate_init(const bps_ring *ring, uint32_t sample_rate);
/* What the server asks for; loads or drops the model as needed. */
void bps_validate_configure(const bps_validation_config *v);
/* Drives loading; call from the client's loop. */
void bps_validate_poll(uint64_t now);
/* Asked, before a model is loaded, to make `bytes` of big memory free (by
 * shrinking the store); true once it is. Until then the model waits. */
void bps_validate_set_room(bool (*room)(size_t bytes));
/* The memory a model waiting for room needs (0 if none is waiting). */
size_t bps_validate_need(void);
/* A model is being loaded or let go: memory use is about to change. */
bool bps_validate_settling(void);

/* A model is loaded and checks recordings. */
bool bps_validate_active(void);
/* What runs: the model, or none while it is not (yet) loaded. */
void bps_validate_running(bps_validation_config *out);
/* Why validation is not running as asked, or how loading goes; NULL if it is. */
const char *bps_validate_note(void);
const bps_model_config *bps_validate_model(void);
size_t bps_validate_arena_used(void);

/* 48 kHz samples a check reads: the model's window, and the margin the
 * resampler needs on each side of it. */
uint32_t bps_validate_window(void);
uint32_t bps_validate_margin(void);
/* Checks the window starting at ring index `start` (whose window and margin
 * must have been captured). False while a check is still running. */
bool bps_validate_start(uint32_t start);
/* Instead of reading the ring when it starts, a check can be fed its audio as
 * it is captured (upload.c: a ring in internal RAM is too short to hold the
 * window): collecting starts for the window at ring index `start` (and its
 * margin either side). False while a check is still running. */
bool bps_validate_collect(uint32_t start);
/* `n` samples from ring index `idx` on, given in order (again is harmless);
 * the check starts once the window and its margins are all in. */
void bps_validate_feed(uint32_t idx, const int16_t *pcm, uint32_t n);
/* Gives up collecting (the recording was dropped). */
void bps_validate_uncollect(void);
/* Collecting, and the window not complete yet. */
bool bps_validate_collecting(void);
/* True once per check, when it is done; judged against the threshold then. */
bool bps_validate_take(bps_validate_result *out);

#endif
