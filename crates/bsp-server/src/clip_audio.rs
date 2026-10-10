//! Codec for stored detection clips: 16-bit FLAC, which compresses losslessly
//! and plays directly in browsers. Clips stored before FLAC are 16-bit WAV.

use anyhow::{Context, bail};
use bsp_core::audio;
use flacenc::component::BitRepr;
use flacenc::error::Verify;
use std::io::Cursor;

const BITS_PER_SAMPLE: usize = 16;

/// Encodes mono f32 samples as 16-bit FLAC, quantised exactly like
/// [`audio::encode_wav`]. Clips too short for FLAC stay WAV.
pub fn encode(sample_rate: u32, pcm: &[f32]) -> Vec<u8> {
    let samples: Vec<i32> = pcm
        .iter()
        .map(|&s| (s.clamp(-1.0, 1.0) * i16::MAX as f32) as i16 as i32)
        .collect();
    encode_ints(&samples, 1, BITS_PER_SAMPLE, sample_rate)
        .unwrap_or_else(|_| audio::encode_wav(sample_rate, pcm))
}

/// FLAC frames hold at least 16 samples, the last one included (decoders
/// reject shorter ones), so pick a block size that leaves no short remainder.
fn block_size(frames: usize) -> Option<usize> {
    if frames < MIN_BLOCK {
        return None;
    }
    (MIN_BLOCK..=4096)
        .rev()
        .find(|&bs| frames.is_multiple_of(bs) || frames % bs >= MIN_BLOCK)
}

const MIN_BLOCK: usize = 16;

/// Interleaved integer samples to FLAC.
fn encode_ints(
    samples: &[i32],
    channels: usize,
    bits_per_sample: usize,
    sample_rate: u32,
) -> anyhow::Result<Vec<u8>> {
    let Some(block_size) = block_size(samples.len() / channels) else {
        bail!("too few samples for FLAC");
    };
    let config = flacenc::config::Encoder::default()
        .into_verified()
        .map_err(|(_, e)| anyhow::anyhow!("FLAC config: {e:?}"))?;
    let source = flacenc::source::MemSource::from_samples(
        samples,
        channels,
        bits_per_sample,
        sample_rate as usize,
    );
    let stream = flacenc::encode_with_fixed_block_size(&config, source, block_size)
        .map_err(|e| anyhow::anyhow!("FLAC encode: {e:?}"))?;
    let mut sink = flacenc::bitsink::ByteSink::new();
    stream
        .write(&mut sink)
        .map_err(|e| anyhow::anyhow!("FLAC write: {e:?}"))?;
    Ok(sink.into_inner())
}

pub fn is_flac(bytes: &[u8]) -> bool {
    bytes.starts_with(b"fLaC")
}

pub fn is_wav(bytes: &[u8]) -> bool {
    bytes.starts_with(b"RIFF")
}

pub fn content_type(bytes: &[u8]) -> &'static str {
    if is_flac(bytes) {
        "audio/flac"
    } else {
        "audio/wav"
    }
}

/// Decodes a stored clip (FLAC or WAV) to mono f32 samples.
pub fn decode(bytes: &[u8]) -> anyhow::Result<(u32, Vec<f32>)> {
    if !is_flac(bytes) {
        return audio::decode_wav(bytes);
    }
    let mut reader = claxon::FlacReader::new(Cursor::new(bytes)).context("invalid FLAC")?;
    let info = reader.streaminfo();
    let channels = info.channels as usize;
    let scale = 1.0 / (1i64 << (info.bits_per_sample - 1)) as f32;
    let interleaved: Vec<i32> = reader.samples().collect::<Result<_, _>>()?;
    let mono = interleaved
        .chunks_exact(channels)
        .map(|f| f.iter().map(|&v| v as f32 * scale).sum::<f32>() / channels as f32)
        .collect();
    Ok((info.sample_rate, mono))
}

/// Re-encodes an integer PCM WAV as FLAC with identical samples.
pub fn flac_from_wav(wav: &[u8]) -> anyhow::Result<Vec<u8>> {
    let mut reader = hound::WavReader::new(Cursor::new(wav)).context("invalid WAV")?;
    let spec = reader.spec();
    if spec.sample_format != hound::SampleFormat::Int {
        bail!("float WAV cannot be stored losslessly as FLAC");
    }
    let samples: Vec<i32> = reader.samples::<i32>().collect::<Result<_, _>>()?;
    encode_ints(
        &samples,
        spec.channels as usize,
        spec.bits_per_sample as usize,
        spec.sample_rate,
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    fn chirp(n: usize) -> Vec<f32> {
        (0..n)
            .map(|i| {
                0.4 * ((i as f32).powi(2) * 1e-5).sin()
                    + 0.01 * ((i * 7919 % 101) as f32 / 101.0 - 0.5)
            })
            .collect()
    }

    #[test]
    fn flac_round_trip_matches_wav() {
        let pcm = chirp(48_000);
        let flac = encode(48_000, &pcm);
        let wav = audio::encode_wav(48_000, &pcm);
        assert!(is_flac(&flac) && flac.len() < wav.len());
        assert_eq!(content_type(&flac), "audio/flac");
        assert_eq!(content_type(&wav), "audio/wav");
        assert_eq!(decode(&flac).unwrap(), decode(&wav).unwrap());
    }

    #[test]
    fn converts_wav_losslessly() {
        let wav = audio::encode_wav(16_000, &chirp(10_000));
        let flac = flac_from_wav(&wav).unwrap();
        assert_eq!(decode(&flac).unwrap(), decode(&wav).unwrap());
    }

    #[test]
    fn encodes_any_length() {
        let bad: Vec<usize> = [0, 1, 15, 16, 100, 4097, 4100, 4096 + 15, 8192 + 3, 480_001]
            .into_iter()
            .filter(|&n| {
                decode(&encode(48_000, &chirp(n)))
                    .map(|(r, p)| (r, p.len()))
                    .ok()
                    != Some((48_000, n))
            })
            .collect();
        assert!(bad.is_empty(), "{bad:?}");
    }
}
