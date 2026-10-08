//! Turning audio into timestamped bird [`Call`]s.

use crate::audio::{self, AudioSample};
use bsp_proto::{ClientId, Timestamp};
use serde::{Deserialize, Serialize};

#[cfg(feature = "birdnet")]
pub mod birdnet;
pub mod local;
pub mod mock;

#[cfg(feature = "birdnet")]
pub use birdnet::{BirdNetConfig, BirdNetIdentifier};
pub use local::{GeoSpecies, LocalSpecies, LocalSpeciesFilter, RangeModel, Thresholds};
pub use mock::MockIdentifier;

#[derive(Debug, Clone, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub struct Species {
    pub scientific: String,
    pub common: String,
}

/// A single vocalisation heard by one client.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Call {
    pub client_id: ClientId,
    pub species: Species,
    pub confidence: f32,
    pub start: Timestamp,
    pub end: Timestamp,
    /// The species is not expected at this site (see [`LocalSpeciesFilter`]).
    #[serde(default)]
    pub unexpected: bool,
}

/// Something that can recognise bird calls in audio.
///
/// Implementations are synchronous and may be CPU heavy; callers in async
/// contexts should run them on a blocking thread.
pub trait Identifier: Send + Sync {
    fn name(&self) -> &str;
    fn identify(&self, sample: &AudioSample) -> anyhow::Result<Vec<Call>>;
}

/// Band used to measure call energy for onset refinement.
pub const BIRD_BAND_HZ: (f32, f32) = (1000.0, 10000.0);
const ENVELOPE_FRAME_MS: u32 = 10;

/// Narrows a coarse detection window (sample indices `[lo, hi)`) to the
/// dominant above-noise region inside it, using the band-passed energy
/// envelope. The noise floor is estimated over the whole sample. Returns the
/// refined `[start, end)` in sample indices.
///
/// Classifiers such as BirdNET only localise a call to its analysis window
/// (seconds); grouping calls across microphones needs ~10 ms onsets.
pub fn refine_call_bounds(pcm: &[f32], sample_rate: u32, lo: usize, hi: usize) -> (usize, usize) {
    let hi = hi.min(pcm.len());
    if lo >= hi {
        return (lo, hi);
    }
    let frame = (sample_rate * ENVELOPE_FRAME_MS / 1000) as usize;
    let filtered = audio::bandpass(pcm, sample_rate, BIRD_BAND_HZ.0, BIRD_BAND_HZ.1);
    let env: Vec<f32> = audio::rms_frames(&filtered, frame)
        .into_iter()
        .map(audio::to_db)
        .collect();
    let floor = audio::percentile(&env, 0.2);
    let (f_lo, f_hi) = (lo / frame, hi.div_ceil(frame).min(env.len()));
    let Some((peak_i, &peak)) = env[f_lo..f_hi]
        .iter()
        .enumerate()
        .max_by(|a, b| a.1.total_cmp(b.1))
        .map(|(i, v)| (i + f_lo, v))
    else {
        return (lo, hi);
    };
    if peak - floor < 3.0 {
        return (lo, hi);
    }
    let threshold = floor + ((peak - floor) * 0.3).max(3.0);
    let mut s = peak_i;
    while s > f_lo && env[s - 1] > threshold {
        s -= 1;
    }
    let mut e = peak_i + 1;
    while e < f_hi && env[e] > threshold {
        e += 1;
    }
    (s * frame, (e * frame).min(hi))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::audio::test_util::{chirp, noise};

    #[test]
    fn refine_finds_onset() {
        let sr = 48000;
        let mut pcm: Vec<f32> = noise(sr as usize * 3, 1).iter().map(|x| x * 0.01).collect();
        let call = chirp(sr, 0.3);
        let at = (1.234 * sr as f32) as usize;
        for (i, s) in call.iter().enumerate() {
            pcm[at + i] += s * 0.5;
        }
        let (s, e) = refine_call_bounds(&pcm, sr, 0, pcm.len());
        let tol = (0.05 * sr as f32) as usize;
        assert!(s.abs_diff(at) < tol, "start {s} vs {at}");
        assert!(e.abs_diff(at + call.len()) < tol, "end {e}");
    }
}
