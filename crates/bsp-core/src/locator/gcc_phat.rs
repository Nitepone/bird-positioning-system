//! Generalised cross-correlation with phase transform (GCC-PHAT) time-delay estimation.

use rustfft::FftPlanner;
use rustfft::num_complex::Complex32;

/// Result of a delay estimate.
#[derive(Debug, Clone, Copy)]
pub struct Delay {
    /// How many samples `sig` lags `reference` (sub-sample, may be negative).
    pub lag: f64,
    /// Peak height relative to the mean absolute correlation in the search range.
    pub sharpness: f32,
}

/// PHAT weighting exponent; < 1 keeps a little magnitude information, which is
/// more robust when the signal is not much louder than the noise.
const PHAT_BETA: f32 = 0.8;

/// Estimates the delay of `sig` relative to `reference` (equal sample rates),
/// searching lags within `±max_lag` samples and only using the `band_hz` range.
pub fn gcc_phat(
    reference: &[f32],
    sig: &[f32],
    sample_rate: u32,
    band_hz: (f32, f32),
    max_lag: usize,
) -> Option<Delay> {
    let n = reference.len().max(sig.len());
    if n == 0 {
        return None;
    }
    let size = (2 * n).next_power_of_two();
    let mut planner = FftPlanner::<f32>::new();
    let fwd = planner.plan_fft_forward(size);
    let inv = planner.plan_fft_inverse(size);

    let to_complex = |x: &[f32]| {
        let mut v: Vec<Complex32> = x.iter().map(|&r| Complex32::new(r, 0.0)).collect();
        v.resize(size, Complex32::default());
        v
    };
    let mut r = to_complex(reference);
    let mut s = to_complex(sig);
    fwd.process(&mut r);
    fwd.process(&mut s);

    let bin_hz = sample_rate as f32 / size as f32;
    let mut g: Vec<Complex32> = s
        .iter()
        .zip(&r)
        .enumerate()
        .map(|(k, (s, r))| {
            let freq = k.min(size - k) as f32 * bin_hz;
            if freq < band_hz.0 || freq > band_hz.1 {
                return Complex32::default();
            }
            let cross = s * r.conj();
            cross / (cross.norm().powf(PHAT_BETA) + 1e-12)
        })
        .collect();
    inv.process(&mut g);

    let max_lag = max_lag.min(size / 2 - 1) as isize;
    let at = |lag: isize| g[lag.rem_euclid(size as isize) as usize].re;
    let (mut best, mut best_v) = (0isize, f32::MIN);
    let mut sum_abs = 0.0f32;
    for lag in -max_lag..=max_lag {
        let v = at(lag);
        sum_abs += v.abs();
        if v > best_v {
            best_v = v;
            best = lag;
        }
    }
    let mean_abs = sum_abs / (2 * max_lag + 1) as f32;

    // Parabolic interpolation around the peak for sub-sample precision.
    let (y0, y1, y2) = (at(best - 1), best_v, at(best + 1));
    let denom = y0 - 2.0 * y1 + y2;
    let frac = if best.abs() < max_lag && denom.abs() > 1e-12 {
        0.5 * (y0 - y2) / denom
    } else {
        0.0
    };

    Some(Delay {
        lag: best as f64 + frac.clamp(-0.5, 0.5) as f64,
        sharpness: best_v / (mean_abs + 1e-12),
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::audio::test_util::{chirp, noise};

    #[test]
    fn recovers_integer_delay() {
        let sr = 48000;
        let len = sr as usize / 2;
        let call = chirp(sr, 0.2);
        let place = |at: usize, seed| {
            let mut v: Vec<f32> = noise(len, seed).iter().map(|x| x * 0.05).collect();
            for (i, s) in call.iter().enumerate() {
                v[at + i] += s;
            }
            v
        };
        let a = place(5000, 1);
        for delay in [-317isize, 0, 42, 900] {
            let b = place((5000 + delay) as usize, 2);
            let d = gcc_phat(&a, &b, sr, (1000.0, 10000.0), 2000).unwrap();
            assert!(
                (d.lag - delay as f64).abs() < 0.6,
                "delay {delay}: got {}",
                d.lag
            );
            assert!(d.sharpness > 5.0);
        }
    }
}
