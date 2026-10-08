//! Estimating where a call came from, given the same call recorded by several
//! clients with known positions and synchronised clocks.

use crate::audio;
use crate::identifier::{BIRD_BAND_HZ, Call};
use bsp_proto::{ClientId, NANOS_PER_MS, NANOS_PER_SEC, Timestamp};
use serde::{Deserialize, Serialize};
use std::sync::Arc;

pub mod gcc_phat;
pub mod solve;

/// Metres per second at ~20 °C.
pub const SPEED_OF_SOUND: f64 = 343.0;
/// Minimum number of microphones needed to produce a location.
pub const MIN_MICS: usize = 3;

/// Position in local metres: x = East, y = North, z = Up.
#[derive(Debug, Clone, Copy, PartialEq, Default, Serialize, Deserialize)]
pub struct Vec3 {
    pub x: f64,
    pub y: f64,
    pub z: f64,
}

impl Vec3 {
    pub fn dist(self, o: Vec3) -> f64 {
        ((self.x - o.x).powi(2) + (self.y - o.y).powi(2) + (self.z - o.z).powi(2)).sqrt()
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum Cardinal {
    N,
    NE,
    E,
    SE,
    S,
    SW,
    W,
    NW,
}

impl Cardinal {
    /// `bearing_deg`: 0 = North, clockwise.
    pub fn from_bearing(bearing_deg: f64) -> Self {
        const ALL: [Cardinal; 8] = [
            Cardinal::N,
            Cardinal::NE,
            Cardinal::E,
            Cardinal::SE,
            Cardinal::S,
            Cardinal::SW,
            Cardinal::W,
            Cardinal::NW,
        ];
        ALL[((bearing_deg.rem_euclid(360.0) / 45.0).round() as usize) % 8]
    }
}

/// One microphone's view of a call.
#[derive(Debug, Clone)]
pub struct LocatorInput {
    pub client_id: ClientId,
    pub position: Vec3,
    pub call: Call,
    /// Timestamp of `pcm[0]`. The audio should cover the call plus some context.
    pub start: Timestamp,
    pub sample_rate: u32,
    pub pcm: Arc<[f32]>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Tdoa {
    pub client_id: ClientId,
    /// Arrival time relative to the reference client (s).
    pub seconds: f64,
    /// Correlation peak sharpness against the reference (`None` for the reference itself).
    pub sharpness: Option<f32>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct LocationEstimate {
    /// Direction from the array centroid; 0 = North, clockwise, degrees.
    pub bearing_deg: f64,
    pub cardinal: Cardinal,
    /// Rough near-field position, when the geometry pins it down.
    pub position: Option<Vec3>,
    pub reference: ClientId,
    pub tdoas: Vec<Tdoa>,
    /// RMS TDOA fit residual of the bearing solution (s).
    pub residual_s: f64,
}

pub trait Locator: Send + Sync {
    fn locate(&self, inputs: &[LocatorInput]) -> Option<LocationEstimate>;
}

/// TDOA locator: aligns recordings on absolute time, measures the remaining
/// offsets with GCC-PHAT, then fits a bearing (and, if possible, a position).
#[derive(Debug, Clone)]
pub struct TdoaLocator {
    /// Context added around the calls before correlating.
    pub pad_ns: i64,
    /// Allowed clock error between clients, widening the lag search.
    pub clock_tolerance_s: f64,
    /// Search radius for the near-field position.
    pub max_range_m: f64,
    /// Positions with a worse residual are discarded.
    pub max_position_residual_s: f64,
}

impl Default for TdoaLocator {
    fn default() -> Self {
        Self {
            pad_ns: 200 * NANOS_PER_MS,
            clock_tolerance_s: 0.002,
            max_range_m: 300.0,
            max_position_residual_s: 0.0005,
        }
    }
}

const WORK_RATE: u32 = 48000;

/// Extracts `len` samples at `WORK_RATE` starting at absolute time `from`
/// (zero-filled where the input has no audio).
fn extract(input: &LocatorInput, from: Timestamp, len: usize) -> Vec<f32> {
    let margin = input.sample_rate as i64 / 20; // 50 ms, room for the resampler kernel
    let first = ((from - input.start) as i128 * input.sample_rate as i128 / NANOS_PER_SEC as i128)
        as i64
        - margin;
    let count =
        (len as u64 * input.sample_rate as u64).div_ceil(WORK_RATE as u64) as i64 + 2 * margin;
    let native: Vec<f32> = (first..first + count)
        .map(|i| {
            if i >= 0 && (i as usize) < input.pcm.len() {
                input.pcm[i as usize]
            } else {
                0.0
            }
        })
        .collect();
    let crop_start = input.start + first * NANOS_PER_SEC / input.sample_rate as i64;
    let res = audio::resample(&native, input.sample_rate, WORK_RATE);
    let skip = ((from - crop_start) as f64 * WORK_RATE as f64 / NANOS_PER_SEC as f64)
        .round()
        .max(0.0) as usize;
    let mut out: Vec<f32> = res.into_iter().skip(skip).take(len).collect();
    out.resize(len, 0.0);
    out
}

impl Locator for TdoaLocator {
    fn locate(&self, inputs: &[LocatorInput]) -> Option<LocationEstimate> {
        if inputs.len() < MIN_MICS {
            return None;
        }
        let from = inputs.iter().map(|i| i.call.start).min()? - self.pad_ns;
        let to = inputs.iter().map(|i| i.call.end).max()? + self.pad_ns;
        let len = ((to - from) as i128 * WORK_RATE as i128 / NANOS_PER_SEC as i128) as usize;
        let segs: Vec<Vec<f32>> = inputs.iter().map(|i| extract(i, from, len)).collect();

        // Loudest recording is the reference.
        let energy = |s: &[f32]| s.iter().map(|x| x * x).sum::<f32>();
        let r = (0..segs.len()).max_by(|&a, &b| energy(&segs[a]).total_cmp(&energy(&segs[b])))?;
        let mut order: Vec<usize> = vec![r];
        order.extend((0..inputs.len()).filter(|&i| i != r));

        let mut mics = Vec::new();
        let mut tdoas = Vec::new();
        for &i in &order {
            let (seconds, sharpness) = if i == r {
                (0.0, None)
            } else {
                let max_lag_s = inputs[i].position.dist(inputs[r].position) / SPEED_OF_SOUND
                    + self.clock_tolerance_s;
                let d = gcc_phat::gcc_phat(
                    &segs[r],
                    &segs[i],
                    WORK_RATE,
                    BIRD_BAND_HZ,
                    (max_lag_s * WORK_RATE as f64).ceil() as usize,
                )?;
                (d.lag / WORK_RATE as f64, Some(d.sharpness))
            };
            mics.push(inputs[i].position);
            tdoas.push(Tdoa {
                client_id: inputs[i].client_id,
                seconds,
                sharpness,
            });
        }
        let t: Vec<f64> = tdoas.iter().map(|t| t.seconds).collect();

        let (dir, residual_s) = solve::plane_wave(&mics, &t)?;
        let bearing_deg = dir[0].atan2(dir[1]).to_degrees().rem_euclid(360.0);
        let (pos, pos_residual, on_edge) = solve::grid_search(&mics, &t, self.max_range_m);
        let position = (!on_edge && pos_residual <= self.max_position_residual_s).then_some(pos);

        Some(LocationEstimate {
            bearing_deg,
            cardinal: Cardinal::from_bearing(bearing_deg),
            position,
            reference: inputs[r].client_id,
            tdoas,
            residual_s,
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::audio::test_util::{chirp, noise};
    use crate::identifier::Species;
    use bsp_proto::Uuid;

    fn simulate(mics: &[Vec3], source: Vec3, sample_rate: u32) -> Vec<LocatorInput> {
        let call = chirp(48000, 0.25);
        let t0 = 1_000 * NANOS_PER_SEC; // arbitrary epoch
        let emit = 0.4; // s after t0, at the nearest mic
        let nearest = mics.iter().map(|p| p.dist(source)).fold(f64::MAX, f64::min);
        mics.iter()
            .enumerate()
            .map(|(k, &p)| {
                let arrival = emit + (p.dist(source) - nearest) / SPEED_OF_SOUND;
                let mut pcm: Vec<f32> = noise(48000 * 2, k as u64 + 10)
                    .iter()
                    .map(|x| x * 0.02)
                    .collect();
                let at = (arrival * 48000.0).round() as usize;
                for (i, s) in call.iter().enumerate() {
                    pcm[at + i] += s * 0.5;
                }
                let pcm = audio::resample(&pcm, 48000, sample_rate);
                let start = t0 + (arrival * NANOS_PER_SEC as f64) as i64;
                LocatorInput {
                    client_id: Uuid::from_u128(k as u128),
                    position: p,
                    call: Call {
                        client_id: Uuid::from_u128(k as u128),
                        species: Species {
                            scientific: "x".into(),
                            common: "x".into(),
                        },
                        confidence: 1.0,
                        // Coarse, slightly wrong onsets, as an identifier would report.
                        start: start + (k as i64 - 1) * 30 * NANOS_PER_MS,
                        end: start + 250 * NANOS_PER_MS,
                        unexpected: false,
                    },
                    start: t0,
                    sample_rate,
                    pcm: pcm.into(),
                }
            })
            .collect()
    }

    fn ang_diff(a: f64, b: f64) -> f64 {
        let d = (a - b).rem_euclid(360.0);
        d.min(360.0 - d)
    }

    #[test]
    fn far_field_bearing() {
        let mics = [
            Vec3::default(),
            Vec3 {
                x: 30.0,
                y: 0.0,
                z: 0.0,
            },
            Vec3 {
                x: 5.0,
                y: 25.0,
                z: 0.0,
            },
        ];
        for bearing in [0.0f64, 60.0, 135.0, 200.0, 290.0] {
            let b = bearing.to_radians();
            let src = Vec3 {
                x: 2000.0 * b.sin(),
                y: 2000.0 * b.cos(),
                z: 0.0,
            };
            let est = TdoaLocator::default()
                .locate(&simulate(&mics, src, 48000))
                .unwrap();
            assert!(
                ang_diff(est.bearing_deg, bearing) < 5.0,
                "{bearing}: {est:?}"
            );
            assert_eq!(est.cardinal, Cardinal::from_bearing(bearing));
        }
    }

    #[test]
    fn near_field_position() {
        let mics = [
            Vec3::default(),
            Vec3 {
                x: 40.0,
                y: 0.0,
                z: 0.0,
            },
            Vec3 {
                x: 0.0,
                y: 40.0,
                z: 0.0,
            },
            Vec3 {
                x: 40.0,
                y: 40.0,
                z: 0.0,
            },
        ];
        let src = Vec3 {
            x: 25.0,
            y: 60.0,
            z: 0.0,
        };
        let est = TdoaLocator::default()
            .locate(&simulate(&mics, src, 44100))
            .unwrap();
        let pos = est.position.expect("position");
        assert!(pos.dist(src) < 3.0, "{pos:?}");
        assert_eq!(est.cardinal, Cardinal::N);
    }

    #[test]
    fn too_few_mics() {
        let mics = [
            Vec3::default(),
            Vec3 {
                x: 30.0,
                y: 0.0,
                z: 0.0,
            },
        ];
        let src = Vec3 {
            x: 100.0,
            y: 100.0,
            z: 0.0,
        };
        assert!(
            TdoaLocator::default()
                .locate(&simulate(&mics, src, 48000))
                .is_none()
        );
    }
}
