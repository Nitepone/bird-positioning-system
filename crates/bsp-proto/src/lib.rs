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

/// Optional header: when the sample after the chunk's last was captured, on
/// the same clock as [`HEADER_START_NS`]. With the start it gives the chunk's
/// actual duration, which differs from the nominal one by the sample clock's
/// error.
pub const HEADER_END_NS: &str = "x-bps-end-ns";

/// Optional header from clients that follow their Wi-Fi access point's clock
/// (TSF) by its beacons: `<bssid> <start> <end>`, the chunk's start and end
/// as [`HEADER_START_NS`] and [`HEADER_END_NS`] but on that clock (ns). All
/// clients of one access point share it to microseconds; the server converts
/// it to its own clock (see [`BeaconSync`]).
pub const HEADER_TSF: &str = "x-bps-tsf";

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
    /// Name the client suggests for itself. A name set by the operator takes
    /// precedence; the suggestion is shown while there is none.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub name: Option<String>,
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
    /// The server accepts audio uploads as FLAC as well as WAV. Older
    /// servers leave it out, and clients then send WAV.
    #[serde(default)]
    pub flac_uploads: bool,
}

/// Result of the client's clock measurement against the server.
#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq)]
pub struct ClockStatus {
    /// Server clock minus the client's audio timestamps (ns): what is left
    /// after any correction the client applies itself.
    pub offset_ns: i64,
    /// Lowest round trip behind the estimate (ns).
    pub rtt_ns: i64,
    /// When the latest measurement was taken (server clock; older clients
    /// sent their own clock).
    pub measured_at: Timestamp,
    /// Worst-case error of the client's audio timestamps (ns). Older clients
    /// leave it out; see [`ClockStatus::error_bound_ns`].
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub error_ns: Option<i64>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub detail: Option<SyncDetail>,
    /// Present while the client follows its access point's clock.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub beacon: Option<BeaconSync>,
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn bssids_round_trip() {
        let b: Bssid = "04:f4:1c:07:93:52".parse().unwrap();
        assert_eq!(b.0, [4, 0xf4, 0x1c, 7, 0x93, 0x52]);
        assert_eq!(serde_json::to_string(&b).unwrap(), "\"04:f4:1c:07:93:52\"");
        assert!("04:f4:1c:07:93".parse::<Bssid>().is_err());
        assert!("04:f4:1c:07:93:52:00".parse::<Bssid>().is_err());
        assert!("04:f4:1c:07:93:zz".parse::<Bssid>().is_err());
    }
}

impl ClockStatus {
    /// Worst-case error of the client's audio timestamps: as reported, else
    /// the offset plus half the round trip (the measurement cannot tell how
    /// delay splits between the two directions).
    pub fn error_bound_ns(&self) -> i64 {
        self.error_ns
            .unwrap_or(self.offset_ns.abs() + self.rtt_ns / 2)
    }
}

/// A client's view of its Wi-Fi access point's clock (TSF), which it follows
/// by the AP's beacons, and of the server's clock at the same instant. The
/// server pools these across the AP's clients to convert its TSF.
#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq)]
pub struct BeaconSync {
    /// The access point.
    pub bssid: Bssid,
    /// Its TSF (ns), now.
    pub tsf_ns: i64,
    /// The server clock at the same instant, by the client's measurement.
    pub server_ns: Timestamp,
    /// Worst-case error of `server_ns` (ns).
    pub server_error_ns: i64,
    /// RMS distance of the beacons' arrivals from the client's line through
    /// them (ns): how precisely the client follows the TSF.
    pub scatter_ns: i64,
    /// Client clock rate relative to the access point's, parts per million.
    pub drift_ppm: f64,
    /// Beacons the line is fitted through.
    pub beacons: u32,
}

/// A Wi-Fi access point's address, written `aa:bb:cc:dd:ee:ff`.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct Bssid(pub [u8; 6]);

impl std::fmt::Display for Bssid {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        let b = self.0;
        write!(
            f,
            "{:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}",
            b[0], b[1], b[2], b[3], b[4], b[5]
        )
    }
}

impl std::str::FromStr for Bssid {
    type Err = String;
    fn from_str(s: &str) -> Result<Self, String> {
        let mut out = [0u8; 6];
        let mut parts = s.split(':');
        for b in &mut out {
            let p = parts.next().filter(|p| p.len() == 2);
            *b = p
                .and_then(|p| u8::from_str_radix(p, 16).ok())
                .ok_or_else(|| format!("not a BSSID: {s:?}"))?;
        }
        if parts.next().is_some() {
            return Err(format!("not a BSSID: {s:?}"));
        }
        Ok(Bssid(out))
    }
}

impl Serialize for Bssid {
    fn serialize<S: serde::Serializer>(&self, s: S) -> Result<S::Ok, S::Error> {
        s.collect_str(self)
    }
}

impl<'de> Deserialize<'de> for Bssid {
    fn deserialize<D: serde::Deserializer<'de>>(d: D) -> Result<Self, D::Error> {
        let s = String::deserialize(d)?;
        s.parse().map_err(serde::de::Error::custom)
    }
}

/// How clock measurement went, for monitoring. Counts and round trips cover
/// the time since the previous heartbeat.
#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq)]
pub struct SyncDetail {
    /// Whether the client corrects its audio timestamps by the measured offset.
    pub corrects_timestamps: bool,
    /// Measured server clock minus client clock, now (ns).
    pub clock_offset_ns: i64,
    /// Client clock rate relative to the server's, parts per million.
    pub drift_ppm: f64,
    /// Measurements the estimate is fitted through.
    pub fit_points: u32,
    pub requests: u32,
    pub replies: u32,
    pub rtt_min_ns: Option<i64>,
    pub rtt_median_ns: Option<i64>,
    pub rtt_max_ns: Option<i64>,
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
