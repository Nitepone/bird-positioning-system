//! Noise gate: only chunks with band-limited energy clearly above the
//! adaptive noise floor are worth sending to the server.

use bsp_core::audio::{self, BandPass};
use bsp_proto::GateConfig;

pub struct NoiseGate {
    cfg: GateConfig,
    sample_rate: u32,
    filter: BandPass,
    floor_db: Option<f32>,
}

#[derive(Debug, Clone, Copy)]
pub struct GateResult {
    pub pass: bool,
    pub peak_snr_db: f32,
    pub floor_db: f32,
}

/// Per-frame smoothing factors for the floor estimate.
const FALL_RATE: f32 = 0.3;
const QUIET_RISE_RATE: f32 = 0.05;
/// Lets the floor follow sustained loud backgrounds (rain, wind) slowly.
const LOUD_RISE_RATE: f32 = 0.002;

impl NoiseGate {
    pub fn new(cfg: GateConfig, sample_rate: u32) -> Self {
        let filter = BandPass::new(sample_rate, cfg.band_low_hz, cfg.band_high_hz);
        Self {
            cfg,
            sample_rate,
            filter,
            floor_db: None,
        }
    }

    pub fn check(&mut self, pcm: &[f32]) -> GateResult {
        let filtered: Vec<f32> = pcm.iter().map(|&x| self.filter.process(x)).collect();
        let frame = (self.sample_rate * self.cfg.frame_ms / 1000).max(1) as usize;
        let frames: Vec<f32> = audio::rms_frames(&filtered, frame)
            .into_iter()
            .map(audio::to_db)
            .collect();
        let mut floor = *self
            .floor_db
            .get_or_insert_with(|| audio::percentile(&frames, 0.2));
        let mut peak_snr = f32::MIN;
        for &db in &frames {
            let snr = db - floor;
            peak_snr = peak_snr.max(snr);
            let rate = if db < floor {
                FALL_RATE
            } else if snr < self.cfg.threshold_db {
                QUIET_RISE_RATE
            } else {
                LOUD_RISE_RATE
            };
            floor += (db - floor) * rate;
        }
        self.floor_db = Some(floor);
        GateResult {
            pass: peak_snr >= self.cfg.threshold_db,
            peak_snr_db: peak_snr,
            floor_db: floor,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::f32::consts::PI;

    fn noise(len: usize, seed: u32) -> Vec<f32> {
        let mut s = seed.wrapping_mul(2654435761) | 1;
        (0..len)
            .map(|_| {
                s ^= s << 13;
                s ^= s >> 17;
                s ^= s << 5;
                (s as f32 / u32::MAX as f32 - 0.5) * 0.02
            })
            .collect()
    }

    #[test]
    fn passes_tone_burst_not_noise() {
        let sr = 48000;
        let mut gate = NoiseGate::new(GateConfig::default(), sr);
        for k in 0..5 {
            assert!(
                !gate.check(&noise(sr as usize * 3, k)).pass,
                "noise chunk {k} passed"
            );
        }
        let mut chunk = noise(sr as usize * 3, 99);
        for i in 0..(sr as usize / 5) {
            chunk[sr as usize + i] += 0.2 * (2.0 * PI * 3500.0 * i as f32 / sr as f32).sin();
        }
        let r = gate.check(&chunk);
        assert!(r.pass, "{r:?}");
    }

    #[test]
    fn ignores_low_frequency_rumble() {
        let sr = 48000;
        let mut gate = NoiseGate::new(GateConfig::default(), sr);
        gate.check(&noise(sr as usize * 3, 1));
        let mut chunk = noise(sr as usize * 3, 2);
        for (i, s) in chunk.iter_mut().enumerate().skip(sr as usize) {
            *s += 0.5 * (2.0 * PI * 80.0 * i as f32 / sr as f32).sin();
        }
        assert!(!gate.check(&chunk).pass);
    }
}
