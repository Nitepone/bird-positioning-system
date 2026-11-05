//! Codec for stored detection clips: 16-bit FLAC, which compresses losslessly
//! and plays directly in browsers. Clips stored before FLAC are 16-bit WAV.

use anyhow::{Context, bail};
use bsp_core::audio;
use bsp_proto::{NANOS_PER_SEC, Timestamp};
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

/// A stored clip as integer PCM WAV with identical samples (WAV clips are
/// returned unchanged). For browsers that cannot decode FLAC: Safari on iOS
/// has no FLAC decoder for Web Audio, but plays PCM WAV.
pub fn wav_from(bytes: Vec<u8>) -> anyhow::Result<Vec<u8>> {
    if !is_flac(&bytes) {
        return Ok(bytes);
    }
    let mut reader = claxon::FlacReader::new(Cursor::new(&bytes)).context("invalid FLAC")?;
    let info = reader.streaminfo();
    let spec = hound::WavSpec {
        channels: info.channels as u16,
        sample_rate: info.sample_rate,
        bits_per_sample: info.bits_per_sample as u16,
        sample_format: hound::SampleFormat::Int,
    };
    let frames = info.samples.unwrap_or(0) as usize;
    let mut out = Cursor::new(Vec::with_capacity(44 + frames * spec.channels as usize * 2));
    {
        let mut w = hound::WavWriter::new(&mut out, spec)?;
        for s in reader.samples() {
            w.write_sample(s?)?;
        }
        w.finalize()?;
    }
    Ok(out.into_inner())
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

/// One continuous recording of `from..to` pieced together from decoded clips
/// `(start, sample_rate, pcm)` of one client: a detection sample holding every
/// call heard in it. Clips of one client come from the same audio, so where
/// they overlap they agree; time no clip covers is silent. Clips at another
/// sample rate than the first are left out. `None` when there are no clips.
pub fn stitch(
    clips: &[(Timestamp, u32, Vec<f32>)],
    from: Timestamp,
    to: Timestamp,
) -> Option<(u32, Vec<f32>)> {
    let sample_rate = clips.first()?.1;
    let index = |t: Timestamp| {
        ((t - from) as i128 * sample_rate as i128).div_euclid(NANOS_PER_SEC as i128) as i64
    };
    let len = index(to).max(0) as usize;
    let mut out = vec![0.0f32; len];
    for (start, rate, pcm) in clips {
        if *rate != sample_rate {
            continue;
        }
        let offset = index(*start);
        for (i, &v) in pcm.iter().enumerate() {
            let j = offset + i as i64;
            if (0..len as i64).contains(&j) {
                out[j as usize] = v;
            }
        }
    }
    Some((sample_rate, out))
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

    /// Uploads from the microcontroller firmware's encoder (firmware/lib/bps_core/src/flac.c):
    /// the bytes its unit test pins, a 40-sample stream with a second-order predictor.
    #[test]
    fn decodes_the_firmware_encoder() {
        let flac: Vec<u8> = [
            0x66, 0x4c, 0x61, 0x43, 0x80, 0x00, 0x00, 0x22, 0x04, 0x00, 0x04, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x0b, 0xb8, 0x00, 0xf0, 0x00, 0x00, 0x00, 0x28, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0xff, 0xf8, 0x6a, 0x08, 0x00, 0x27, 0xd2, 0x14, 0x00, 0x00, 0x00, 0x64, 0x08, 0x3f,
            0xc3, 0xff, 0x4a, 0x00, 0x00, 0x33, 0x68, 0x56, 0xd0, 0xad, 0xa0, 0x26, 0x28, 0xc5,
            0x18, 0xa3, 0x8c, 0xca,
        ]
        .to_vec();
        let want: Vec<f32> = (0..40)
            .map(|i| if i < 20 { 100 * i } else { 2000 - 37 * (i - 20) + i % 3 })
            .map(|v| v as f32 / 32768.0)
            .collect();
        assert_eq!(decode(&flac).unwrap(), (48_000, want));
    }

    #[test]
    fn converts_flac_to_wav_losslessly() {
        let pcm = chirp(10_000);
        let flac = encode(16_000, &pcm);
        let wav = wav_from(flac.clone()).unwrap();
        assert!(is_wav(&wav));
        assert_eq!(content_type(&wav), "audio/wav");
        assert_eq!(decode(&wav).unwrap(), decode(&flac).unwrap());
        // Byte for byte what the WAV encoder writes for the same samples.
        assert_eq!(wav, audio::encode_wav(16_000, &pcm));
        // WAV clips pass through.
        assert_eq!(wav_from(wav.clone()).unwrap(), wav);
    }

    #[test]
    fn converts_wav_losslessly() {
        let wav = audio::encode_wav(16_000, &chirp(10_000));
        let flac = flac_from_wav(&wav).unwrap();
        assert_eq!(decode(&flac).unwrap(), decode(&wav).unwrap());
    }

    #[test]
    fn stitches_overlapping_clips_into_one_sample() {
        const S: i64 = NANOS_PER_SEC;
        // 10 samples/s; the source is 0.0, 0.1, 0.2, ... from t = 0.
        let src = |from: usize, n: usize| -> Vec<f32> {
            (from..from + n).map(|i| i as f32 / 10.0).collect()
        };
        let clips = vec![
            (S, 10, src(10, 20)),          // 1.0..3.0 s
            (2 * S, 10, src(20, 20)),      // 2.0..4.0 s, overlapping the first
            (6 * S, 10, src(60, 10)),      // 6.0..7.0 s, after a gap
            (2 * S, 16_000, vec![9.0; 5]), // another sample rate: ignored
        ];
        let (rate, pcm) = stitch(&clips, S, 8 * S).unwrap();
        assert_eq!(rate, 10);
        assert_eq!(pcm.len(), 70);
        let mut want: Vec<f32> = src(10, 30);
        want.extend([0.0; 20]);
        want.extend(src(60, 10));
        want.extend([0.0; 10]);
        assert_eq!(pcm, want);
        // Cropped to the requested span, which may start inside a clip.
        let (_, pcm) = stitch(&clips, 3 * S / 2, 2 * S).unwrap();
        assert_eq!(pcm, src(15, 5));
        assert!(stitch(&[], 0, S).is_none());
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
