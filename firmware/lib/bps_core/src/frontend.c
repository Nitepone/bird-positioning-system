#include "frontend.h"

/* The firmware builds for size (-Os); this is the hot loop of validation. */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")
#endif

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define PI_D 3.14159265358979323846

/* ---- Decimation (scipy.signal.resample_poly, up = 1) ---- */

/* Modified Bessel function of the first kind, order 0 (series). */
static double bessel_i0(double x)
{
    double sum = 1.0, term = 1.0, q = x * x / 4.0;
    for (int k = 1; k < 50; k++) {
        term *= q / ((double)k * (double)k);
        sum += term;
        if (term < sum * 1e-17)
            break;
    }
    return sum;
}

bool bps_decimator_init(bps_decimator *d, uint32_t factor)
{
    if (factor != 2 && factor != 3)
        return false;
    memset(d, 0, sizeof *d);
    d->factor = factor;
    d->half_len = 10 * factor;
    d->taps = 2 * d->half_len + 1;
    /* firwin(taps, 1 / factor, window=('kaiser', 5.0)), scaled to unit DC gain. */
    double cutoff = 1.0 / factor, alpha = (d->taps - 1) / 2.0, beta = 5.0, sum = 0.0;
    double h[2 * 10 * 3 + 1];
    for (uint32_t n = 0; n < d->taps; n++) {
        double m = n - alpha, a = PI_D * cutoff * m;
        double sinc = m == 0 ? 1.0 : sin(a) / a;
        double r = 2.0 * n / (d->taps - 1) - 1.0;
        double w = bessel_i0(beta * sqrt(1.0 - r * r)) / bessel_i0(beta);
        h[n] = cutoff * sinc * w;
        sum += h[n];
    }
    for (uint32_t n = 0; n < d->taps; n++)
        d->h[n] = (float)(h[n] / sum);
    return true;
}

void bps_decimate(const bps_decimator *d, const int16_t *in, float *out, uint32_t n_out)
{
    /* y[n] = sum_i h[i] x[n * factor + half_len - i], x[j] = in[margin + j]. */
    const float scale = 1.0f / 32768.0f;
    for (uint32_t n = 0; n < n_out; n++) {
        const int16_t *x = in + d->half_len + n * d->factor + d->half_len;
        float acc = 0.0f;
        for (uint32_t i = 0; i < d->taps; i++)
            acc += d->h[i] * (float)x[-(int32_t)i];
        out[n] = acc * scale;
    }
}

/* ---- FFT: an N-point real FFT through an N/2-point complex one ---- */

static void fft_complex(float *z, const float *tw, uint32_t n)
{
    /* Bit reversal. */
    for (uint32_t i = 1, j = 0; i < n; i++) {
        uint32_t bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j) {
            float t = z[2 * i];
            z[2 * i] = z[2 * j];
            z[2 * j] = t;
            t = z[2 * i + 1];
            z[2 * i + 1] = z[2 * j + 1];
            z[2 * j + 1] = t;
        }
    }
    for (uint32_t len = 2; len <= n; len <<= 1) {
        uint32_t half = len >> 1, step = n / len;
        for (uint32_t i = 0; i < n; i += len)
            for (uint32_t k = 0; k < half; k++) {
                float wr = tw[2 * k * step], wi = tw[2 * k * step + 1];
                float *a = z + 2 * (i + k), *b = z + 2 * (i + k + half);
                float tr = b[0] * wr - b[1] * wi, ti = b[0] * wi + b[1] * wr;
                b[0] = a[0] - tr;
                b[1] = a[1] - ti;
                a[0] += tr;
                a[1] += ti;
            }
    }
}

/* Power |X[k]|^2 for k = 0..N/2 of the real frame in f->buf (N values); the
 * result overwrites f->buf[0..N/2]. */
static void power_spectrum(bps_frontend *f)
{
    uint32_t n = f->cfg.fft_length, h = n / 2;
    float *z = f->buf;
    fft_complex(z, f->twiddle, h);
    /* Zk = z[k], Zc = conj(z[h - k]); X[k] = (Zk + Zc)/2 + W^k (Zk - Zc)/(2i). */
    float p0 = (z[0] + z[1]) * (z[0] + z[1]), ph = (z[0] - z[1]) * (z[0] - z[1]);
    float tmp[BPS_FE_MAX_FFT / 2 + 1];
    tmp[0] = p0;
    tmp[h] = ph;
    for (uint32_t k = 1; k < h; k++) {
        float ar = z[2 * k], ai = z[2 * k + 1];
        float br = z[2 * (h - k)], bi = -z[2 * (h - k) + 1];
        float er = 0.5f * (ar + br), ei = 0.5f * (ai + bi);
        float dr = 0.5f * (ar - br), di = 0.5f * (ai - bi);
        /* O = d / i = (di, -dr) */
        float or_ = di, oi = -dr;
        float wr = f->split[2 * k], wi = f->split[2 * k + 1];
        float xr = er + wr * or_ - wi * oi, xi = ei + wr * oi + wi * or_;
        tmp[k] = xr * xr + xi * xi;
    }
    memcpy(f->buf, tmp, (h + 1) * sizeof(float));
}

/* ---- Mel filterbank (librosa.filters.mel, htk=False, norm='slaney') ---- */

static double hz_to_mel(double f)
{
    const double f_sp = 200.0 / 3, min_log_hz = 1000.0, logstep = log(6.4) / 27.0;
    return f >= min_log_hz ? min_log_hz / f_sp + log(f / min_log_hz) / logstep : f / f_sp;
}

static double mel_to_hz(double m)
{
    const double f_sp = 200.0 / 3, min_log_hz = 1000.0, logstep = log(6.4) / 27.0;
    double min_log_mel = min_log_hz / f_sp;
    return m >= min_log_mel ? min_log_hz * exp(logstep * (m - min_log_mel)) : f_sp * m;
}

static bool mel_init(bps_frontend *f)
{
    const bps_fe_config *c = &f->cfg;
    uint32_t bins = c->fft_length / 2 + 1, m_n = c->mel_bands;
    double pts[BPS_FE_MAX_MELS + 2];
    double lo = hz_to_mel(c->mel_fmin_hz), hi = hz_to_mel(c->mel_fmax_hz);
    for (uint32_t i = 0; i < m_n + 2; i++)
        pts[i] = mel_to_hz(lo + (hi - lo) * i / (m_n + 1));
    /* Two passes: the runs' lengths, then their weights. */
    float *w = NULL;
    for (int pass = 0; pass < 2; pass++) {
        uint32_t total = 0;
        for (uint32_t m = 0; m < m_n; m++) {
            double enorm = 2.0 / (pts[m + 2] - pts[m]);
            int32_t first = -1, last = -1;
            for (uint32_t b = 0; b < bins; b++) {
                double fr = (double)c->sample_rate / 2 * b / (bins - 1);
                double lower = (fr - pts[m]) / (pts[m + 1] - pts[m]);
                double upper = (pts[m + 2] - fr) / (pts[m + 2] - pts[m + 1]);
                double v = lower < upper ? lower : upper;
                if (v <= 0)
                    continue;
                if (first < 0)
                    first = (int32_t)b;
                last = (int32_t)b;
                if (pass)
                    w[total + (b - (uint32_t)first)] = (float)(v * enorm);
            }
            uint32_t len = first < 0 ? 0 : (uint32_t)(last - first + 1);
            if (pass) {
                /* Bins inside the run with zero weight stay zero. */
            } else {
                f->mel_start[m] = (uint16_t)(first < 0 ? 0 : first);
                f->mel_len[m] = (uint16_t)len;
            }
            total += len;
        }
        if (!pass) {
            w = calloc(total ? total : 1, sizeof *w);
            if (!w)
                return false;
        }
    }
    f->mel_w = w;
    return true;
}

/* ---- Front end ---- */

bool bps_frontend_init(bps_frontend *f, const bps_fe_config *cfg)
{
    memset(f, 0, sizeof *f);
    f->cfg = *cfg;
    uint32_t n = cfg->fft_length;
    if (n < 16 || n > BPS_FE_MAX_FFT || (n & (n - 1)) || !cfg->frames || !cfg->window_samples)
        return false;
    if (cfg->kind == BPS_FE_LOGMEL_DB_MINMAX &&
        (!cfg->hop_length || !cfg->mel_bands || cfg->mel_bands > BPS_FE_MAX_MELS ||
         cfg->window_samples < n + (cfg->frames - 1) * cfg->hop_length ||
         !(cfg->mel_fmax_hz > cfg->mel_fmin_hz)))
        return false;
    for (uint32_t i = 0; i < n; i++)
        f->window[i] = (float)(0.5 - 0.5 * cos(2.0 * PI_D * i / n));
    for (uint32_t k = 0; k < n / 2; k++) {
        f->twiddle[2 * k] = (float)cos(-2.0 * PI_D * k / (n / 2));
        f->twiddle[2 * k + 1] = (float)sin(-2.0 * PI_D * k / (n / 2));
    }
    for (uint32_t k = 0; k <= n / 2; k++) {
        f->split[2 * k] = (float)cos(-2.0 * PI_D * k / n);
        f->split[2 * k + 1] = (float)sin(-2.0 * PI_D * k / n);
    }
    if (cfg->kind == BPS_FE_LOGMEL_DB_MINMAX)
        return mel_init(f);
    return true;
}

void bps_frontend_free(bps_frontend *f)
{
    free(f->mel_w);
    f->mel_w = NULL;
}

uint32_t bps_frontend_outputs(const bps_frontend *f)
{
    uint32_t bins = f->cfg.kind == BPS_FE_LOGMEL_DB_MINMAX ? f->cfg.mel_bands : f->cfg.fft_length / 2;
    return bins * f->cfg.frames;
}

static void minmax(float *v, uint32_t n, float eps)
{
    float lo = v[0], hi = v[0];
    for (uint32_t i = 1; i < n; i++) {
        lo = v[i] < lo ? v[i] : lo;
        hi = v[i] > hi ? v[i] : hi;
    }
    float s = 1.0f / (hi - lo + eps);
    for (uint32_t i = 0; i < n; i++)
        v[i] = (v[i] - lo) * s;
}

static void run_logmel(bps_frontend *f, const float *x, float *out)
{
    const bps_fe_config *c = &f->cfg;
    uint32_t n = c->fft_length, m_n = c->mel_bands, frames = c->frames;
    float top = 0.0f;
    for (uint32_t t = 0; t < frames; t++) {
        const float *fr = x + t * c->hop_length;
        for (uint32_t i = 0; i < n; i++)
            f->buf[i] = fr[i] * f->window[i];
        power_spectrum(f);
        const float *w = f->mel_w;
        for (uint32_t m = 0; m < m_n; m++) {
            const float *p = f->buf + f->mel_start[m];
            float acc = 0.0f;
            for (uint32_t k = 0; k < f->mel_len[m]; k++)
                acc += w[k] * p[k];
            w += f->mel_len[m];
            uint32_t at = c->time_major ? t * m_n + m : m * frames + t;
            out[at] = acc;
            top = acc > top ? acc : top;
        }
    }
    /* power_to_db(ref=max, amin=1e-10, top_db=80): 0 dB at the peak. */
    const float amin = 1e-10f;
    float ref_db = 10.0f * log10f(top > amin ? top : amin);
    uint32_t total = m_n * frames;
    for (uint32_t i = 0; i < total; i++) {
        float db = 10.0f * log10f(out[i] > amin ? out[i] : amin) - ref_db;
        out[i] = db < -80.0f ? -80.0f : db;
    }
    minmax(out, total, 1e-8f);
}

static void run_stft(bps_frontend *f, const float *x, float *out)
{
    const bps_fe_config *c = &f->cfg;
    uint32_t n = c->fft_length, bins = n / 2, frames = c->frames;
    uint32_t hop = c->window_samples / frames;
    for (uint32_t t = 0; t < frames; t++) {
        /* Centred: frame t covers x[t * hop - n/2, t * hop + n/2), zeros outside. */
        int32_t s0 = (int32_t)(t * hop) - (int32_t)(n / 2);
        for (uint32_t i = 0; i < n; i++) {
            int32_t j = s0 + (int32_t)i;
            float v = j >= 0 && j < (int32_t)c->window_samples ? x[j] : 0.0f;
            f->buf[i] = v * f->window[i];
        }
        power_spectrum(f);
        for (uint32_t b = 0; b < bins; b++) {
            float mag = sqrtf(f->buf[b]);
            uint32_t at = c->time_major ? t * bins + b : b * frames + t;
            out[at] = sqrtf(mag);
        }
    }
    minmax(out, bins * frames, 1e-10f);
}

void bps_frontend_run(bps_frontend *f, const float *x, float *out)
{
    if (f->cfg.kind == BPS_FE_LOGMEL_DB_MINMAX)
        run_logmel(f, x, out);
    else
        run_stft(f, x, out);
}
