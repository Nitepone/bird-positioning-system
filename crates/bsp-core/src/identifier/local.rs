//! Flags calls from species not expected at the site, and applies separate
//! confidence thresholds to expected and unexpected species.

use super::{Call, Identifier, Species};
use crate::audio::AudioSample;
use serde::{Deserialize, Serialize};
use std::collections::HashSet;
use std::sync::{Arc, RwLock};

/// A species with its year-round occurrence probability at a location.
#[derive(Debug, Clone, Serialize)]
pub struct GeoSpecies {
    pub species: Species,
    pub probability: f32,
}

/// Species range data, e.g. the BirdNET geo model.
pub trait RangeModel: Send + Sync {
    /// Every species the model covers, with its highest weekly occurrence
    /// probability over the year at this location.
    fn year_round(&self, latitude: f32, longitude: f32) -> anyhow::Result<Vec<GeoSpecies>>;
}

/// Minimum confidence for reporting a call.
#[derive(Debug, Clone, Copy, PartialEq, Serialize, Deserialize)]
pub struct Thresholds {
    pub expected: f32,
    pub unexpected: f32,
}

/// The species expected at a site, and the wider set the range data knows
/// about. Species are matched by scientific name, falling back to the common
/// name (taxonomies differ between models, e.g. after genus changes).
#[derive(Debug, Clone, Default)]
pub struct LocalSpecies {
    known: HashSet<String>,
    local: HashSet<String>,
}

fn keys(s: &Species) -> [String; 2] {
    [
        format!("s:{}", s.scientific.to_lowercase()),
        format!("c:{}", s.common.to_lowercase()),
    ]
}

impl LocalSpecies {
    /// `known` is every species the range data covers; `local` those expected here.
    pub fn new<'a>(
        known: impl IntoIterator<Item = &'a Species>,
        local: impl IntoIterator<Item = &'a Species>,
    ) -> Self {
        Self {
            known: known.into_iter().flat_map(keys).collect(),
            local: local.into_iter().flat_map(keys).collect(),
        }
    }

    /// `Some(true)` if expected, `Some(false)` if not, `None` if the range data
    /// does not cover the species (it is then treated as expected).
    pub fn is_expected(&self, s: &Species) -> Option<bool> {
        let k = keys(s);
        if k.iter().any(|k| self.local.contains(k)) {
            Some(true)
        } else if k.iter().any(|k| self.known.contains(k)) {
            Some(false)
        } else {
            None
        }
    }

    pub fn local_count(&self) -> usize {
        self.local.iter().filter(|k| k.starts_with("s:")).count()
    }
}

struct FilterState {
    thresholds: Thresholds,
    /// `None` until a site location is known: every species is expected.
    local: Option<LocalSpecies>,
}

/// Wraps another identifier; settings can be changed while it is in use.
pub struct LocalSpeciesFilter {
    inner: Arc<dyn Identifier>,
    state: RwLock<FilterState>,
}

impl LocalSpeciesFilter {
    pub fn new(inner: Arc<dyn Identifier>, thresholds: Thresholds) -> Self {
        Self {
            inner,
            state: RwLock::new(FilterState {
                thresholds,
                local: None,
            }),
        }
    }

    pub fn set_thresholds(&self, thresholds: Thresholds) {
        self.state.write().unwrap().thresholds = thresholds;
    }

    pub fn set_local_species(&self, local: Option<LocalSpecies>) {
        self.state.write().unwrap().local = local;
    }

    /// Applies the thresholds and sets [`Call::unexpected`].
    pub fn filter(&self, calls: Vec<Call>) -> Vec<Call> {
        let state = self.state.read().unwrap();
        calls
            .into_iter()
            .filter_map(|mut call| {
                call.unexpected = state
                    .local
                    .as_ref()
                    .and_then(|l| l.is_expected(&call.species))
                    == Some(false);
                let min = if call.unexpected {
                    state.thresholds.unexpected
                } else {
                    state.thresholds.expected
                };
                (call.confidence >= min).then_some(call)
            })
            .collect()
    }
}

impl Identifier for LocalSpeciesFilter {
    fn name(&self) -> &str {
        self.inner.name()
    }

    fn identify(&self, sample: &AudioSample) -> anyhow::Result<Vec<Call>> {
        Ok(self.filter(self.inner.identify(sample)?))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::identifier::MockIdentifier;
    use bsp_proto::Uuid;

    fn sp(sci: &str, common: &str) -> Species {
        Species {
            scientific: sci.into(),
            common: common.into(),
        }
    }

    fn call(s: Species, confidence: f32) -> Call {
        Call {
            client_id: Uuid::nil(),
            species: s,
            confidence,
            start: 0,
            end: 0,
            unexpected: false,
        }
    }

    #[test]
    fn flags_and_thresholds() {
        let crow = sp("Corvus brachyrhynchos", "American Crow");
        let raven = sp("Corvus corax", "Common Raven");
        // Renamed genus: matched through the common name.
        let hawk_geo = sp("Astur cooperii", "Cooper's Hawk");
        let hawk = sp("Accipiter cooperii", "Cooper's Hawk");
        let cricket = sp("Acheta domesticus", "House Cricket");

        let filter = LocalSpeciesFilter::new(
            Arc::new(MockIdentifier::default()),
            Thresholds {
                expected: 0.8,
                unexpected: 0.9,
            },
        );
        // No location yet: everything is expected.
        let out = filter.filter(vec![call(raven.clone(), 0.85)]);
        assert!(!out[0].unexpected);

        let known = [crow.clone(), raven.clone(), hawk_geo.clone()];
        filter.set_local_species(Some(LocalSpecies::new(&known, &[crow.clone(), hawk_geo])));
        let out = filter.filter(vec![
            call(crow.clone(), 0.85),
            call(crow, 0.75),          // below the expected threshold
            call(raven.clone(), 0.85), // unexpected, below its threshold
            call(raven, 0.95),         // unexpected, reported
            call(hawk, 0.85),          // expected via common name
            call(cricket, 0.85),       // not covered: treated as expected
        ]);
        let summary: Vec<_> = out
            .iter()
            .map(|c| (c.species.common.as_str(), c.unexpected))
            .collect();
        assert_eq!(
            summary,
            vec![
                ("American Crow", false),
                ("Common Raven", true),
                ("Cooper's Hawk", false),
                ("House Cricket", false)
            ]
        );
    }
}
