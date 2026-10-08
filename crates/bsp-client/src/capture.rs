//! Audio sources producing timestamped mono blocks: a live input device (cpal)
//! or a WAV file replayed in real time.

use anyhow::{Context, bail};
use bsp_proto::{NANOS_PER_MS, NANOS_PER_SEC, Timestamp, now_ns};
use cpal::traits::{DeviceTrait, HostTrait, StreamTrait};
use cpal::{FromSample, SampleFormat, SizedSample};
use std::path::Path;
use std::sync::Arc;
use std::sync::atomic::Ordering;

use crate::uplink::Stats;
use std::time::Duration;
use tokio::sync::mpsc;

/// Mono samples whose first sample was captured at `ts`.
pub struct Block {
    pub ts: Timestamp,
    pub pcm: Vec<f32>,
}

pub struct SourceInfo {
    pub sample_rate: u32,
    pub channels: u16,
    pub sample_format: String,
    pub device_name: String,
}

/// Preferred capture rate (BirdNET's native rate).
const PREFERRED_RATE: u32 = 48000;

pub fn list_devices() -> anyhow::Result<()> {
    let host = cpal::default_host();
    let default = host.default_input_device().and_then(|d| d.id().ok());
    for dev in host.input_devices()? {
        let id = dev.id().map(|i| i.to_string()).unwrap_or_default();
        let name = dev
            .description()
            .map(|d| d.name().to_string())
            .unwrap_or_default();
        let mark = if dev.id().ok() == default { "*" } else { " " };
        println!("{mark} {id}  {name}");
        if let Ok(cfgs) = dev.supported_input_configs() {
            for c in cfgs {
                println!(
                    "      {}ch {:?} {}-{} Hz",
                    c.channels(),
                    c.sample_format(),
                    c.min_sample_rate(),
                    c.max_sample_rate()
                );
            }
        }
    }
    Ok(())
}

/// Converts per-callback wall-clock estimates into a smooth sample timeline.
///
/// Each callback yields a noisy estimate of when its first sample was captured
/// (scheduling jitter only ever makes it late). The timeline is
/// `anchor + n / sample_rate`; every `window` we take the *smallest* error seen
/// against that timeline and, if it exceeds `tolerance`, move the anchor by it.
/// This tracks ADC-vs-system clock drift and recovers from dropouts while
/// ignoring callback jitter.
pub struct Timestamper {
    sample_rate: u32,
    anchor: Option<Timestamp>,
    samples: u64,
    window_min_err: i64,
    window_samples: u64,
    tolerance_ns: i64,
    dropout_ns: i64,
}

impl Timestamper {
    pub fn new(sample_rate: u32) -> Self {
        Self {
            sample_rate,
            anchor: None,
            samples: 0,
            window_min_err: i64::MAX,
            window_samples: 0,
            tolerance_ns: NANOS_PER_MS / 2,
            dropout_ns: 20 * NANOS_PER_MS,
        }
    }

    fn timeline(&self, samples: u64) -> Timestamp {
        self.anchor.unwrap_or(0)
            + (samples as i128 * NANOS_PER_SEC as i128 / self.sample_rate as i128) as i64
    }

    /// Returns the timestamp of the first of `n` new samples, plus whether a
    /// dropout (a large timeline correction) was detected.
    pub fn push(&mut self, estimate: Timestamp, n: usize) -> (Timestamp, bool) {
        self.anchor.get_or_insert(estimate);
        let ts = self.timeline(self.samples);
        self.window_min_err = self.window_min_err.min(estimate - ts);
        self.samples += n as u64;
        self.window_samples += n as u64;

        let mut dropout = false;
        if self.window_samples >= 2 * self.sample_rate as u64 {
            let err = self.window_min_err;
            if err.abs() > self.tolerance_ns {
                *self.anchor.as_mut().unwrap() += err;
                dropout = err.abs() > self.dropout_ns;
                if dropout {
                    tracing::warn!(
                        correction_ms = err as f64 / 1e6,
                        "capture timeline jumped (dropout?)"
                    );
                }
            }
            self.window_min_err = i64::MAX;
            self.window_samples = 0;
        }
        (ts, dropout)
    }
}

fn find_device(host: &cpal::Host, wanted: Option<&str>) -> anyhow::Result<cpal::Device> {
    let Some(wanted) = wanted else {
        return host
            .default_input_device()
            .context("no default input device");
    };
    for dev in host.input_devices()? {
        let id = dev.id().map(|i| i.to_string()).unwrap_or_default();
        let name = dev
            .description()
            .map(|d| d.name().to_string())
            .unwrap_or_default();
        if id == wanted || name.contains(wanted) {
            return Ok(dev);
        }
    }
    bail!("no input device matching {wanted:?} (see --list-devices)")
}

fn choose_config(dev: &cpal::Device) -> anyhow::Result<cpal::SupportedStreamConfig> {
    let usable = [SampleFormat::F32, SampleFormat::I16, SampleFormat::I32];
    let mut best: Option<(u32, cpal::SupportedStreamConfig)> = None;
    for range in dev.supported_input_configs()? {
        if !usable.contains(&range.sample_format()) {
            continue;
        }
        let Some(cfg) = range.try_with_sample_rate(PREFERRED_RATE) else {
            continue;
        };
        // Prefer mono, then float.
        let score =
            (cfg.channels() == 1) as u32 * 2 + (cfg.sample_format() == SampleFormat::F32) as u32;
        if best.as_ref().is_none_or(|(s, _)| score > *s) {
            best = Some((score, cfg));
        }
    }
    match best {
        Some((_, cfg)) => Ok(cfg),
        None => Ok(dev.default_input_config()?),
    }
}

/// Live capture. The cpal stream lives on its own thread for the life of the process.
pub fn start_live(
    device: Option<&str>,
    stats: Arc<Stats>,
) -> anyhow::Result<(SourceInfo, mpsc::UnboundedReceiver<Block>)> {
    let host = cpal::default_host();
    let dev = find_device(&host, device)?;
    let cfg = choose_config(&dev)?;
    let info = SourceInfo {
        sample_rate: cfg.sample_rate(),
        channels: cfg.channels(),
        sample_format: format!("{:?}", cfg.sample_format()),
        device_name: dev
            .description()
            .map(|d| d.name().to_string())
            .unwrap_or_else(|_| "unknown".into()),
    };
    tracing::info!(device = %info.device_name, rate = info.sample_rate, channels = info.channels,
                   format = %info.sample_format, "capturing");

    let (tx, rx) = mpsc::unbounded_channel();
    let (ready_tx, ready_rx) = std::sync::mpsc::channel();
    std::thread::Builder::new()
        .name("capture".into())
        .spawn(move || {
            let result = match cfg.sample_format() {
                SampleFormat::F32 => build::<f32>(&dev, &cfg, tx, stats),
                SampleFormat::I16 => build::<i16>(&dev, &cfg, tx, stats),
                SampleFormat::I32 => build::<i32>(&dev, &cfg, tx, stats),
                f => Err(anyhow::anyhow!("unsupported sample format {f:?}")),
            };
            match result.and_then(|s| s.play().map(|_| s).map_err(Into::into)) {
                Ok(stream) => {
                    let _ = ready_tx.send(Ok(()));
                    let _keep = stream;
                    loop {
                        std::thread::park();
                    }
                }
                Err(e) => {
                    let _ = ready_tx.send(Err(e));
                }
            }
        })?;
    ready_rx.recv().context("capture thread exited")??;
    Ok((info, rx))
}

fn build<T>(
    dev: &cpal::Device,
    cfg: &cpal::SupportedStreamConfig,
    tx: mpsc::UnboundedSender<Block>,
    stats: Arc<Stats>,
) -> anyhow::Result<cpal::Stream>
where
    T: SizedSample + Send + 'static,
    f32: FromSample<T>,
{
    let channels = cfg.channels() as usize;
    let mut stamper = Timestamper::new(cfg.sample_rate());
    let stream = dev.build_input_stream(
        cfg.config(),
        move |data: &[T], info: &cpal::InputCallbackInfo| {
            let t = info.timestamp();
            // How long ago the first sample of this buffer was captured.
            let delay = t.callback.duration_since(t.capture).as_nanos() as i64;
            let pcm: Vec<f32> = data
                .chunks_exact(channels)
                .map(|f| {
                    f.iter()
                        .map(|&s| <f32 as FromSample<T>>::from_sample_(s))
                        .sum::<f32>()
                        / channels as f32
                })
                .collect();
            let (ts, dropout) = stamper.push(now_ns() - delay, pcm.len());
            if dropout {
                stats.dropouts.fetch_add(1, Ordering::Relaxed);
            }
            let _ = tx.send(Block { ts, pcm });
        },
        |e| tracing::warn!("capture stream error: {e}"),
        None,
    )?;
    Ok(stream)
}

/// Replays a WAV file in real time as if it were being captured now.
pub fn start_wav(
    path: &Path,
    repeat: bool,
) -> anyhow::Result<(SourceInfo, mpsc::UnboundedReceiver<Block>)> {
    let bytes = std::fs::read(path).with_context(|| format!("reading {}", path.display()))?;
    let (sample_rate, pcm) = bsp_core::audio::decode_wav(&bytes)?;
    let info = SourceInfo {
        sample_rate,
        channels: 1,
        sample_format: "wav".into(),
        device_name: format!("wav:{}", path.display()),
    };
    let (tx, rx) = mpsc::unbounded_channel();
    tokio::spawn(async move {
        let block = (sample_rate / 10) as usize;
        let start = now_ns();
        let mut sent = 0u64;
        loop {
            for chunk in pcm.chunks(block) {
                let ts =
                    start + (sent as i128 * NANOS_PER_SEC as i128 / sample_rate as i128) as i64;
                sent += chunk.len() as u64;
                // Deliver the block once it has "finished recording".
                let ready =
                    start + (sent as i128 * NANOS_PER_SEC as i128 / sample_rate as i128) as i64;
                let wait = ready - now_ns();
                if wait > 0 {
                    tokio::time::sleep(Duration::from_nanos(wait as u64)).await;
                }
                if tx
                    .send(Block {
                        ts,
                        pcm: chunk.to_vec(),
                    })
                    .is_err()
                {
                    return;
                }
            }
            if !repeat {
                tracing::info!("WAV replay finished");
                return;
            }
        }
    });
    Ok((info, rx))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn timestamper_ignores_jitter_and_tracks_jumps() {
        let sr = 48000;
        let block = 480; // 10 ms
        let mut ts = Timestamper::new(sr);
        let t0 = 1_000 * NANOS_PER_SEC;
        let mut first = None;
        for i in 0..1000u64 {
            let truth = t0 + i as i64 * 10 * NANOS_PER_MS;
            // Late by 0..3 ms of scheduling jitter, plus a 50 ms dropout after 5 s.
            let jitter = ((i * 7919) % 30) as i64 * NANOS_PER_MS / 10;
            let gap = if i >= 500 { 50 * NANOS_PER_MS } else { 0 };
            let (stamp, _) = ts.push(truth + gap + jitter, block);
            first.get_or_insert(stamp);
            if i < 500 {
                assert!((stamp - truth).abs() <= 3 * NANOS_PER_MS);
            }
            if i > 800 {
                assert!(
                    (stamp - truth - gap).abs() <= NANOS_PER_MS,
                    "block {i}: off by {}",
                    stamp - truth - gap
                );
            }
        }
    }
}
