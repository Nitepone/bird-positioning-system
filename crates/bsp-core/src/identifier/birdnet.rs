//! BirdNET identifier running an ONNX model via ONNX Runtime.
//!
//! Supported models (fetch them with `scripts/fetch-birdnet.sh`):
//! - BirdNET+ V3.0 developer preview: 32 kHz mono, `[batch, samples]` f32 in
//!   (variable length; we feed 3 s windows), probabilities as the first output.
//! - BirdNET v2.4 (converted from TFLite): 3 s of 48 kHz mono
//!   (`[batch, 144000]` f32) in, one logit per label out.
//!
//! Labels are read one `Scientific name_Common name` entry per line, in model output order.

use super::{Call, Identifier, Species, refine_call_bounds};
use crate::audio::{self, AudioSample};
use anyhow::{Context, bail, ensure};
use ort::session::Session;
use ort::value::Tensor;
use serde::{Deserialize, Serialize};
use std::path::PathBuf;
use std::sync::Mutex;

/// Analysis window length used for both model versions.
const WINDOW_SECS: u32 = 3;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default, Serialize, Deserialize)]
pub enum BirdNetVersion {
    #[serde(rename = "v2.4")]
    V2_4,
    #[default]
    #[serde(rename = "v3.0")]
    V3_0,
}

impl BirdNetVersion {
    pub fn sample_rate(self) -> u32 {
        match self {
            Self::V2_4 => 48000,
            Self::V3_0 => 32000,
        }
    }

    fn window_samples(self) -> usize {
        (self.sample_rate() * WINDOW_SECS) as usize
    }

    /// Whether the model outputs logits (true) or probabilities (false).
    fn outputs_logits(self) -> bool {
        matches!(self, Self::V2_4)
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(default)]
pub struct BirdNetConfig {
    pub version: BirdNetVersion,
    pub model_path: PathBuf,
    pub labels_path: PathBuf,
    /// Scores below this (0..1) are discarded outright. The server applies its
    /// own, stricter thresholds afterwards (see `LocalSpeciesFilter`).
    pub min_confidence: f32,
    /// Score curve steepness, as in BirdNET-Analyzer (0.5..1.5, default 1.0).
    /// Higher values push scores towards 0 or 1.
    pub sensitivity: f32,
    /// Hop between consecutive 3 s analysis windows.
    pub hop_secs: f32,
    pub threads: usize,
}

impl Default for BirdNetConfig {
    fn default() -> Self {
        Self {
            version: BirdNetVersion::default(),
            model_path: "models/birdnet-v3.0/model.onnx".into(),
            labels_path: "models/birdnet-v3.0/labels.txt".into(),
            min_confidence: 0.5,
            sensitivity: 1.0,
            hop_secs: 1.5,
            threads: 4,
        }
    }
}

pub struct BirdNetIdentifier {
    session: Mutex<Session>,
    input_name: String,
    /// Name of the per-label score output (the first output).
    output_name: String,
    /// Whether the model accepts more than one window per run.
    batched: bool,
    labels: Vec<Species>,
    cfg: BirdNetConfig,
}

fn parse_label(line: &str) -> Species {
    match line.split_once('_') {
        Some((sci, common)) => Species {
            scientific: sci.trim().into(),
            common: common.trim().into(),
        },
        None => Species {
            scientific: line.trim().into(),
            common: line.trim().into(),
        },
    }
}

impl BirdNetIdentifier {
    pub fn load(cfg: BirdNetConfig) -> anyhow::Result<Self> {
        let labels: Vec<Species> = std::fs::read_to_string(&cfg.labels_path)
            .with_context(|| format!("reading labels {}", cfg.labels_path.display()))?
            .lines()
            .filter(|l| !l.trim().is_empty())
            .map(parse_label)
            .collect();
        ensure!(!labels.is_empty(), "labels file is empty");

        let session = Session::builder()
            .map_err(|e| anyhow::anyhow!("{e}"))?
            .with_intra_threads(cfg.threads)
            .map_err(|e| anyhow::anyhow!("{e}"))?
            .commit_from_file(&cfg.model_path)
            .map_err(|e| anyhow::anyhow!("{e}"))
            .with_context(|| format!("loading model {}", cfg.model_path.display()))?;

        ensure!(
            session.inputs().len() == 1,
            "expected 1 model input, found {}",
            session.inputs().len()
        );
        let input = &session.inputs()[0];
        let in_shape = input
            .dtype()
            .tensor_shape()
            .context("model input is not a tensor")?
            .to_vec();
        let window = cfg.version.window_samples();
        ensure!(
            in_shape.len() == 2 && (in_shape[1] == window as i64 || in_shape[1] < 0),
            "model input shape {in_shape:?} does not accept [batch, {window}]; \
             is `version` ({:?}) right for this model?",
            cfg.version
        );
        let out_shape = session.outputs()[0]
            .dtype()
            .tensor_shape()
            .context("model output is not a tensor")?
            .to_vec();
        let classes = *out_shape.last().unwrap_or(&-1);
        if classes >= 0 && classes as usize != labels.len() {
            bail!(
                "model has {classes} classes but labels file has {} entries",
                labels.len()
            );
        }
        let batched = in_shape[0] != 1;
        let input_name = input.name().to_string();
        let output_name = session.outputs()[0].name().to_string();
        tracing::info!(
            version = ?cfg.version, model = %cfg.model_path.display(), labels = labels.len(),
            ?in_shape, ?out_shape, batched, "BirdNET loaded"
        );
        Ok(Self {
            session: Mutex::new(session),
            input_name,
            output_name,
            batched,
            labels,
            cfg,
        })
    }

    /// Runs the model on `n` concatenated windows, returning `n * labels` raw outputs.
    fn infer(&self, windows: Vec<f32>, n: usize) -> anyhow::Result<Vec<f32>> {
        let window = self.cfg.version.window_samples();
        let mut session = self.session.lock().unwrap();
        let mut run = |data: Vec<f32>, n: usize| -> anyhow::Result<Vec<f32>> {
            let tensor =
                Tensor::from_array(([n, window], data)).map_err(|e| anyhow::anyhow!("{e}"))?;
            let outputs = session
                .run(ort::inputs![self.input_name.as_str() => tensor])
                .map_err(|e| anyhow::anyhow!("{e}"))?;
            let (_, logits) = outputs[self.output_name.as_str()]
                .try_extract_tensor::<f32>()
                .map_err(|e| anyhow::anyhow!("{e}"))?;
            ensure!(
                logits.len() == n * self.labels.len(),
                "unexpected output size {}",
                logits.len()
            );
            Ok(logits.to_vec())
        };
        if self.batched {
            run(windows, n)
        } else {
            let mut all = Vec::with_capacity(n * self.labels.len());
            for w in windows.chunks_exact(window) {
                all.extend(run(w.to_vec(), 1)?);
            }
            Ok(all)
        }
    }
}

impl BirdNetIdentifier {
    /// Converts a raw model output into a 0..1 score with the configured sensitivity.
    fn score(&self, raw: f32) -> f32 {
        let logit = if self.cfg.version.outputs_logits() {
            raw
        } else {
            let p = raw.clamp(1e-7, 1.0 - 1e-7);
            (p / (1.0 - p)).ln()
        };
        1.0 / (1.0 + (-self.cfg.sensitivity * logit).exp())
    }
}

impl Identifier for BirdNetIdentifier {
    fn name(&self) -> &str {
        match self.cfg.version {
            BirdNetVersion::V2_4 => "birdnet-v2.4",
            BirdNetVersion::V3_0 => "birdnet-v3.0",
        }
    }

    fn identify(&self, sample: &AudioSample) -> anyhow::Result<Vec<Call>> {
        let (sr, window) = (
            self.cfg.version.sample_rate(),
            self.cfg.version.window_samples(),
        );
        let pcm = audio::resample(&sample.pcm, sample.sample_rate, sr);
        if pcm.len() < sr as usize / 2 {
            return Ok(Vec::new());
        }
        let hop = ((self.cfg.hop_secs * sr as f32) as usize).max(1);
        let mut starts: Vec<usize> = (0..pcm.len().saturating_sub(window) + 1)
            .step_by(hop)
            .collect();
        // Make sure the tail of the chunk is covered.
        if let Some(&last) = starts.last()
            && last + window < pcm.len()
        {
            starts.push(pcm.len() - window);
        }
        let mut windows = Vec::with_capacity(starts.len() * window);
        for &s in &starts {
            let end = (s + window).min(pcm.len());
            windows.extend_from_slice(&pcm[s..end]);
            windows.resize(windows.len() + window - (end - s), 0.0);
        }
        let logits = self.infer(windows, starts.len())?;

        // (label, window range, score) hits, merging overlapping windows of the same label.
        let mut hits: Vec<(usize, usize, usize, f32)> = Vec::new();
        for (w, &s) in starts.iter().enumerate() {
            let e = (s + window).min(pcm.len());
            for (label, &x) in logits[w * self.labels.len()..(w + 1) * self.labels.len()]
                .iter()
                .enumerate()
            {
                let score = self.score(x);
                if score < self.cfg.min_confidence {
                    continue;
                }
                match hits.iter_mut().rev().find(|h| h.0 == label && s <= h.2) {
                    Some(h) => {
                        h.2 = e;
                        h.3 = h.3.max(score);
                    }
                    None => hits.push((label, s, e, score)),
                }
            }
        }

        Ok(hits
            .into_iter()
            .map(|(label, lo, hi, score)| {
                let (s, e) = refine_call_bounds(&pcm, sr, lo, hi);
                let ts =
                    |i: usize| sample.start + (i as i64 * bsp_proto::NANOS_PER_SEC) / sr as i64;
                Call {
                    client_id: sample.client_id,
                    species: self.labels[label].clone(),
                    confidence: score,
                    start: ts(s),
                    end: ts(e),
                    unexpected: false,
                }
            })
            .collect())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn labels() {
        let s = parse_label("Turdus migratorius_American Robin");
        assert_eq!(s.scientific, "Turdus migratorius");
        assert_eq!(s.common, "American Robin");
    }
}
