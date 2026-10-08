//! TDOA solvers working in the local East/North/Up frame.

use super::{SPEED_OF_SOUND, Vec3};

/// Far-field plane-wave fit. `tdoas[i]` is arrival time at `mics[i]` minus at
/// the reference mic `mics[0]` (so `tdoas[0] == 0`). Returns the horizontal
/// unit vector pointing from the array towards the source (`[east, north]`)
/// and the RMS fit residual in seconds.
pub fn plane_wave(mics: &[Vec3], tdoas: &[f64]) -> Option<([f64; 2], f64)> {
    // t_i - t_0 = -((p_i - p_0) . u) / c  =>  A w = b with w = u / c.
    let rows: Vec<([f64; 2], f64)> = mics
        .iter()
        .zip(tdoas)
        .skip(1)
        .map(|(p, &t)| ([-(p.x - mics[0].x), -(p.y - mics[0].y)], t))
        .collect();
    let (mut ata, mut atb) = ([[0.0f64; 2]; 2], [0.0f64; 2]);
    for (a, b) in &rows {
        for i in 0..2 {
            atb[i] += a[i] * b;
            for j in 0..2 {
                ata[i][j] += a[i] * a[j];
            }
        }
    }
    let det = ata[0][0] * ata[1][1] - ata[0][1] * ata[1][0];
    let scale = ata[0][0] * ata[1][1];
    if rows.len() < 2 || det.abs() <= 1e-6 * scale.max(1e-12) {
        return None; // collinear array: mirror ambiguity
    }
    let w = [
        (ata[1][1] * atb[0] - ata[0][1] * atb[1]) / det,
        (ata[0][0] * atb[1] - ata[1][0] * atb[0]) / det,
    ];
    let norm = (w[0] * w[0] + w[1] * w[1]).sqrt();
    if norm < 1e-12 {
        return None;
    }
    let ss: f64 = rows
        .iter()
        .map(|(a, b)| (a[0] * w[0] + a[1] * w[1] - b).powi(2))
        .sum();
    Some(([w[0] / norm, w[1] / norm], (ss / rows.len() as f64).sqrt()))
}

/// RMS mismatch between measured TDOAs and those predicted for a source at `s`.
pub fn tdoa_residual(mics: &[Vec3], tdoas: &[f64], s: Vec3) -> f64 {
    let d0 = s.dist(mics[0]);
    let ss: f64 = mics
        .iter()
        .zip(tdoas)
        .skip(1)
        .map(|(p, &t)| ((s.dist(*p) - d0) / SPEED_OF_SOUND - t).powi(2))
        .sum();
    (ss / (mics.len() - 1).max(1) as f64).sqrt()
}

/// Near-field source position by coarse-to-fine grid search in the horizontal
/// plane (at the mean microphone height) within `radius` of the array centroid.
/// Returns the position, its residual, and whether it lies on the search edge.
pub fn grid_search(mics: &[Vec3], tdoas: &[f64], radius: f64) -> (Vec3, f64, bool) {
    let n = mics.len() as f64;
    let c = Vec3 {
        x: mics.iter().map(|p| p.x).sum::<f64>() / n,
        y: mics.iter().map(|p| p.y).sum::<f64>() / n,
        z: mics.iter().map(|p| p.z).sum::<f64>() / n,
    };
    const STEPS: i32 = 100;
    let (mut center, mut half) = (c, radius);
    let mut best = (c, f64::MAX);
    for _ in 0..6 {
        let step = 2.0 * half / STEPS as f64;
        for i in 0..=STEPS {
            for j in 0..=STEPS {
                let s = Vec3 {
                    x: center.x - half + i as f64 * step,
                    y: center.y - half + j as f64 * step,
                    z: c.z,
                };
                if s.dist(c) > radius {
                    continue;
                }
                let r = tdoa_residual(mics, tdoas, s);
                if r < best.1 {
                    best = (s, r);
                }
            }
        }
        center = best.0;
        half = step * 2.0;
    }
    let on_edge = best.0.dist(c) > radius * 0.97;
    (best.0, best.1, on_edge)
}
