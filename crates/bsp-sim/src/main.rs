//! Simulates a set of microphones hearing one bird at a known bearing, and
//! drives the server through the real client protocol. Useful for checking
//! the identification -> grouping -> location pipeline without hardware.

use anyhow::{Context, bail};
use bsp_core::audio;
use bsp_proto::{
    CLIENT_API_PREFIX, Capabilities, ClockStatus, HEADER_START_NS, Heartbeat, RegisterRequest,
    Uuid, now_ns,
};
use clap::Parser;
use serde_json::json;
use std::f64::consts::PI;
use std::path::PathBuf;
use std::time::Duration;

const SR: u32 = 48000;
const SPEED_OF_SOUND: f64 = 343.0;

#[derive(Parser)]
#[command(about = "Simulate several bsp clients hearing the same bird")]
struct Args {
    /// Server client-API URL.
    #[arg(short, long, default_value = "http://127.0.0.1:2473")]
    server: String,
    /// Web UI URL, for --set-positions. Its certificate is not verified (it is
    /// usually self-signed).
    #[arg(long, default_value = "https://127.0.0.1")]
    web: String,
    /// Microphone positions as "x,y[,z]" in metres (East, North, Up), separated by ';'.
    #[arg(long, default_value = "0,0;30,0;10,25")]
    mics: String,
    /// Direction of the bird from the first microphone, degrees clockwise from North.
    #[arg(long, default_value_t = 60.0)]
    bearing: f64,
    #[arg(long, default_value_t = 150.0)]
    distance: f64,
    /// Bird sound to use (first 2 s); a synthetic chirp when omitted.
    #[arg(long)]
    wav: Option<PathBuf>,
    /// Number of 9 s chunks to send.
    #[arg(long, default_value_t = 2)]
    rounds: u32,
    /// Also configure the virtual clients' names and positions on the server.
    #[arg(long)]
    set_positions: bool,
}

fn parse_mics(s: &str) -> anyhow::Result<Vec<[f64; 3]>> {
    s.split(';')
        .map(|m| {
            let v: Vec<f64> = m
                .split(',')
                .map(|x| x.trim().parse())
                .collect::<Result<_, _>>()?;
            match v[..] {
                [x, y] => Ok([x, y, 0.0]),
                [x, y, z] => Ok([x, y, z]),
                _ => bail!("bad microphone position {m:?}"),
            }
        })
        .collect()
}

fn noise(len: usize, seed: u64) -> Vec<f32> {
    let mut s = seed.wrapping_mul(6364136223846793005) | 1;
    (0..len)
        .map(|_| {
            s ^= s << 13;
            s ^= s >> 7;
            s ^= s << 17;
            ((s >> 40) as f32 / (1u64 << 23) as f32 - 1.0) * 0.01
        })
        .collect()
}

fn chirp() -> Vec<f32> {
    let n = SR as usize * 3 / 10;
    (0..n)
        .map(|i| {
            let t = i as f64 / SR as f64;
            let env = 0.5 - 0.5 * (2.0 * PI * i as f64 / n as f64).cos();
            (0.5 * env * (2.0 * PI * (2500.0 + 6000.0 * t) * t).sin()) as f32
        })
        .collect()
}

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    let args = Args::parse();
    let mics = parse_mics(&args.mics)?;
    let base = args.server.trim_end_matches('/');
    let http = reqwest::Client::new();
    let web_base = args.web.trim_end_matches('/');
    let web = reqwest::Client::builder()
        .danger_accept_invalid_certs(true)
        .build()?;

    let call = match &args.wav {
        Some(p) => {
            let (sr, pcm) =
                audio::decode_wav(&std::fs::read(p).with_context(|| p.display().to_string())?)?;
            let mut pcm = audio::resample(&pcm, sr, SR);
            pcm.truncate(2 * SR as usize);
            pcm
        }
        None => chirp(),
    };

    let b = args.bearing.to_radians();
    let src = [
        mics[0][0] + args.distance * b.sin(),
        mics[0][1] + args.distance * b.cos(),
        mics[0][2],
    ];
    let dist = |m: &[f64; 3]| {
        ((m[0] - src[0]).powi(2) + (m[1] - src[1]).powi(2) + (m[2] - src[2]).powi(2)).sqrt()
    };
    let nearest = mics.iter().map(dist).fold(f64::MAX, f64::min);
    let n = mics.len() as f64;
    let centroid = [
        mics.iter().map(|m| m[0]).sum::<f64>() / n,
        mics.iter().map(|m| m[1]).sum::<f64>() / n,
    ];
    let expected = (src[0] - centroid[0])
        .atan2(src[1] - centroid[1])
        .to_degrees()
        .rem_euclid(360.0);
    println!(
        "source at ({:.1}, {:.1}); expected bearing from array centroid {expected:.1} deg",
        src[0], src[1]
    );

    let ids: Vec<Uuid> = (0..mics.len())
        .map(|k| Uuid::from_u128(0xb5b0_0000_0000_0000_0000_0000_0000_0000 + k as u128))
        .collect();
    for (k, id) in ids.iter().enumerate() {
        let req = RegisterRequest {
            client_id: *id,
            hostname: format!("sim-{k}"),
            version: env!("CARGO_PKG_VERSION").into(),
            capabilities: Capabilities {
                sample_rate: SR,
                channels: 1,
                sample_format: "F32".into(),
                clock_source: "simulated".into(),
                device_name: "simulator".into(),
            },
            name: Some(format!("sim-{k}")),
        };
        http.post(format!("{base}{CLIENT_API_PREFIX}/register"))
            .json(&req)
            .send()
            .await?
            .error_for_status()?;
        if args.set_positions {
            let m = mics[k];
            web.put(format!("{web_base}/api/v1/control/clients/{id}"))
                .json(&json!({ "name": format!("sim-{k}"), "position": { "x": m[0], "y": m[1], "z": m[2] } }))
                .send()
                .await?
                .error_for_status()?;
        }
    }

    let chunk_len = 9 * SR as usize;
    for round in 0..args.rounds {
        // Chunks are uploaded once fully recorded, so they started one chunk length ago.
        let start = now_ns() - 9 * bsp_proto::NANOS_PER_SEC;
        for (k, id) in ids.iter().enumerate() {
            let hb = Heartbeat {
                clock: Some(ClockStatus {
                    offset_ns: 0,
                    rtt_ns: 200_000,
                    measured_at: now_ns(),
                    error_ns: Some(100_000),
                    detail: None,
                }),
                uptime_s: 0,
                chunks_sent: round as u64,
                chunks_gated: 0,
                queue_len: 0,
                capture_dropouts: 0,
            };
            http.post(format!("{base}{CLIENT_API_PREFIX}/{id}/heartbeat"))
                .json(&hb)
                .send()
                .await?
                .error_for_status()?;

            let mut pcm = noise(chunk_len, (round as u64) * 100 + k as u64);
            let delay = (dist(&mics[k]) - nearest) / SPEED_OF_SOUND;
            let atten = (nearest / dist(&mics[k])) as f32;
            let at = ((2.0 + delay) * SR as f64).round() as usize;
            for (i, s) in call.iter().enumerate() {
                if let Some(p) = pcm.get_mut(at + i) {
                    *p += s * atten;
                }
            }
            http.post(format!("{base}{CLIENT_API_PREFIX}/{id}/audio"))
                .header(HEADER_START_NS, start.to_string())
                .body(audio::encode_wav(SR, &pcm))
                .send()
                .await?
                .error_for_status()?;
            println!("round {round}: mic {k} delay {:.2} ms", delay * 1e3);
        }
        tokio::time::sleep(Duration::from_secs(2)).await;
    }

    println!("audio sent; detections appear after the server's grouping window (default 15 s):");
    println!("  curl -sk '{web_base}/api/v1/control/detections?limit=5'");
    Ok(())
}
