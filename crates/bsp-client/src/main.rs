//! bsp client: captures audio from one microphone, drops chunks that are just
//! noise, and uploads the rest as timestamped WAV to the server.

mod capture;
mod gate;
mod keypress;
mod timesync;
mod uplink;

use anyhow::Context;
use bsp_proto::{Capabilities, ClientId, NANOS_PER_SEC, Timestamp, Uuid};
use capture::Block;
use clap::Parser;
use std::net::ToSocketAddrs;
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::sync::atomic::Ordering;
use std::time::Duration;
use uplink::{Session, Stats, UploadQueue};

#[derive(Parser)]
#[command(version, about = "bsp client: capture, filter and upload bird audio")]
struct Args {
    /// Server client-API URL (port 2473 by default; not the web UI port).
    #[arg(short, long, default_value = "http://127.0.0.1:2473")]
    server: String,
    /// Directory holding this client's persistent identity.
    #[arg(long, default_value = "state")]
    state_dir: PathBuf,
    /// Input device id or (partial) name; the default input when omitted.
    #[arg(short, long)]
    device: Option<String>,
    /// List input devices and exit.
    #[arg(long)]
    list_devices: bool,
    /// Replay a WAV file in real time instead of capturing (for testing).
    #[arg(long)]
    wav_file: Option<PathBuf>,
    /// Loop the WAV file forever.
    #[arg(long, requires = "wav_file")]
    repeat: bool,
    /// How the OS clock is disciplined (reported to the server).
    #[arg(long, default_value = "unknown")]
    clock_source: String,
    /// Upload every chunk, bypassing the noise gate.
    #[arg(long)]
    no_gate: bool,
}

fn load_or_create_id(dir: &Path) -> anyhow::Result<ClientId> {
    let path = dir.join("client_id");
    if let Ok(s) = std::fs::read_to_string(&path) {
        return s
            .trim()
            .parse()
            .with_context(|| format!("invalid id in {}", path.display()));
    }
    std::fs::create_dir_all(dir)?;
    let id = Uuid::new_v4();
    std::fs::write(&path, id.to_string()).with_context(|| format!("writing {}", path.display()))?;
    Ok(id)
}

fn hostname() -> String {
    std::fs::read_to_string("/etc/hostname")
        .map(|s| s.trim().to_string())
        .unwrap_or_else(|_| "unknown".into())
}

/// Accumulates contiguous blocks into fixed-length chunks; a timeline
/// discontinuity ends the current chunk early so every chunk is gap-free.
struct Chunker {
    sample_rate: u32,
    len: usize,
    start: Timestamp,
    pcm: Vec<f32>,
}

impl Chunker {
    /// Whatever is buffered, if it is long enough to be worth sending.
    fn flush(&mut self) -> Option<(Timestamp, Vec<f32>)> {
        (self.pcm.len() >= self.sample_rate as usize)
            .then(|| (self.start, std::mem::take(&mut self.pcm)))
    }

    fn push(&mut self, block: Block) -> Vec<(Timestamp, Vec<f32>)> {
        let mut out = Vec::new();
        let expected = self.start
            + (self.pcm.len() as i128 * NANOS_PER_SEC as i128 / self.sample_rate as i128) as i64;
        if !self.pcm.is_empty() && (block.ts - expected).abs() > NANOS_PER_SEC / 2000 {
            out.push((self.start, std::mem::take(&mut self.pcm)));
        }
        if self.pcm.is_empty() {
            self.start = block.ts;
        }
        self.pcm.extend_from_slice(&block.pcm);
        while self.pcm.len() >= self.len {
            let rest = self.pcm.split_off(self.len);
            out.push((self.start, std::mem::replace(&mut self.pcm, rest)));
            self.start +=
                (self.len as i128 * NANOS_PER_SEC as i128 / self.sample_rate as i128) as i64;
        }
        out
    }
}

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    tracing_subscriber::fmt()
        .with_env_filter(
            tracing_subscriber::EnvFilter::try_from_default_env().unwrap_or_else(|_| "info".into()),
        )
        .init();
    let args = Args::parse();
    if args.list_devices {
        return capture::list_devices();
    }

    let id = load_or_create_id(&args.state_dir)?;
    let stats = Arc::new(Stats::default());
    let (source, mut blocks) = match &args.wav_file {
        Some(path) => capture::start_wav(path, args.repeat)?,
        None => capture::start_live(args.device.as_deref(), stats.clone())?,
    };

    let base = args.server.trim_end_matches('/').to_string();
    let session = Arc::new(Session {
        http: reqwest::Client::builder()
            .timeout(Duration::from_secs(30))
            .build()?,
        base: base.clone(),
        id,
        hostname: hostname(),
        caps: Capabilities {
            sample_rate: source.sample_rate,
            channels: source.channels,
            sample_format: source.sample_format.clone(),
            clock_source: args.clock_source.clone(),
            device_name: source.device_name.clone(),
        },
    });
    tracing::info!(%id, "client starting; press Enter to request positioning");
    let reg = session.register_until_ok().await;

    // Clock measurement against the server's UDP port on the same host.
    let url = reqwest::Url::parse(&base)?;
    let host = url.host_str().context("server URL has no host")?;
    let udp_addr = (host, reg.udp_timesync_port)
        .to_socket_addrs()?
        .next()
        .context("cannot resolve server host")?;
    let clock = timesync::SharedClock::default();
    {
        let clock = clock.clone();
        tokio::spawn(async move {
            if let Err(e) = timesync::run(udp_addr, id, clock).await {
                tracing::error!("timesync stopped: {e:#}");
            }
        });
    }
    tokio::spawn(uplink::heartbeat_loop(
        session.clone(),
        Duration::from_secs(reg.heartbeat_interval_s.max(1) as u64),
        clock,
        stats.clone(),
    ));
    let queue = Arc::new(UploadQueue::new(50));
    {
        let (queue, session, stats) = (queue.clone(), session.clone(), stats.clone());
        tokio::spawn(async move { queue.run(session, stats).await });
    }
    keypress::spawn(session.clone());

    let mut gate = gate::NoiseGate::new(reg.gate.clone(), source.sample_rate);
    let mut chunker = Chunker {
        sample_rate: source.sample_rate,
        len: (reg.chunk_secs * source.sample_rate as f32) as usize,
        start: 0,
        pcm: Vec::new(),
    };
    loop {
        let chunks = match blocks.recv().await {
            Some(block) => chunker.push(block),
            None => match chunker.flush() {
                Some(c) => vec![c],
                None => break,
            },
        };
        for (start, pcm) in chunks {
            let g = gate.check(&pcm);
            if !args.no_gate && !g.pass {
                stats.chunks_gated.fetch_add(1, Ordering::Relaxed);
                tracing::debug!(snr = g.peak_snr_db, floor = g.floor_db, "chunk gated");
                continue;
            }
            tracing::debug!(snr = g.peak_snr_db, floor = g.floor_db, "chunk queued");
            let wav = bsp_core::audio::encode_wav(source.sample_rate, &pcm);
            queue.push(start, wav, &stats).await;
        }
    }
    // Source ended (WAV replay): let the queue drain.
    while stats.queue_len.load(Ordering::Relaxed) > 0 {
        tokio::time::sleep(Duration::from_millis(200)).await;
    }
    Ok(())
}
