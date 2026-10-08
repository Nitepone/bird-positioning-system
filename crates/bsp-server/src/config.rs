use anyhow::Context;
use bsp_proto::GateConfig;
use serde::{Deserialize, Serialize};
use std::path::{Path, PathBuf};

/// Port for client devices; 2473 spells "BIRD" on a phone keypad.
pub const DEFAULT_CLIENT_PORT: u16 = 2473;

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(default, deny_unknown_fields)]
pub struct ServerConfig {
    /// Client devices: registration, heartbeats and audio (HTTP, TCP).
    pub client_listen: String,
    /// Client clock measurement (UDP).
    pub udp_listen: String,
    /// Web UI and control API.
    pub web: WebConfig,
    pub db_path: PathBuf,
    pub identifier: IdentifierConfig,
    pub clients: ClientDefaults,
    /// A client with no heartbeat for this long is shown as inactive.
    pub active_timeout_s: u64,
    /// Clients whose measured |offset| exceeds this are excluded from location.
    pub max_clock_offset_us: i64,
    /// Calls on different clients starting within this window may be the same call.
    pub max_call_skew_ms: i64,
    /// How long calls wait for matching calls from other clients before a detection is emitted.
    pub group_window_s: u64,
    /// How much received audio is kept in memory per client for the locator.
    pub audio_retention_s: u64,
    /// Number of raw events kept in the database.
    pub event_retention: u32,
    /// How long a client's "request positioning" stays active.
    pub positioning_request_ttl_s: u64,
}

impl Default for ServerConfig {
    fn default() -> Self {
        Self {
            client_listen: format!("0.0.0.0:{DEFAULT_CLIENT_PORT}"),
            udp_listen: format!("0.0.0.0:{DEFAULT_CLIENT_PORT}"),
            web: WebConfig::default(),
            db_path: "bsp.db".into(),
            identifier: IdentifierConfig::default(),
            clients: ClientDefaults::default(),
            active_timeout_s: 15,
            max_clock_offset_us: 2000,
            max_call_skew_ms: 200,
            group_window_s: 15,
            audio_retention_s: 120,
            event_retention: 10_000,
            positioning_request_ttl_s: 600,
        }
    }
}

/// Web UI and control API listeners.
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(default, deny_unknown_fields)]
pub struct WebConfig {
    /// Serve the UI over HTTPS (with `http_listen` redirecting to it).
    pub https: bool,
    pub https_listen: String,
    /// Redirects to HTTPS when `https` is on, otherwise serves the UI. Empty disables it.
    pub http_listen: String,
    /// PEM certificate (chain) and private key. When neither file exists, a
    /// self-signed certificate is generated there on first start.
    pub cert_path: PathBuf,
    pub key_path: PathBuf,
    /// Extra host names / IP addresses for a generated certificate, besides
    /// localhost and this machine's host name.
    pub self_signed_names: Vec<String>,
}

impl Default for WebConfig {
    fn default() -> Self {
        Self {
            https: true,
            https_listen: "0.0.0.0:443".into(),
            http_listen: "0.0.0.0:80".into(),
            cert_path: "tls/cert.pem".into(),
            key_path: "tls/key.pem".into(),
            self_signed_names: Vec::new(),
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum IdentifierKind {
    Mock,
    Birdnet,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(default, deny_unknown_fields)]
pub struct IdentifierConfig {
    pub kind: IdentifierKind,
    /// SNR threshold for the mock identifier.
    pub mock_threshold_db: f32,
    pub geo: GeoConfig,
    #[cfg(feature = "birdnet")]
    pub birdnet: bsp_core::identifier::BirdNetConfig,
}

impl Default for IdentifierConfig {
    fn default() -> Self {
        Self {
            kind: if cfg!(feature = "birdnet") {
                IdentifierKind::Birdnet
            } else {
                IdentifierKind::Mock
            },
            mock_threshold_db: 12.0,
            geo: GeoConfig::default(),
            #[cfg(feature = "birdnet")]
            birdnet: Default::default(),
        }
    }
}

/// BirdNET geo model, used to flag species not expected at the site. The
/// site location and confidence thresholds are set from the web UI.
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(default, deny_unknown_fields)]
pub struct GeoConfig {
    pub model_path: PathBuf,
    pub labels_path: PathBuf,
    /// Species whose year-round occurrence probability reaches this are expected.
    pub threshold: f32,
}

impl Default for GeoConfig {
    fn default() -> Self {
        Self {
            model_path: "models/birdnet-geo/model.onnx".into(),
            labels_path: "models/birdnet-geo/labels.txt".into(),
            threshold: 0.03,
        }
    }
}

/// Settings handed to clients at registration.
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(default, deny_unknown_fields)]
pub struct ClientDefaults {
    pub heartbeat_interval_s: u32,
    pub chunk_secs: f32,
    pub gate: GateConfig,
}

impl Default for ClientDefaults {
    fn default() -> Self {
        Self {
            heartbeat_interval_s: 5,
            chunk_secs: 9.0,
            gate: GateConfig::default(),
        }
    }
}

impl ServerConfig {
    pub fn load(path: &Path) -> anyhow::Result<Self> {
        let text =
            std::fs::read_to_string(path).with_context(|| format!("reading {}", path.display()))?;
        toml::from_str(&text).with_context(|| format!("parsing {}", path.display()))
    }
}
