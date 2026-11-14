#include "pdm.h"

#include <math.h>
#include <string.h>

#define CUTOFF_HZ 23000.0
#define KAISER_BETA 7.0
/* Tables hold taps times 4 * 32767: more precision, and a sum over a whole
 * window of ones (full scale) still fits after the shift back. */
#define TAB_SHIFT 2

static double bessel_i0(double x)
{
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 40; k++) {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
        if (term < 1e-12 * sum)
            break;
    }
    return sum;
}

void bps_pdm_init(bps_pdm *f, uint32_t sample_rate)
{
    memset(f, 0, sizeof *f);
    double fs = (double)sample_rate * BPS_PDM_DECIM;
    double fc = CUTOFF_HZ / fs;
    double m = (BPS_PDM_TAPS - 1) / 2.0;
    /* The taps (half: they are symmetric), in single precision: they go to
     * 16-bit tables. Static, not on the caller's stack. */
    static float h[BPS_PDM_TAPS / 2];
    double sum = 0.0;
    for (int i = 0; i < BPS_PDM_TAPS / 2; i++) {
        double t = i - m;
        double s = 2.0 * fc * (sin(2.0 * M_PI * fc * t) / (2.0 * M_PI * fc * t));
        double r = t / m;
        double w = bessel_i0(KAISER_BETA * sqrt(1.0 - r * r)) / bessel_i0(KAISER_BETA);
        h[i] = (float)(s * w);
        sum += 2.0 * h[i];
    }
    /* A DC gain of 1: all ones is +full scale, all zeros -full scale. */
    double scale = 32767.0 * (1 << TAB_SHIFT) / sum;
    for (int k = 0; k < BPS_PDM_BYTES / 2; k++)
        for (int b = 0; b < 256; b++) {
            double v = 0.0;
            for (int j = 0; j < 8; j++)
                v += h[8 * k + j] * ((b >> (7 - j)) & 1 ? 1.0 : -1.0);
            f->tab[k][b] = (int16_t)lround(v * scale);
        }
    for (int b = 0; b < 256; b++) {
        uint8_t r = 0;
        for (int j = 0; j < 8; j++)
            if (b & (1 << j))
                r |= (uint8_t)(0x80 >> j);
        f->rev[b] = r;
        f->ones_in[b] = (uint8_t)__builtin_popcount((unsigned)b);
    }
    /* Silence before the first bits: alternating ones and zeros. */
    memset(f->hist, 0x55, sizeof f->hist);
}

void bps_pdm_reset(bps_pdm *f)
{
    memset(f->hist, 0x55, sizeof f->hist);
    f->w = 0;
}

size_t bps_pdm_process(bps_pdm *f, const uint8_t *in, size_t n, int16_t *out)
{
    size_t made = 0;
    const uint32_t mask = BPS_PDM_HIST - 1;
    uint32_t ones = 0;
    for (size_t i = 0; i < n; i++) {
        uint32_t at = f->w & mask;
        f->hist[at] = f->hist[at + BPS_PDM_HIST] = in[i];
        ones += f->ones_in[in[i]];
        f->w++;
        if (f->w % (BPS_PDM_DECIM / 8))
            continue;
        const uint8_t *win = &f->hist[(f->w - BPS_PDM_BYTES) & mask];
        int32_t acc = 0;
        for (int k = 0; k < BPS_PDM_BYTES / 2; k++)
            acc += f->tab[k][win[k]];
        for (int k = 0; k < BPS_PDM_BYTES / 2; k++)
            acc += f->tab[k][f->rev[win[BPS_PDM_BYTES - 1 - k]]];
        acc >>= TAB_SHIFT;
        out[made++] = (int16_t)(acc > 32767 ? 32767 : acc < -32768 ? -32768 : acc);
    }
    f->ones += ones;
    return made;
}
