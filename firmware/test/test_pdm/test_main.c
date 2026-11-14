// PDM to PCM in software: a tone through a second-order sigma-delta
// modulator (as a PDM microphone's) comes out at its level and clean, silence
// as silence, and a tone above the band (it would fold into it) not at all.
#include <unity.h>

#include <math.h>
#include <string.h>

#include "pdm.h"

void setUp(void) {}
void tearDown(void) {}

#define RATE 48000
#define OUT 4800 /* 100 ms */

static bps_pdm f;
static uint8_t bits[OUT * BPS_PDM_DECIM / 8];
static int16_t pcm[OUT];

// Modulates amp * sin(2 pi hz t) into bits, earliest bit most significant.
static void modulate(double hz, double amp)
{
    double i1 = 0, i2 = 0, y = 1;
    double fs = (double)RATE * BPS_PDM_DECIM;
    memset(bits, 0, sizeof bits);
    for (size_t n = 0; n < sizeof bits * 8; n++) {
        double x = amp * sin(2.0 * M_PI * hz * (double)n / fs);
        i1 += x - y;
        i2 += i1 - y;
        y = i2 >= 0 ? 1.0 : -1.0;
        if (y > 0)
            bits[n / 8] |= (uint8_t)(0x80 >> (n % 8));
    }
}

// The level of `hz` in the output (full scale 1), and the RMS of what is
// left once it is taken out, past the filter's start.
static void measure(double hz, double *amp, double *rest)
{
    bps_pdm_init(&f, RATE);
    TEST_ASSERT_EQUAL_size_t(OUT, bps_pdm_process(&f, bits, sizeof bits, pcm));
    const int skip = 100;
    double s = 0, c = 0;
    int n = OUT - skip;
    for (int i = skip; i < OUT; i++) {
        double t = 2.0 * M_PI * hz * i / RATE;
        s += pcm[i] / 32768.0 * sin(t);
        c += pcm[i] / 32768.0 * cos(t);
    }
    s *= 2.0 / n;
    c *= 2.0 / n;
    *amp = sqrt(s * s + c * c);
    double e = 0;
    for (int i = skip; i < OUT; i++) {
        double t = 2.0 * M_PI * hz * i / RATE;
        double d = pcm[i] / 32768.0 - (s * sin(t) + c * cos(t));
        e += d * d;
    }
    *rest = sqrt(e / n);
}

static void test_a_tone_in_the_band_comes_out_at_its_level(void)
{
    double amp, rest;
    modulate(3000.0, 0.5);
    measure(3000.0, &amp, &rest);
    TEST_ASSERT_DOUBLE_WITHIN(0.01, 0.5, amp);
    // The modulator's noise, shaped above the band: well down.
    TEST_ASSERT_TRUE_MESSAGE(20 * log10(rest / amp) < -50.0, "too much noise");
    modulate(10000.0, 0.5);
    measure(10000.0, &amp, &rest);
    TEST_ASSERT_DOUBLE_WITHIN(0.01, 0.5, amp);
}

static void test_silence_is_silent(void)
{
    double amp, rest;
    modulate(1000.0, 0.0);
    measure(1000.0, &amp, &rest);
    TEST_ASSERT_TRUE_MESSAGE(20 * log10(rest + 1e-9) < -60.0, "silence is not silent");
}

static void test_a_tone_above_the_band_does_not_fold_into_it(void)
{
    double amp, rest;
    // 40 kHz would fold to 8 kHz.
    modulate(40000.0, 0.5);
    measure(8000.0, &amp, &rest);
    TEST_ASSERT_TRUE_MESSAGE(20 * log10(amp / 0.5 + 1e-9) < -60.0, "40 kHz folded into 8 kHz");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_tone_in_the_band_comes_out_at_its_level);
    RUN_TEST(test_silence_is_silent);
    RUN_TEST(test_a_tone_above_the_band_does_not_fold_into_it);
    return UNITY_END();
}
