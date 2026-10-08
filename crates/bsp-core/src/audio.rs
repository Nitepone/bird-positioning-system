//! Audio containers and small DSP helpers (WAV codec, resampling, filtering, envelopes).

use anyhow::{Context, bail};
use bsp_proto::{ClientId, NANOS_PER_SEC, Timestamp};
use std::f32::consts::PI;
use std::io::Cursor;
use std::sync::Arc;

/// A mono chunk of PCM audio whose first sample was captured at `start`.
#[derive(Debug, Clone)]
pub struct AudioSample {
    pub client_id: ClientId,
    pub start: Timestamp,
    pub sample_rate: u32,
    pub pcm: Arc<[f32]>,
}

impl AudioSample {
    pub fn duration_ns(&self) -> i64 {
        self.pcm.len() as i64 * NANOS_PER_SEC / self.sample_rate as i64
    }

    pub fn end(&self) -> Timestamp {
        self.start + self.duration_ns()
    }

    /// Timestamp of the sample at `index`.
    pub fn time_at(&self, index: usize) -> Timestamp {
        self.start + index as i64 * NANOS_PER_SEC / self.sample_rate as i64
    }

    /// Fractional sample index of `ts` (may be out of range).
    pub fn index_of(&self, ts: Timestamp) -> f64 {
        (ts - self.start) as f64 * self.sample_rate as f64 / NANOS_PER_SEC as f64
    }
}

/// Decodes a WAV file to mono f32 samples (multi-channel input is averaged).
pub fn decode_wav(bytes: &[u8]) -> anyhow::Result<(u32, Vec<f32>)> {
    let mut reader = hound::WavReader::new(Cursor::new(bytes)).context("invalid WAV")?;
    let spec = reader.spec();
    let channels = spec.channels as usize;
    if channels == 0 {
        bail!("WAV has zero channels");
    }
    let interleaved: Vec<f32> = match spec.sample_format {
        hound::SampleFormat::Float => reader.samples::<f32>().collect::<Result<_, _>>()?,
        hound::SampleFormat::Int => {
            let scale = 1.0 / (1i64 << (spec.bits_per_sample - 1)) as f32;
            reader
                .samples::<i32>()
                .map(|s| s.map(|v| v as f32 * scale))
                .collect::<Result<_, _>>()?
        }
    };
    let mono = interleaved
        .chunks_exact(channels)
        .map(|f| f.iter().sum::<f32>() / channels as f32)
        .collect();
    Ok((spec.sample_rate, mono))
}

/// Encodes mono f32 samples as 16-bit PCM WAV.
pub fn encode_wav(sample_rate: u32, pcm: &[f32]) -> Vec<u8> {
    let spec = hound::WavSpec {
        channels: 1,
        sample_rate,
        bits_per_sample: 16,
        sample_format: hound::SampleFormat::Int,
    };
    let mut out = Cursor::new(Vec::with_capacity(44 + pcm.len() * 2));
    {
        let mut w = hound::WavWriter::new(&mut out, spec).expect("valid spec");
        for &s in pcm {
            w.write_sample((s.clamp(-1.0, 1.0) * i16::MAX as f32) as i16)
                .expect("write to memory");
        }
        w.finalize().expect("finalize to memory");
    }
    out.into_inner()
}

/// Band-limited resampling with a Hann-windowed sinc kernel.
pub fn resample(pcm: &[f32], from: u32, to: u32) -> Vec<f32> {
    if from == to || pcm.is_empty() {
        return pcm.to_vec();
    }
    const HALF_TAPS: isize = 16;
    let ratio = to as f64 / from as f64;
    let cutoff = ratio.min(1.0);
    let out_len = (pcm.len() as f64 * ratio).round() as usize;
    let mut out = Vec::with_capacity(out_len);
    for n in 0..out_len {
        let pos = n as f64 / ratio;
        let center = pos.floor() as isize;
        let mut acc = 0.0f64;
        let mut wsum = 0.0f64;
        for k in (center - HALF_TAPS + 1)..=(center + HALF_TAPS) {
            if k < 0 || k as usize >= pcm.len() {
                continue;
            }
            let x = pos - k as f64;
            let sinc = if x.abs() < 1e-9 {
                1.0
            } else {
                let a = std::f64::consts::PI * x * cutoff;
                a.sin() / a
            };
            let win = 0.5 + 0.5 * (std::f64::consts::PI * x / HALF_TAPS as f64).cos();
            let w = sinc * win * cutoff;
            acc += pcm[k as usize] as f64 * w;
            wsum += w;
        }
        // Normalising by the kernel sum keeps DC gain at 1 near the edges.
        out.push(if wsum.abs() > 1e-9 {
            (acc / wsum * cutoff) as f32
        } else {
            0.0
        });
    }
    out
}

/// RBJ biquad filter, usable for streaming.
#[derive(Debug, Clone)]
pub struct Biquad {
    b0: f32,
    b1: f32,
    b2: f32,
    a1: f32,
    a2: f32,
    z1: f32,
    z2: f32,
}

impl Biquad {
    fn new(b: [f32; 3], a: [f32; 3]) -> Self {
        Self {
            b0: b[0] / a[0],
            b1: b[1] / a[0],
            b2: b[2] / a[0],
            a1: a[1] / a[0],
            a2: a[2] / a[0],
            z1: 0.0,
            z2: 0.0,
        }
    }

    pub fn highpass(sample_rate: u32, freq: f32) -> Self {
        let (cos, alpha) = Self::params(sample_rate, freq);
        Self::new(
            [(1.0 + cos) / 2.0, -(1.0 + cos), (1.0 + cos) / 2.0],
            [1.0 + alpha, -2.0 * cos, 1.0 - alpha],
        )
    }

    pub fn lowpass(sample_rate: u32, freq: f32) -> Self {
        let (cos, alpha) = Self::params(sample_rate, freq);
        Self::new(
            [(1.0 - cos) / 2.0, 1.0 - cos, (1.0 - cos) / 2.0],
            [1.0 + alpha, -2.0 * cos, 1.0 - alpha],
        )
    }

    fn params(sample_rate: u32, freq: f32) -> (f32, f32) {
        let freq = freq.min(sample_rate as f32 * 0.45);
        let w0 = 2.0 * PI * freq / sample_rate as f32;
        let q = std::f32::consts::FRAC_1_SQRT_2;
        (w0.cos(), w0.sin() / (2.0 * q))
    }

    #[inline]
    pub fn process(&mut self, x: f32) -> f32 {
        // Transposed direct form II.
        let y = self.b0 * x + self.z1;
        self.z1 = self.b1 * x - self.a1 * y + self.z2;
        self.z2 = self.b2 * x - self.a2 * y;
        y
    }
}

/// Streaming band-pass made of a high-pass and low-pass biquad.
#[derive(Debug, Clone)]
pub struct BandPass {
    hp: Biquad,
    lp: Biquad,
}

impl BandPass {
    pub fn new(sample_rate: u32, low_hz: f32, high_hz: f32) -> Self {
        Self {
            hp: Biquad::highpass(sample_rate, low_hz),
            lp: Biquad::lowpass(sample_rate, high_hz),
        }
    }

    #[inline]
    pub fn process(&mut self, x: f32) -> f32 {
        self.lp.process(self.hp.process(x))
    }
}

/// Band-pass a whole buffer.
pub fn bandpass(pcm: &[f32], sample_rate: u32, low_hz: f32, high_hz: f32) -> Vec<f32> {
    let mut f = BandPass::new(sample_rate, low_hz, high_hz);
    pcm.iter().map(|&x| f.process(x)).collect()
}

/// RMS of consecutive non-overlapping frames (a trailing partial frame is included).
pub fn rms_frames(pcm: &[f32], frame_len: usize) -> Vec<f32> {
    pcm.chunks(frame_len.max(1))
        .map(|f| (f.iter().map(|x| x * x).sum::<f32>() / f.len() as f32).sqrt())
        .collect()
}

pub fn to_db(rms: f32) -> f32 {
    20.0 * rms.max(1e-9).log10()
}

/// The `p`-th percentile (0..=1) of `values`.
pub fn percentile(values: &[f32], p: f32) -> f32 {
    if values.is_empty() {
        return 0.0;
    }
    let mut v = values.to_vec();
    v.sort_by(|a, b| a.total_cmp(b));
    v[((v.len() - 1) as f32 * p.clamp(0.0, 1.0)).round() as usize]
}

#[cfg(test)]
pub(crate) mod test_util {
    use std::f32::consts::PI;

    /// Deterministic pseudo-random noise in [-1, 1].
    pub fn noise(len: usize, seed: u64) -> Vec<f32> {
        let mut s = seed
            .wrapping_mul(6364136223846793005)
            .wrapping_add(1442695040888963407);
        (0..len)
            .map(|_| {
                s ^= s << 13;
                s ^= s >> 7;
                s ^= s << 17;
                (s >> 40) as f32 / (1u64 << 23) as f32 - 1.0
            })
            .collect()
    }

    /// A chirp burst (2–6 kHz sweep) with a Hann envelope.
    pub fn chirp(sample_rate: u32, secs: f32) -> Vec<f32> {
        let n = (sample_rate as f32 * secs) as usize;
        (0..n)
            .map(|i| {
                let t = i as f32 / sample_rate as f32;
                let f = 2000.0 + 4000.0 * t / secs;
                let env = 0.5 - 0.5 * (2.0 * PI * i as f32 / n as f32).cos();
                env * (2.0 * PI * f * t).sin()
            })
            .collect()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn wav_roundtrip() {
        let pcm: Vec<f32> = (0..480).map(|i| (i as f32 / 10.0).sin() * 0.5).collect();
        let (sr, back) = decode_wav(&encode_wav(48000, &pcm)).unwrap();
        assert_eq!(sr, 48000);
        assert_eq!(back.len(), pcm.len());
        assert!(pcm.iter().zip(&back).all(|(a, b)| (a - b).abs() < 1e-3));
    }

    #[test]
    fn resample_preserves_tone() {
        let sr_in = 44100;
        let tone: Vec<f32> = (0..sr_in)
            .map(|i| (2.0 * PI * 1000.0 * i as f32 / sr_in as f32).sin())
            .collect();
        let out = resample(&tone, sr_in, 48000);
        assert_eq!(out.len(), 48000);
        for (i, &y) in out.iter().enumerate().skip(100).take(47800) {
            let expect = (2.0 * PI * 1000.0 * i as f32 / 48000.0).sin();
            assert!((y - expect).abs() < 0.02, "sample {i}: {y} vs {expect}");
        }
    }

    #[test]
    fn bandpass_rejects_low_freq() {
        let sr = 48000;
        let hum: Vec<f32> = (0..sr)
            .map(|i| (2.0 * PI * 60.0 * i as f32 / sr as f32).sin())
            .collect();
        let bird: Vec<f32> = (0..sr)
            .map(|i| (2.0 * PI * 4000.0 * i as f32 / sr as f32).sin())
            .collect();
        let r_hum = rms_frames(&bandpass(&hum, sr, 1000.0, 10000.0)[4800..], sr as usize)[0];
        let r_bird = rms_frames(&bandpass(&bird, sr, 1000.0, 10000.0)[4800..], sr as usize)[0];
        assert!(r_hum < 0.01, "{r_hum}");
        assert!(r_bird > 0.6, "{r_bird}");
    }
}
