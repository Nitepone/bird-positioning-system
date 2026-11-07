// The front end against its specification (trigger-models/tools/frontends.py
// and scipy's resample_poly), through vectors.h.
#include <unity.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "frontend.h"
#include "vectors.h"

void setUp(void) {}
void tearDown(void) {}

#define MARGIN 64

static int16_t *sig; // MARGIN zeros, VEC_N48 samples, MARGIN zeros

static void test_signal(void)
{
    sig = calloc(VEC_N48 + 2 * MARGIN, sizeof *sig);
    uint32_t state = 12345;
    for (uint32_t i = 0; i < VEC_N48; i++) {
        state = (state * 1103515245u + 12345u) & 0x7FFFFFFFu;
        int32_t noise = (int32_t)((state >> 16) & 2047) - 1024;
        uint32_t period = 6 + (i >> 13) % 9;
        int32_t tone = (i >> 14) & 1 ? ((i / period) % 2 ? 3000 : -3000) : 0;
        sig[MARGIN + i] = (int16_t)(noise + tone);
    }
}

static float max_err(const float *a, const float *b, uint32_t n)
{
    float e = 0;
    for (uint32_t i = 0; i < n; i++) {
        float d = fabsf(a[i] - b[i]);
        e = d > e ? d : e;
    }
    return e;
}

// The whole signal at 48/f kHz, as resample_poly gives it (zeros beyond its ends).
static float *decimate_all(uint32_t f, uint32_t *n_out)
{
    bps_decimator d;
    TEST_ASSERT_TRUE(bps_decimator_init(&d, f));
    *n_out = VEC_N48 / f;
    float *y = malloc(*n_out * sizeof *y);
    bps_decimate(&d, sig + MARGIN - bps_decimator_margin(&d), y, *n_out);
    return y;
}

static void test_decimators_match_resample_poly(void)
{
    uint32_t n;
    float *y = decimate_all(3, &n);
    TEST_ASSERT_FLOAT_WITHIN(2e-6f, 0.0f, max_err(y, dec3, 2000));
    TEST_ASSERT_FLOAT_WITHIN(2e-6f, 0.0f, max_err(y + n / 2, dec3_mid, 200));
    free(y);
    y = decimate_all(2, &n);
    TEST_ASSERT_FLOAT_WITHIN(2e-6f, 0.0f, max_err(y, dec2, 2000));
    TEST_ASSERT_FLOAT_WITHIN(2e-6f, 0.0f, max_err(y + n / 2, dec2_mid, 200));
    free(y);
    bps_decimator d;
    TEST_ASSERT_FALSE(bps_decimator_init(&d, 4));
}

static void check_logmel(uint32_t mels, const float *want)
{
    uint32_t n;
    float *y16 = decimate_all(3, &n);
    bps_fe_config c = {.kind = BPS_FE_LOGMEL_DB_MINMAX, .sample_rate = 16000, .window_samples = 48000,
                       .fft_length = 1024, .hop_length = 256, .frames = 184, .mel_bands = mels,
                       .mel_fmin_hz = 100.0f, .mel_fmax_hz = 8000.0f, .time_major = true};
    static bps_frontend f;
    TEST_ASSERT_TRUE(bps_frontend_init(&f, &c));
    uint32_t outs = bps_frontend_outputs(&f);
    TEST_ASSERT_EQUAL_UINT32(184 * mels, outs);
    float *out = malloc(outs * sizeof *out);
    bps_frontend_run(&f, y16 + 8000, out);
    float e = max_err(out, want, outs);
    char msg[64];
    snprintf(msg, sizeof msg, "%u bands: max error %g", (unsigned)mels, (double)e);
    TEST_ASSERT_TRUE_MESSAGE(e < 1e-3f, msg);
    free(out);
    free(y16);
    bps_frontend_free(&f);
}

static void test_logmel_16_bands(void) { check_logmel(16, logmel16); }
static void test_logmel_80_bands(void) { check_logmel(80, logmel80); }

static void test_stft_sqrt_minmax(void)
{
    uint32_t n;
    float *y24 = decimate_all(2, &n);
    bps_fe_config c = {.kind = BPS_FE_STFT_SQRT_MINMAX, .sample_rate = 24000, .window_samples = 60000,
                       .fft_length = 512, .frames = 384, .time_major = false};
    static bps_frontend f;
    TEST_ASSERT_TRUE(bps_frontend_init(&f, &c));
    uint32_t outs = bps_frontend_outputs(&f);
    TEST_ASSERT_EQUAL_UINT32(256 * 384, outs);
    float *out = malloc(outs * sizeof *out);
    bps_frontend_run(&f, y24 + 12000, out);
    double sum = 0;
    for (uint32_t i = 0; i < outs; i++)
        sum += out[i];
    TEST_ASSERT_DOUBLE_WITHIN(STFT_SUM * 1e-4, STFT_SUM, sum);
    float sampled[sizeof stft_sampled / sizeof stft_sampled[0]];
    for (uint32_t i = 0; i < sizeof sampled / sizeof sampled[0]; i++)
        sampled[i] = out[i * 97];
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.0f, max_err(sampled, stft_sampled, sizeof sampled / sizeof sampled[0]));
    free(out);
    free(y24);
    bps_frontend_free(&f);
}

static void test_rejects_bad_settings(void)
{
    static bps_frontend f;
    bps_fe_config c = {.kind = BPS_FE_LOGMEL_DB_MINMAX, .sample_rate = 16000, .window_samples = 48000,
                       .fft_length = 1000, .hop_length = 256, .frames = 184, .mel_bands = 16,
                       .mel_fmin_hz = 100.0f, .mel_fmax_hz = 8000.0f, .time_major = true};
    TEST_ASSERT_FALSE(bps_frontend_init(&f, &c)); // not a power of two
    c.fft_length = 1024;
    c.frames = 200; // more frames than the window holds
    TEST_ASSERT_FALSE(bps_frontend_init(&f, &c));
}

int main(void)
{
    test_signal();
    UNITY_BEGIN();
    RUN_TEST(test_decimators_match_resample_poly);
    RUN_TEST(test_logmel_16_bands);
    RUN_TEST(test_logmel_80_bands);
    RUN_TEST(test_stft_sqrt_minmax);
    RUN_TEST(test_rejects_bad_settings);
    return UNITY_END();
}
