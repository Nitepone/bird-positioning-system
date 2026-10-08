//! Runs BirdNET on WAV files and prints the calls it finds.
//!
//! cargo run --release -p bsp-core --example identify -- [v3.0|v2.4] FILE.wav...
//! Models are expected where `scripts/fetch-birdnet.sh` puts them.

use bsp_core::audio::{self, AudioSample};
use bsp_core::identifier::{BirdNetConfig, BirdNetIdentifier, Identifier, birdnet::BirdNetVersion};
use bsp_proto::{NANOS_PER_SEC, Uuid};

fn main() -> anyhow::Result<()> {
    let mut args: Vec<String> = std::env::args().skip(1).collect();
    let (version, dir) = match args.first().map(String::as_str) {
        Some("v2.4") => (BirdNetVersion::V2_4, "models/birdnet-v2.4"),
        Some("v3.0") | None => (BirdNetVersion::V3_0, "models/birdnet-v3.0"),
        Some(_) => (BirdNetVersion::V3_0, "models/birdnet-v3.0"),
    };
    if matches!(args.first().map(String::as_str), Some("v2.4" | "v3.0")) {
        args.remove(0);
    }
    anyhow::ensure!(!args.is_empty(), "usage: identify [v3.0|v2.4] FILE.wav...");

    let id = BirdNetIdentifier::load(BirdNetConfig {
        version,
        model_path: format!("{dir}/model.onnx").into(),
        labels_path: format!("{dir}/labels.txt").into(),
        min_confidence: 0.1,
        ..Default::default()
    })?;
    for path in args {
        let (sample_rate, pcm) = audio::decode_wav(&std::fs::read(&path)?)?;
        let sample = AudioSample {
            client_id: Uuid::nil(),
            start: 0,
            sample_rate,
            pcm: pcm.into(),
        };
        let calls = id.identify(&sample)?;
        println!("{path}: {} call(s)", calls.len());
        for c in calls {
            println!(
                "  {:6.2}-{:6.2}s  {:.2}  {} ({})",
                c.start as f64 / NANOS_PER_SEC as f64,
                c.end as f64 / NANOS_PER_SEC as f64,
                c.confidence,
                c.species.common,
                c.species.scientific
            );
        }
    }
    Ok(())
}
