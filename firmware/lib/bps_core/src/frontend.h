/*
 * Model front ends for validation (validate.h): 48 kHz audio from the ring ->
 * a model's input. The specification is trigger-models/tools/frontends.py,
 * which the native tests check this against:
 *
 *   logmel_db_minmax  mel power spectrogram (no centring, periodic Hann,
 *                     Slaney mel), dB relative to its peak floored 80 dB
 *                     below, min-max to [0, 1]; frames x bands
 *   stft_sqrt_minmax  centred frames, periodic Hann, |FFT| without Nyquist,
 *                     square root, min-max to [0, 1]; bins x frames
 *
 * Resampling to the model's rate is scipy.signal.resample_poly's (Kaiser 5.0
 * FIR, 10 x the factor taps each side), for 48 kHz to 16 or 24 kHz.
 */
#ifndef BPS_FRONTEND_H
#define BPS_FRONTEND_H

#include "bps_common.h"

#define BPS_FE_MAX_FFT 1024
#define BPS_FE_MAX_MELS 128

typedef enum { BPS_FE_LOGMEL_DB_MINMAX, BPS_FE_STFT_SQRT_MINMAX } bps_fe_kind;

typedef struct {
    bps_fe_kind kind;
    uint32_t sample_rate;    /* the model's */
    uint32_t window_samples; /* at that rate */
    uint32_t fft_length;     /* power of two, at most BPS_FE_MAX_FFT */
    uint32_t hop_length;     /* logmel (stft: window_samples / frames) */
    uint32_t frames;
    uint32_t mel_bands;
    float mel_fmin_hz, mel_fmax_hz;
    bool time_major; /* output frames x bins (else bins x frames) */
} bps_fe_config;

/* Decimation from 48 kHz by an integer factor (2 or 3). */
typedef struct {
    uint32_t factor, half_len, taps;
    float h[2 * 10 * 3 + 1];
} bps_decimator;

/* False for factors other than 2 and 3. */
bool bps_decimator_init(bps_decimator *d, uint32_t factor);
/* Samples of 48 kHz input needed on each side of a window for its edges to
 * come out as from continuous audio. */
static inline uint32_t bps_decimator_margin(const bps_decimator *d) { return d->half_len; }
/* `out[n]` for n < n_out, from `in`, which holds `margin` samples before the
 * window's first and after its last; in[margin + n * factor] lines up with out[n]. */
void bps_decimate(const bps_decimator *d, const int16_t *in, float *out, uint32_t n_out);

typedef struct {
    bps_fe_config cfg;
    float window[BPS_FE_MAX_FFT];
    float twiddle[BPS_FE_MAX_FFT];   /* the N/2-point complex FFT's: cos, sin interleaved */
    float split[BPS_FE_MAX_FFT + 2]; /* exp(-2 pi i k / N), k = 0..N/2: cos, sin */
    float buf[BPS_FE_MAX_FFT + 2];
    /* Mel bands as runs of weights over the FFT bins. */
    uint16_t mel_start[BPS_FE_MAX_MELS], mel_len[BPS_FE_MAX_MELS];
    float *mel_w; /* all runs, back to back (allocated) */
} bps_frontend;

/* False if the settings are out of range or memory runs out. */
bool bps_frontend_init(bps_frontend *f, const bps_fe_config *cfg);
void bps_frontend_free(bps_frontend *f);
/* Values in the model input: frames x bins or bins x frames. */
uint32_t bps_frontend_outputs(const bps_frontend *f);
/* Computes the model input from `x` (window_samples at the model's rate)
 * into `out` (bps_frontend_outputs floats). */
void bps_frontend_run(bps_frontend *f, const float *x, float *out);

#endif
