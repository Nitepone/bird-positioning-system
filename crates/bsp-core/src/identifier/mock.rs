//! Energy-based stand-in identifier, so the pipeline can run without a model.

use super::{BIRD_BAND_HZ, Call, Identifier, Species};
use crate::audio::{self, AudioSample};

/// Reports every loud, band-limited burst as an "Unknown bird" call.
pub struct MockIdentifier {
    /// SNR over the noise floor needed to report a call.
    pub threshold_db: f32,
}

impl Default for MockIdentifier {
    fn default() -> Self {
        Self { threshold_db: 12.0 }
    }
}

const FRAME_MS: u32 = 10;
const MIN_CALL_MS: usize = 50;
const MAX_GAP_MS: usize = 100;

impl Identifier for MockIdentifier {
    fn name(&self) -> &str {
        "mock"
    }

    fn identify(&self, sample: &AudioSample) -> anyhow::Result<Vec<Call>> {
        let sr = sample.sample_rate;
        let frame = (sr * FRAME_MS / 1000) as usize;
        let filtered = audio::bandpass(&sample.pcm, sr, BIRD_BAND_HZ.0, BIRD_BAND_HZ.1);
        let env: Vec<f32> = audio::rms_frames(&filtered, frame)
            .into_iter()
            .map(audio::to_db)
            .collect();
        let floor = audio::percentile(&env, 0.2);

        // Collect above-threshold runs, bridging short gaps.
        let mut runs: Vec<(usize, usize, f32)> = Vec::new();
        for (i, &db) in env.iter().enumerate() {
            if db - floor < self.threshold_db {
                continue;
            }
            match runs.last_mut() {
                Some(r) if i - r.1 <= MAX_GAP_MS / FRAME_MS as usize => {
                    r.1 = i + 1;
                    r.2 = r.2.max(db - floor);
                }
                _ => runs.push((i, i + 1, db - floor)),
            }
        }
        let species = Species {
            scientific: "Aves sp.".into(),
            common: "Unknown bird".into(),
        };
        Ok(runs
            .into_iter()
            .filter(|r| (r.1 - r.0) * FRAME_MS as usize >= MIN_CALL_MS)
            .map(|(s, e, snr)| Call {
                client_id: sample.client_id,
                species: species.clone(),
                confidence: (snr / 40.0).clamp(0.1, 1.0),
                start: sample.time_at(s * frame),
                end: sample.time_at((e * frame).min(sample.pcm.len())),
                unexpected: false,
            })
            .collect())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::audio::test_util::{chirp, noise};
    use bsp_proto::{NANOS_PER_MS, Uuid};

    #[test]
    fn detects_bursts() {
        let sr = 48000;
        let mut pcm: Vec<f32> = noise(sr as usize * 4, 2).iter().map(|x| x * 0.01).collect();
        for at_s in [0.5f32, 2.5] {
            let at = (at_s * sr as f32) as usize;
            for (i, s) in chirp(sr, 0.4).iter().enumerate() {
                pcm[at + i] += s * 0.4;
            }
        }
        let sample = AudioSample {
            client_id: Uuid::nil(),
            start: 0,
            sample_rate: sr,
            pcm: pcm.into(),
        };
        let calls = MockIdentifier::default().identify(&sample).unwrap();
        assert_eq!(calls.len(), 2, "{calls:?}");
        assert!((calls[0].start - 500 * NANOS_PER_MS).abs() < 60 * NANOS_PER_MS);
        assert!((calls[1].start - 2500 * NANOS_PER_MS).abs() < 60 * NANOS_PER_MS);
    }

    #[test]
    fn ignores_noise() {
        let pcm: Vec<f32> = noise(48000 * 3, 3).iter().map(|x| x * 0.1).collect();
        let sample = AudioSample {
            client_id: Uuid::nil(),
            start: 0,
            sample_rate: 48000,
            pcm: pcm.into(),
        };
        assert!(
            MockIdentifier::default()
                .identify(&sample)
                .unwrap()
                .is_empty()
        );
    }
}
