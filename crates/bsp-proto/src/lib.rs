//! Wire protocol shared by `bsp-client` and `bsp-server`.
//!
//! Control/data traffic is HTTP + JSON under [`CLIENT_API_PREFIX`]; clock
//! measurement uses a tiny fixed-size UDP packet (see [`timesync`]).

use serde::{Deserialize, Serialize};
use std::time::{SystemTime, UNIX_EPOCH};

pub use uuid::Uuid;

pub mod timesync;

/// Nanoseconds since the Unix epoch, as measured by a (disciplined) wall clock.
pub type Timestamp = i64;

/// Unique, persistent identity of a client device.
pub type ClientId = Uuid;

pub const NANOS_PER_SEC: i64 = 1_000_000_000;
pub const NANOS_PER_MS: i64 = 1_000_000;

pub const CLIENT_API_PREFIX: &str = "/api/v1/client";

/// Header carrying the timestamp of the first sample of an uploaded WAV chunk.
pub const HEADER_START_NS: &str = "x-bsp-start-ns";

/// Current wall-clock time.
pub fn now_ns() -> Timestamp {
    let d = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .expect("system clock before 1970");
    d.as_nanos() as Timestamp
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct Capabilities {
    pub sample_rate: u32,
    pub channels: u16,
    pub sample_format: String,
    /// Free-form description of how the OS clock is disciplined (e.g. "chrony", "ptp", "unknown").
    pub clock_source: String,
    pub device_name: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct RegisterRequest {
    pub client_id: ClientId,
    pub hostname: String,
    pub version: String,
    pub capabilities: Capabilities,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct GateConfig {
    /// Lower edge of the bird band used for noise gating (Hz).
    pub band_low_hz: f32,
    /// Upper edge of the bird band used for noise gating (Hz).
    pub band_high_hz: f32,
    /// A frame must exceed the noise floor by this much to pass the gate.
    pub threshold_db: f32,
    /// Frame length used for RMS measurement.
    pub frame_ms: u32,
}

impl Default for GateConfig {
    fn default() -> Self {
        Self {
            band_low_hz: 1000.0,
            band_high_hz: 10000.0,
            threshold_db: 10.0,
            frame_ms: 100,
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct RegisterResponse {
    pub udp_timesync_port: u16,
    pub heartbeat_interval_s: u32,
    pub chunk_secs: f32,
    pub gate: GateConfig,
}

/// Result of the client's most recent clock measurement against the server.
#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq)]
pub struct ClockStatus {
    /// Server clock minus client clock (ns).
    pub offset_ns: i64,
    pub rtt_ns: i64,
    /// When this measurement was taken (client clock).
    pub measured_at: Timestamp,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Heartbeat {
    pub clock: Option<ClockStatus>,
    pub uptime_s: u64,
    pub chunks_sent: u64,
    pub chunks_gated: u64,
    pub queue_len: u32,
    /// Number of capture discontinuities since the previous heartbeat.
    pub capture_dropouts: u32,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct AudioAccepted {
    pub duration_s: f32,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ErrorBody {
    pub error: String,
}
