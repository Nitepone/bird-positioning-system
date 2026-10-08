//! BirdNET+ geo model: which species occur at a location.
//!
//! The ONNX model takes `[batch, 3]` f32 rows of `(latitude, longitude, week)`,
//! with weeks 1..=48 (four per month), and returns per-species occurrence
//! probabilities. Labels are tab-separated `code<TAB>scientific<TAB>common`.

use crate::identifier::{GeoSpecies, RangeModel, Species};
use anyhow::{Context, ensure};
use ort::session::Session;
use ort::value::Tensor;
use std::path::Path;
use std::sync::Mutex;

const WEEKS: usize = 48;

pub struct GeoModel {
    session: Mutex<Session>,
    labels: Vec<Species>,
}

fn parse_label(line: &str) -> Option<Species> {
    let mut parts = line.split('\t');
    let _code = parts.next()?;
    let scientific = parts.next()?.trim().to_string();
    let common = parts
        .next()
        .map(str::trim)
        .filter(|c| !c.is_empty())
        .unwrap_or(&scientific)
        .to_string();
    Some(Species { scientific, common })
}

impl GeoModel {
    pub fn load(model_path: &Path, labels_path: &Path) -> anyhow::Result<Self> {
        let labels: Vec<Species> = std::fs::read_to_string(labels_path)
            .with_context(|| format!("reading geo labels {}", labels_path.display()))?
            .lines()
            .filter(|l| !l.trim().is_empty())
            .map(|l| parse_label(l).with_context(|| format!("bad geo label line {l:?}")))
            .collect::<anyhow::Result<_>>()?;
        let session = Session::builder()
            .map_err(|e| anyhow::anyhow!("{e}"))?
            .commit_from_file(model_path)
            .map_err(|e| anyhow::anyhow!("{e}"))
            .with_context(|| format!("loading geo model {}", model_path.display()))?;
        tracing::info!(model = %model_path.display(), species = labels.len(), "geo model loaded");
        Ok(Self {
            session: Mutex::new(session),
            labels,
        })
    }
}

impl RangeModel for GeoModel {
    fn year_round(&self, latitude: f32, longitude: f32) -> anyhow::Result<Vec<GeoSpecies>> {
        let input: Vec<f32> = (1..=WEEKS)
            .flat_map(|w| [latitude, longitude, w as f32])
            .collect();
        let tensor = Tensor::from_array(([WEEKS, 3], input)).map_err(|e| anyhow::anyhow!("{e}"))?;
        let mut session = self.session.lock().unwrap();
        let input_name = session.inputs()[0].name().to_string();
        let outputs = session
            .run(ort::inputs![input_name.as_str() => tensor])
            .map_err(|e| anyhow::anyhow!("{e}"))?;
        let (_, probs) = outputs[0]
            .try_extract_tensor::<f32>()
            .map_err(|e| anyhow::anyhow!("{e}"))?;
        let n = self.labels.len();
        ensure!(
            probs.len() == WEEKS * n,
            "geo model returned {} values, expected {}",
            probs.len(),
            WEEKS * n
        );
        Ok(self
            .labels
            .iter()
            .enumerate()
            .map(|(i, species)| GeoSpecies {
                species: species.clone(),
                probability: (0..WEEKS).map(|w| probs[w * n + i]).fold(0.0, f32::max),
            })
            .collect())
    }
}
