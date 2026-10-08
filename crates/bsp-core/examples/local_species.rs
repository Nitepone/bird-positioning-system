//! Prints the species the BirdNET geo model expects at a location (year-round).
//!
//! cargo run --release -p bsp-core --example local_species -- LAT LON [THRESHOLD]

use bsp_core::geo::GeoModel;
use bsp_core::identifier::RangeModel;
use std::path::Path;

fn main() -> anyhow::Result<()> {
    let args: Vec<f32> = std::env::args()
        .skip(1)
        .map(|a| a.parse())
        .collect::<Result<_, _>>()?;
    let [lat, lon, rest @ ..] = &args[..] else {
        anyhow::bail!("usage: local_species LAT LON [THRESHOLD]");
    };
    let threshold = rest.first().copied().unwrap_or(0.03);
    let geo = GeoModel::load(
        Path::new("models/birdnet-geo/model.onnx"),
        Path::new("models/birdnet-geo/labels.txt"),
    )?;
    let mut species = geo.year_round(*lat, *lon)?;
    species.retain(|s| s.probability >= threshold);
    species.sort_by(|a, b| b.probability.total_cmp(&a.probability));
    println!(
        "{} species at ({lat}, {lon}) with year-round probability >= {threshold}",
        species.len()
    );
    for s in species.iter().take(15) {
        println!(
            "  {:.2}  {} ({})",
            s.probability, s.species.common, s.species.scientific
        );
    }
    Ok(())
}
