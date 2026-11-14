/*
 * PDM to PCM in software, for chips whose I2S has no PDM-to-PCM filter (the
 * ESP32-C5): the microphone's 1-bit stream at 64 times the sample rate
 * (3.072 MHz for 48 kHz) is low-pass filtered and decimated by 64.
 *
 * The filter is a 640-tap FIR (Kaiser-windowed sinc, cut-off 23 kHz, about
 * 70 dB down from 33 kHz: what folds into the band below 15 kHz is gone). Its
 * input is bits, so it runs on bytes: a table per byte of the window gives
 * that byte's eight taps' sum for each of its 256 values. The taps are
 * symmetric, so the second half of the window uses the first half's tables
 * with the byte's bits reversed: 40 tables, 20 KB.
 */
#ifndef BPS_PDM_H
#define BPS_PDM_H

#include "bps_common.h"

#define BPS_PDM_DECIM 64
#define BPS_PDM_TAPS 640
#define BPS_PDM_BYTES (BPS_PDM_TAPS / 8)
#define BPS_PDM_HIST 128 /* bytes kept: a power of two, at least BPS_PDM_BYTES + 8 */
/* The filter's delay: half its length, in PDM bits. */
#define BPS_PDM_DELAY_BITS ((BPS_PDM_TAPS - 1) / 2.0)

typedef struct {
    int16_t tab[BPS_PDM_BYTES / 2][256];
    uint8_t rev[256];
    uint8_t ones_in[256]; /* bits set in each byte value */
    /* The bytes kept, twice over (each at i and i + BPS_PDM_HIST), so that
     * any window of them is contiguous. */
    uint8_t hist[2 * BPS_PDM_HIST];
    uint32_t w;    /* bytes taken in all */
    uint32_t ones; /* bits set, for the level check */
} bps_pdm;

/* Builds the tables (a few ms; floating point). `sample_rate` is the PCM rate. */
void bps_pdm_init(bps_pdm *f, uint32_t sample_rate);
/* Forgets the bits taken so far (the tables stay). */
void bps_pdm_reset(bps_pdm *f);
/* Takes `n` bytes of the bit stream, the earliest bit of each byte its most
 * significant; writes a PCM sample for every 8 bytes (64 bits) to `out`, and
 * returns how many. */
size_t bps_pdm_process(bps_pdm *f, const uint8_t *in, size_t n, int16_t *out);

#endif
