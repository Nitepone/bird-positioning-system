//! Settings edited from the web UI (stored in the database), and keeping the
//! identifier's local-species filter in sync with them.

use crate::state::SharedState;
use bsp_core::identifier::{GeoSpecies, LocalSpecies, Thresholds};
use serde::{Deserialize, Serialize};

pub const DB_KEY: &str = "settings";

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum ConfidenceLevel {
    VeryHigh,
    High,
    Medium,
    Low,
}

impl ConfidenceLevel {
    pub const ALL: [Self; 4] = [Self::VeryHigh, Self::High, Self::Medium, Self::Low];

    /// Detections below the lowest level are never kept.
    pub const MIN: f32 = Self::Low.value();

    pub const fn value(self) -> f32 {
        match self {
            Self::VeryHigh => 0.95,
            Self::High => 0.90,
            Self::Medium => 0.80,
            Self::Low => 0.70,
        }
    }

    pub fn label(self) -> &'static str {
        match self {
            Self::VeryHigh => "Very high",
            Self::High => "High",
            Self::Medium => "Medium",
            Self::Low => "Low",
        }
    }
}

/// Where the system is, for deciding which species are expected.
#[derive(Debug, Clone, Copy, PartialEq, Serialize, Deserialize)]
pub struct Site {
    pub latitude: f64,
    pub longitude: f64,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(default)]
pub struct Settings {
    pub site: Option<Site>,
    /// Minimum confidence for species expected at the site.
    pub min_confidence_expected: ConfidenceLevel,
    /// Minimum confidence for species not expected at the site.
    pub min_confidence_unexpected: ConfidenceLevel,
}

impl Default for Settings {
    fn default() -> Self {
        Self {
            site: None,
            min_confidence_expected: ConfidenceLevel::Medium,
            min_confidence_unexpected: ConfidenceLevel::High,
        }
    }
}

impl Settings {
    pub fn validate(&self) -> Result<(), String> {
        if let Some(s) = self.site
            && !((-90.0..=90.0).contains(&s.latitude) && (-180.0..=180.0).contains(&s.longitude))
        {
            return Err("latitude must be within -90..90 and longitude within -180..180".into());
        }
        Ok(())
    }

    pub fn thresholds(&self) -> Thresholds {
        Thresholds {
            expected: self.min_confidence_expected.value(),
            unexpected: self.min_confidence_unexpected.value(),
        }
    }
}

/// State of the expected-species list, for the UI.
#[derive(Debug, Clone, Serialize)]
#[serde(tag = "status", rename_all = "snake_case")]
pub enum LocalSpeciesStatus {
    /// No site location set: every species is treated as expected.
    NoLocation,
    /// No geo model available: every species is treated as expected.
    NoGeoModel,
    Ready {
        site: Site,
        /// Expected species, most likely first.
        species: Vec<GeoSpecies>,
    },
    Error {
        error: String,
    },
}

/// Applies `settings` to the running identifier, recomputing the expected
/// species if the site changed (or on first call, when `previous` is `None`).
pub async fn apply(state: &SharedState, settings: &Settings, previous: Option<&Settings>) {
    state.species_filter.set_thresholds(settings.thresholds());
    if previous.is_some_and(|p| p.site == settings.site) {
        return;
    }
    let status = match (settings.site, state.geo.clone()) {
        (None, _) => LocalSpeciesStatus::NoLocation,
        (Some(_), None) => LocalSpeciesStatus::NoGeoModel,
        (Some(site), Some(geo)) => {
            let threshold = state.cfg.identifier.geo.threshold;
            let result = tokio::task::spawn_blocking(move || {
                geo.year_round(site.latitude as f32, site.longitude as f32)
            })
            .await
            .map_err(anyhow::Error::from)
            .and_then(|r| r);
            match result {
                Ok(all) => {
                    let mut local: Vec<GeoSpecies> = all
                        .iter()
                        .filter(|s| s.probability >= threshold)
                        .cloned()
                        .collect();
                    local.sort_by(|a, b| b.probability.total_cmp(&a.probability));
                    let filter = LocalSpecies::new(
                        all.iter().map(|s| &s.species),
                        local.iter().map(|s| &s.species),
                    );
                    tracing::info!(
                        latitude = site.latitude,
                        longitude = site.longitude,
                        expected = local.len(),
                        "expected species updated"
                    );
                    state.species_filter.set_local_species(Some(filter));
                    *state.local_species.write().unwrap() = LocalSpeciesStatus::Ready {
                        site,
                        species: local,
                    };
                    return;
                }
                Err(e) => {
                    tracing::error!("geo model failed: {e:#}");
                    LocalSpeciesStatus::Error {
                        error: format!("{e:#}"),
                    }
                }
            }
        }
    };
    state.species_filter.set_local_species(None);
    *state.local_species.write().unwrap() = status;
}

/// Loads the saved settings, falling back to defaults.
pub fn load(state: &SharedState) -> Settings {
    match state.db.get_setting(DB_KEY) {
        Ok(Some(json)) => serde_json::from_str(&json).unwrap_or_else(|e| {
            tracing::warn!("ignoring unreadable saved settings: {e}");
            Settings::default()
        }),
        Ok(None) => Settings::default(),
        Err(e) => {
            tracing::error!("reading settings: {e}");
            Settings::default()
        }
    }
}
