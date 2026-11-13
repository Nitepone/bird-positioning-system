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

/// Version of the client-server API (register, heartbeat, audio, time sync):
/// semver, apart from the version of any client or of the server. Raise the
/// major version for a change that old clients or servers cannot handle, the
/// minor for a compatible addition (a new optional field or endpoint), the
/// patch for a fix that changes nothing on the wire. The firmware's copy is
/// `BPS_API_VERSION` in firmware/lib/bps_core/src/proto.h; a test keeps them
/// equal.
pub const API_VERSION: &str = "1.2.0";

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
    /// The client's own build, e.g. "bps-mcu 3f7c789-dirty".
    pub version: String,
    /// The [`API_VERSION`] the client speaks. Clients from before it was
    /// added leave it out.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub api_version: Option<String>,
    pub capabilities: Capabilities,
    /// Name the client suggests for itself. A name set by the operator takes
    /// precedence; the suggestion is shown while there is none.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub name: Option<String>,
    /// The build the client runs, as embedded in its firmware image (API
    /// 1.2.0; clients that know it). The server matches it against the
    /// firmware builds it has to tell which one the client runs.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub build: Option<BuildInfo>,
    /// The client takes firmware updates over the air: it acts on
    /// `firmware_offer` in heartbeat replies and reports `firmware_status`
    /// (API 1.2.0, ESP32 builds only). Absent means false.
    #[serde(default, skip_serializing_if = "std::ops::Not::not")]
    pub ota: bool,
}

/// Marks the build information in a firmware image: these 15 bytes, then
/// the build as one compact JSON object (`hw`, `tag`, `commit`, `dirty`,
/// `describe`, `built`; the same values as [`BuildInfo`]), then a NUL. An
/// image has exactly one.
pub const BUILD_INFO_MARKER: &[u8] = b"BPS_BUILD_INFO:";

/// Longest build information after [`BUILD_INFO_MARKER`], in bytes.
pub const MAX_BUILD_INFO_LEN: usize = 512;

/// Which firmware build a client runs (API 1.2.0). Two builds are the same
/// when `hardware_id`, `commit`, `dirty` and `built_at` are all equal.
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct BuildInfo {
    /// What the build is for: board and wiring, e.g. "esp32-s3 on
    /// xiao-proto-rev0.1". A build is offered only to clients with the same.
    pub hardware_id: String,
    /// The git tag, when the build is exactly at one with a clean tree;
    /// otherwise null (a development build).
    #[serde(default)]
    pub tag: Option<String>,
    /// Short git commit hash.
    pub commit: String,
    /// Built from a tree with uncommitted changes.
    pub dirty: bool,
    /// `git describe --tags --always --dirty`.
    pub describe: String,
    /// When it was built, UTC: `YYYY-MM-DDTHH:MM:SSZ`.
    pub built_at: String,
}

impl BuildInfo {
    /// A release: tagged, and built from a clean tree.
    pub fn is_stable(&self) -> bool {
        self.tag.is_some() && !self.dirty
    }

    /// Whether `other` is the same build (see [`BuildInfo`]).
    pub fn same_build(&self, other: &BuildInfo) -> bool {
        self.hardware_id == other.hardware_id
            && self.commit == other.commit
            && self.dirty == other.dirty
            && self.built_at == other.built_at
    }
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

/// What opens a recording on the client. The tag and the fields have names
/// unique across the client's messages: the microcontroller client finds
/// values by key anywhere in a reply.
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
#[serde(tag = "trigger_type", rename_all = "snake_case")]
pub enum TriggerConfig {
    /// Band-limited energy clearly above the adaptive noise floor: the noise
    /// gate. Its `threshold_db` is the sensitivity (lower is more sensitive).
    Energy(GateConfig),
    /// A trigger type this build does not know (from a newer peer).
    #[serde(other)]
    Unknown,
}

impl Default for TriggerConfig {
    fn default() -> Self {
        Self::Energy(GateConfig::default())
    }
}

impl TriggerConfig {
    /// The noise gate's settings for clients that know only the gate: the
    /// energy trigger's own, else the defaults.
    pub fn gate(&self) -> GateConfig {
        match self {
            Self::Energy(g) => g.clone(),
            Self::Unknown => GateConfig::default(),
        }
    }
}

/// How the client checks a triggered recording before uploading it, e.g.
/// with a model that hears whether a bird is calling in it.
#[derive(Debug, Clone, Default, Serialize, Deserialize, PartialEq)]
#[serde(tag = "validation_type", rename_all = "snake_case")]
pub enum ValidationConfig {
    /// Every triggered recording is uploaded.
    #[default]
    None,
    /// A model on the client's storage card, in `/models/<validation_model>/`
    /// (model.tflite and model.json, see trigger-models/): a triggered
    /// recording is uploaded only if the model's bird score reaches the
    /// threshold, which defaults to the model's own.
    Model {
        validation_model: String,
        #[serde(default, skip_serializing_if = "Option::is_none")]
        validation_threshold: Option<f32>,
    },
    /// A validation type this build does not know (from a newer peer); the
    /// client uploads everything, as with `None`.
    #[serde(other)]
    Unknown,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct RegisterResponse {
    /// The [`API_VERSION`] the server implements. Older servers leave it out.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub api_version: Option<String>,
    pub udp_timesync_port: u16,
    pub heartbeat_interval_s: u32,
    pub chunk_secs: f32,
    /// The energy trigger's settings, for clients that predate `trigger`.
    pub gate: GateConfig,
    /// The server accepts audio uploads as FLAC as well as WAV. Older
    /// servers leave it out, and clients then send WAV.
    #[serde(default)]
    pub flac_uploads: bool,
    /// Older servers leave it out: the trigger is then the gate, as `gate`.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub trigger: Option<TriggerConfig>,
    /// Older servers leave it out: no validation.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub validation: Option<ValidationConfig>,
}

impl RegisterResponse {
    /// The trigger to run: as sent, else (an older server) the gate.
    pub fn trigger(&self) -> TriggerConfig {
        self.trigger
            .clone()
            .unwrap_or_else(|| TriggerConfig::Energy(self.gate.clone()))
    }

    pub fn validation(&self) -> ValidationConfig {
        self.validation.clone().unwrap_or_default()
    }
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

    /// The firmware's `BPS_API_VERSION` is the same string as [`API_VERSION`].
    #[test]
    fn api_version_matches_the_firmwares() {
        let header = include_str!("../../../firmware/lib/bps_core/src/proto.h");
        let c = header
            .lines()
            .find_map(|l| l.trim().strip_prefix("#define BPS_API_VERSION "))
            .expect("proto.h defines BPS_API_VERSION")
            .trim()
            .trim_matches('"');
        assert_eq!(c, API_VERSION);
        let parts: Vec<_> = API_VERSION.split('.').collect();
        assert!(parts.len() == 3 && parts.iter().all(|p| p.parse::<u32>().is_ok()));
    }

    #[test]
    fn register_messages_without_api_version_still_parse() {
        let old = r#"{"client_id":"00000000-0000-0000-0000-000000000001","hostname":"h","version":"v",
            "capabilities":{"sample_rate":48000,"channels":1,"sample_format":"I32",
            "clock_source":"c","device_name":"d"}}"#;
        let req: RegisterRequest = serde_json::from_str(old).unwrap();
        assert_eq!(req.api_version, None);
        assert!(!serde_json::to_string(&req).unwrap().contains("api_version"));
    }

    #[test]
    fn bssids_round_trip() {
        let b: Bssid = "04:f4:1c:07:93:52".parse().unwrap();
        assert_eq!(b.0, [4, 0xf4, 0x1c, 7, 0x93, 0x52]);
        assert_eq!(serde_json::to_string(&b).unwrap(), "\"04:f4:1c:07:93:52\"");
        assert!("04:f4:1c:07:93".parse::<Bssid>().is_err());
        assert!("04:f4:1c:07:93:52:00".parse::<Bssid>().is_err());
        assert!("04:f4:1c:07:93:zz".parse::<Bssid>().is_err());
    }

    #[test]
    fn trigger_and_validation_wire_format() {
        let t = TriggerConfig::default();
        let v = serde_json::to_value(&t).unwrap();
        assert_eq!(v["trigger_type"], "energy");
        assert_eq!(v["threshold_db"], 10.0);
        assert_eq!(v["frame_ms"], 100);
        assert_eq!(serde_json::from_value::<TriggerConfig>(v).unwrap(), t);
        let v = serde_json::to_value(ValidationConfig::None).unwrap();
        assert_eq!(v, serde_json::json!({ "validation_type": "none" }));
        // Types from a newer peer are kept apart, not an error.
        let t: TriggerConfig =
            serde_json::from_str(r#"{"trigger_type":"spectral","x":1}"#).unwrap();
        assert_eq!(t, TriggerConfig::Unknown);
        assert_eq!(t.gate(), GateConfig::default());
        let v: ValidationConfig =
            serde_json::from_str(r#"{"validation_type":"ensemble","threshold":0.5}"#).unwrap();
        assert_eq!(v, ValidationConfig::Unknown);
        let v: ValidationConfig = serde_json::from_str(
            r#"{"validation_type":"model","validation_model":"drongonet-micro"}"#,
        )
        .unwrap();
        assert_eq!(
            v,
            ValidationConfig::Model {
                validation_model: "drongonet-micro".into(),
                validation_threshold: None
            }
        );
        let v = serde_json::to_value(ValidationConfig::Model {
            validation_model: "m".into(),
            validation_threshold: Some(0.4),
        })
        .unwrap();
        assert_eq!(
            v,
            serde_json::json!({ "validation_type": "model", "validation_model": "m",
                                "validation_threshold": 0.4f32 })
        );
    }

    #[test]
    fn register_response_from_older_server_uses_the_gate() {
        let r: RegisterResponse = serde_json::from_str(
            r#"{"udp_timesync_port":2473,"heartbeat_interval_s":5,"chunk_secs":9.0,
                "gate":{"band_low_hz":2000,"band_high_hz":8000,"threshold_db":6,"frame_ms":50}}"#,
        )
        .unwrap();
        let g = GateConfig {
            band_low_hz: 2000.0,
            band_high_hz: 8000.0,
            threshold_db: 6.0,
            frame_ms: 50,
        };
        assert_eq!(r.trigger(), TriggerConfig::Energy(g));
        assert_eq!(r.validation(), ValidationConfig::None);
        // And an older client still finds `gate` in a new server's reply.
        let new = RegisterResponse {
            trigger: Some(TriggerConfig::default()),
            validation: Some(ValidationConfig::None),
            ..r
        };
        let v = serde_json::to_value(&new).unwrap();
        assert!(v["gate"].is_object());
        assert_eq!(v["trigger"]["trigger_type"], "energy");
    }

    #[test]
    fn heartbeat_response_from_older_server() {
        let r: HeartbeatResponse =
            serde_json::from_str(r#"{"mode":{"mode":"live","interval_s":300}}"#).unwrap();
        assert_eq!(r.trigger, None);
        assert_eq!(r.validation, None);
    }

    #[test]
    fn commands_wire_format() {
        let c = Command {
            command_id: 1_762_950_000_000,
            kind: CommandKind::Reboot,
        };
        let v = serde_json::to_value(&c).unwrap();
        assert_eq!(
            v,
            serde_json::json!({ "command_id": 1_762_950_000_000i64, "command": "reboot" })
        );
        let c = Command {
            command_id: 7,
            kind: CommandKind::Logging {
                log_level: LogLevel::On,
                log_duration_s: 600,
            },
        };
        let v = serde_json::to_value(&c).unwrap();
        assert_eq!(
            v,
            serde_json::json!({ "command_id": 7, "command": "logging", "log_level": "on",
                                "log_duration_s": 600 })
        );
        assert_eq!(serde_json::from_value::<Command>(v).unwrap(), c);
        // From a newer peer: kept apart, not an error.
        let c: Command =
            serde_json::from_str(r#"{"command_id":9,"command":"format_card"}"#).unwrap();
        assert_eq!(c.kind, CommandKind::Unknown);
        let l: LogLevel = serde_json::from_str(r#""trace""#).unwrap();
        assert_eq!(l, LogLevel::Unknown);
        let r: HeartbeatResponse =
            serde_json::from_str(r#"{"mode":{"mode":"live","interval_s":300}}"#).unwrap();
        assert!(r.commands.is_empty());
        assert!(!serde_json::to_string(&r).unwrap().contains("commands"));
    }

    #[test]
    fn heartbeats_carry_acks_reboot_reasons_and_logs() {
        let hb: Heartbeat = serde_json::from_str(
            r#"{"clock":null,"uptime_s":3,"chunks_sent":0,"chunks_gated":0,"queue_len":0,
                "capture_dropouts":0,"acks":[{"command_id":5,"result":"done"}],
                "reboot_reason":"requested",
                "log":{"level":"on","remaining_s":42,"now_ms":3000,"dropped":2,
                       "lines":[[2900,"wifi: connected"],[2950,"a \"quoted\" line"]]}}"#,
        )
        .unwrap();
        assert_eq!(
            hb.acks,
            [CommandAck {
                command_id: 5,
                result: CommandResult::Done
            }]
        );
        assert_eq!(hb.reboot_reason, Some(RebootReason::Requested));
        let log = hb.log.unwrap();
        assert_eq!(log.lines[1], LogLine(2950, "a \"quoted\" line".into()));
        assert_eq!((log.dropped, log.remaining_s), (2, Some(42)));
        // A heartbeat from before API 1.1.0.
        let old: Heartbeat = serde_json::from_str(
            r#"{"clock":null,"uptime_s":3,"chunks_sent":0,"chunks_gated":0,"queue_len":0,
                "capture_dropouts":0}"#,
        )
        .unwrap();
        assert!(old.acks.is_empty() && old.reboot_reason.is_none() && old.log.is_none());
        let r: RebootReason = serde_json::from_str(r#""cosmic_ray""#).unwrap();
        assert_eq!(r, RebootReason::Unknown);
    }

    #[test]
    fn firmware_update_wire_format() {
        // Registration (contract §2).
        let reg = serde_json::json!({
            "client_id": "00000000-0000-0000-0000-000000000001", "hostname": "h", "version": "v",
            "api_version": "1.2.0",
            "capabilities": { "sample_rate": 48000, "channels": 1, "sample_format": "I32",
                              "clock_source": "c", "device_name": "d" },
            "build": { "hardware_id": "esp32-s3 on xiao-proto-rev0.1", "tag": "v1.3.0",
                       "commit": "3c48fb8", "dirty": false, "describe": "v1.3.0",
                       "built_at": "2026-11-12T23:10:00Z" },
            "ota": true });
        let req: RegisterRequest = serde_json::from_value(reg.clone()).unwrap();
        let b = req.build.clone().unwrap();
        assert!(req.ota && b.is_stable());
        assert_eq!(serde_json::to_value(&req).unwrap(), reg);
        // A development build: no tag, and the tag is still sent (as null).
        let dev: BuildInfo = serde_json::from_value(serde_json::json!({
            "hardware_id": "x", "tag": null, "commit": "3c48fb8", "dirty": true,
            "describe": "v1.3.0-2-g3c48fb8-dirty", "built_at": "2026-11-12T23:10:00Z" }))
        .unwrap();
        assert!(!dev.is_stable());
        assert_eq!(
            serde_json::to_value(&dev).unwrap()["tag"],
            serde_json::Value::Null
        );
        // Older clients leave both out.
        let mut old = reg;
        let o = old.as_object_mut().unwrap();
        o.remove("build");
        o.remove("ota");
        let req: RegisterRequest = serde_json::from_value(old.clone()).unwrap();
        assert!(req.build.is_none() && !req.ota);
        assert_eq!(serde_json::to_value(&req).unwrap(), old);

        // The offer (§4): exactly these keys.
        let offer = serde_json::json!({
            "firmware_build_id": 42, "firmware_url": "/api/v1/client/firmware/42.bin",
            "firmware_size": 1283456, "firmware_md5": "0123456789abcdef0123456789abcdef",
            "firmware_describe": "v1.3.0-2-g3c48fb8" });
        let o: FirmwareOffer = serde_json::from_value(offer.clone()).unwrap();
        assert_eq!(o.firmware_url, firmware_path(42));
        let r = HeartbeatResponse {
            mode: ModeSetting::default(),
            trigger: None,
            validation: None,
            commands: Vec::new(),
            firmware_offer: Some(o),
        };
        let v = serde_json::to_value(&r).unwrap();
        assert_eq!(v["firmware_offer"], offer);
        assert_eq!(serde_json::from_value::<HeartbeatResponse>(v).unwrap(), r);
        let r: HeartbeatResponse =
            serde_json::from_str(r#"{"mode":{"mode":"live","interval_s":300}}"#).unwrap();
        assert!(r.firmware_offer.is_none());
        assert!(!serde_json::to_string(&r).unwrap().contains("firmware"));

        // The status (§5).
        for (state, s) in [
            ("deferred", FirmwareUpdateState::Deferred),
            ("failed", FirmwareUpdateState::Failed),
            ("rolled_back", FirmwareUpdateState::RolledBack),
        ] {
            let j = serde_json::json!({ "build_id": 42, "state": state, "detail": "battery 34%" });
            let st: FirmwareStatus = serde_json::from_value(j.clone()).unwrap();
            assert_eq!((st.build_id, st.state), (42, s));
            assert_eq!(serde_json::to_value(&st).unwrap(), j);
        }
        let hb: Heartbeat = serde_json::from_str(
            r#"{"clock":null,"uptime_s":3,"chunks_sent":0,"chunks_gated":0,"queue_len":0,
                "capture_dropouts":0,"firmware_status":{"build_id":7,"state":"exploded"}}"#,
        )
        .unwrap();
        let st = hb.firmware_status.unwrap();
        assert_eq!((st.state, st.detail), (FirmwareUpdateState::Unknown, None));
    }

    #[test]
    fn battery_charging_is_optional() {
        // As the MAX17048 driver reports it today: no charger status.
        let b: BatteryStatus =
            serde_json::from_str(r#"{"percent":87.5,"voltage_v":4.0,"rate_pct_per_h":-1.04}"#)
                .unwrap();
        assert_eq!(b.charging, None);
        assert!(!serde_json::to_string(&b).unwrap().contains("charging"));
        let b: BatteryStatus =
            serde_json::from_str(r#"{"percent":60,"voltage_v":3.9,"charging":true}"#).unwrap();
        assert_eq!(b.charging, Some(true));
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
    /// The client's battery, from its fuel gauge (microcontroller boards with
    /// one, while a battery is connected).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub battery: Option<BatteryStatus>,
    /// The client's storage card, where it keeps a copy of its recordings
    /// (boards with a card slot, whether or not a card is in it).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub storage: Option<StorageStatus>,
    /// The peripherals the client is configured with, and whether each answers.
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub devices: Vec<DeviceStatus>,
    /// How the client uses its radio (clients that can switch modes).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub mode: Option<ModeStatus>,
    /// The trigger the client runs (clients that report it).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub trigger: Option<TriggerConfig>,
    /// The validation the client runs (clients that report it).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub validation: Option<ValidationConfig>,
    /// Why the client runs another validation than asked (e.g. no model on
    /// its card), or how loading the model goes.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub validation_note: Option<String>,
    /// Triggered recordings validation kept from uploading, since the client
    /// started.
    #[serde(default)]
    pub chunks_rejected: u64,
    /// The commands the client has handled since its last delivered heartbeat
    /// (API 1.1.0): the server stops sending each once it is acknowledged.
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub acks: Vec<CommandAck>,
    /// Why the client last started, in its first heartbeats after every
    /// start until one is delivered (API 1.1.0, clients that can tell).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub reboot_reason: Option<RebootReason>,
    /// The client's trace log: whether it is capturing, and the lines captured
    /// since the last delivered heartbeat (API 1.1.0).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub log: Option<LogBatch>,
    /// What became of a firmware offer, in every heartbeat until one is
    /// delivered (API 1.2.0, clients that take updates). An update that
    /// worked is not reported here: the client registers with its new build.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub firmware_status: Option<FirmwareStatus>,
}

/// A client's report on a firmware build it was offered (API 1.2.0).
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct FirmwareStatus {
    /// The offered build ([`FirmwareOffer::firmware_build_id`]).
    pub build_id: i64,
    pub state: FirmwareUpdateState,
    /// Why: e.g. "battery 34%", or the updater's error.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub detail: Option<String>,
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum FirmwareUpdateState {
    /// Not now (e.g. its battery is low): the server keeps offering it.
    Deferred,
    /// Downloading or flashing failed: it is not offered to this client again
    /// until its firmware selection changes.
    Failed,
    /// The client started the build, which never confirmed itself, and its
    /// bootloader went back to the previous one. As `Failed`.
    RolledBack,
    /// A state this build does not know (from a newer peer).
    #[serde(other)]
    Unknown,
}

/// A firmware build for the client to install (API 1.2.0). The keys are
/// unique across the heartbeat reply: the microcontroller client finds
/// values by key anywhere in it.
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct FirmwareOffer {
    pub firmware_build_id: i64,
    /// Path of the image on the server's client port (see [`firmware_path`]).
    pub firmware_url: String,
    /// Its size in bytes.
    pub firmware_size: u64,
    /// Its MD5, 32 lowercase hex digits (also in the download's `x-MD5`).
    pub firmware_md5: String,
    pub firmware_describe: String,
}

/// Where a client downloads firmware build `build_id`: `GET` on its own
/// port, answered with the image and `Content-Length` and `x-MD5` headers.
pub fn firmware_path(build_id: i64) -> String {
    format!("{CLIENT_API_PREFIX}/firmware/{build_id}.bin")
}

/// A maintenance command for one client, passed on in heartbeat replies until
/// the client acknowledges it (API 1.1.0). A client runs each command once:
/// ids increase over time, and it runs only ids above the highest it has run,
/// remembering that across its restarts. The field names are unique across
/// the reply, as the microcontroller client finds values by key.
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct Command {
    /// Assigned by the server: at least the time it was made in ms since
    /// 1970, and above every earlier id.
    pub command_id: i64,
    #[serde(flatten)]
    pub kind: CommandKind,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
#[serde(tag = "command", rename_all = "snake_case")]
pub enum CommandKind {
    /// Restart cleanly: finish or give up the upload in progress, close the
    /// storage card, restart, and report `reboot_reason: requested`.
    Reboot,
    /// Capture the client's log for `log_duration_s` from when the client
    /// gets this (it turns capture off by itself then), or stop (`off`).
    Logging {
        log_level: LogLevel,
        #[serde(default)]
        log_duration_s: u32,
    },
    /// A command this build does not know (from a newer peer): acknowledged
    /// as `unsupported`.
    #[serde(other)]
    Unknown,
}

impl CommandKind {
    pub fn name(&self) -> &'static str {
        match self {
            Self::Reboot => "reboot",
            Self::Logging { .. } => "logging",
            Self::Unknown => "unknown",
        }
    }
}

/// How much of its log a client captures. The firmware has one level today:
/// `on` captures every line it logs. Finer levels may be added under the same
/// field; a client treats a level it does not know as `on`.
#[derive(Debug, Clone, Copy, Default, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum LogLevel {
    #[default]
    Off,
    On,
    /// A level this build does not know.
    #[serde(other)]
    Unknown,
}

/// Longest trace logging the server asks for at once (s).
pub const MAX_LOG_DURATION_S: u32 = 86_400;

/// A client's answer to a command, in its next heartbeat.
#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
pub struct CommandAck {
    pub command_id: i64,
    pub result: CommandResult,
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum CommandResult {
    /// Done (a reboot: acknowledged after the restart).
    Done,
    /// The client does not implement this command.
    Unsupported,
    /// The client tried, and could not.
    Failed,
    #[serde(other)]
    Unknown,
}

/// Why a client last started.
#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum RebootReason {
    /// The server asked it to (a `reboot` command).
    Requested,
    PowerOn,
    Watchdog,
    Brownout,
    /// A crash.
    Panic,
    /// A reset the client can name but is none of the above (e.g. its reset
    /// pin, or a restart it was not asked for).
    Other,
    #[serde(other)]
    Unknown,
}

/// A client's trace log in one heartbeat.
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct LogBatch {
    /// Whether the client captures its log now.
    pub level: LogLevel,
    /// Seconds until capture turns itself off, while it is on.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub remaining_s: Option<u32>,
    /// The client's clock (ms since it started) when it made the heartbeat:
    /// with it, the lines' times map onto the server's clock.
    pub now_ms: u64,
    /// The lines captured since the last delivered heartbeat, oldest first
    /// (as many as fit; the rest follow).
    #[serde(default)]
    pub lines: Vec<LogLine>,
    /// Lines lost since the last delivered heartbeat: overwritten before they
    /// could be sent, or logged while the capture buffer was busy.
    #[serde(default)]
    pub dropped: u32,
}

/// One line of a client's log: `[ms since the client started, text]`.
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct LogLine(pub u64, pub String);

/// What a client's validation decided about one triggered recording: sent
/// for every recording the model read, uploaded or not, so its results and
/// cost can be checked (`POST /{id}/validation`).
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct ValidationReport {
    pub model: String,
    /// The model's bird score, 0..1.
    pub score: f32,
    pub threshold: f32,
    /// Whether the recording is uploaded.
    pub upload: bool,
    /// Server-clock time of the first sample the model read, when known.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub window_start: Option<Timestamp>,
    /// Audio the model read (ms).
    pub window_ms: u32,
    /// Time from the trigger to the decision (ms): waiting for the rest of
    /// the window, then computing.
    pub latency_ms: u32,
    /// Resampling and spectrogram (ms).
    pub frontend_ms: f32,
    /// The model itself (ms).
    pub invoke_ms: f32,
    /// Tensor arena the model uses (bytes).
    pub arena_bytes: u32,
}

/// The server's answer to a heartbeat. Older servers answer with no body.
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct HeartbeatResponse {
    /// The mode the operator wants the client in.
    pub mode: ModeSetting,
    /// The trigger the operator wants the client to run; older servers leave
    /// it out. Passed on at every heartbeat, so a change applies without the
    /// client registering again.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub trigger: Option<TriggerConfig>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub validation: Option<ValidationConfig>,
    /// Maintenance commands still to be acknowledged (API 1.1.0; only to
    /// clients that speak it).
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub commands: Vec<Command>,
    /// A firmware build to install, while the one the operator chose for the
    /// client differs from the one it runs (API 1.2.0; only to clients that
    /// take updates).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub firmware_offer: Option<FirmwareOffer>,
}

/// How a client uses its radio.
#[derive(Debug, Clone, Copy, Default, Serialize, Deserialize, PartialEq, Eq, Hash)]
#[serde(rename_all = "snake_case")]
pub enum OperationMode {
    /// Always connected: audio is uploaded as it is recorded.
    #[default]
    Live,
    /// Wi-Fi off between contacts, one every `interval_s`. Audio is kept on
    /// the storage card meanwhile, and uploaded (then deleted from the card)
    /// at the next contact. Each contact measures the clock again, so audio
    /// recorded in between keeps accurate timestamps.
    Batch,
}

/// Seconds between a batch client's contacts, unless set otherwise.
pub const DEFAULT_BATCH_INTERVAL_S: u32 = 300;
/// The shortest and longest interval between contacts the server accepts.
pub const MIN_BATCH_INTERVAL_S: u32 = 30;
pub const MAX_BATCH_INTERVAL_S: u32 = 3600;

fn default_batch_interval() -> u32 {
    DEFAULT_BATCH_INTERVAL_S
}

/// A mode the operator sets for a client.
#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
pub struct ModeSetting {
    pub mode: OperationMode,
    /// Batch mode: seconds from the start of one contact to the next.
    #[serde(default = "default_batch_interval")]
    pub interval_s: u32,
}

impl Default for ModeSetting {
    fn default() -> Self {
        Self {
            mode: OperationMode::Live,
            interval_s: DEFAULT_BATCH_INTERVAL_S,
        }
    }
}

/// The mode a client runs in, as its heartbeat reports it.
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct ModeStatus {
    pub mode: OperationMode,
    /// Batch mode: seconds from the start of one contact to the next.
    pub interval_s: u32,
    /// Recordings kept on the card that are still to be uploaded.
    #[serde(default)]
    pub backlog: u32,
    #[serde(default)]
    pub backlog_bytes: u64,
    /// All audio the client recorded before this time (server clock) has
    /// been uploaded. Batch mode: the server holds back other clients' calls
    /// after it, so they can still be grouped with this client's.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub delivered_through: Option<Timestamp>,
    /// Batch mode: seconds until the client's next contact, in the last
    /// heartbeat before its radio goes off.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub next_contact_s: Option<u32>,
    /// Share of the time the radio was on since the mode started, percent.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub radio_on_pct: Option<f32>,
    /// About the latest time the radio was off: how long, and how far the
    /// client's clock moved against the server's (and against its access
    /// point's) beyond what the drift measured before predicted (ns). Audio
    /// recorded meanwhile is timestamped between the measurements on either
    /// side, so its error is at most about this.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub gap: Option<RadioGap>,
    /// Why the client is not in the mode the server asked for.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub note: Option<String>,
}

/// See [`ModeStatus::gap`].
#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq)]
pub struct RadioGap {
    pub off_s: u32,
    pub clock_ns: i64,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub tsf_ns: Option<i64>,
}

/// A battery, as its fuel gauge reports it.
#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq)]
pub struct BatteryStatus {
    /// State of charge, percent (a gauge may report a little over 100).
    pub percent: f32,
    /// Cell voltage.
    pub voltage_v: f32,
    /// How fast the gauge's charge estimate changes, percent per hour. A rise
    /// does not prove the cell is charging (see `charging`).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub rate_pct_per_h: Option<f32>,
    /// Whether the charger says it is charging the cell, from boards that can
    /// read its status. Never inferred from the rate; absent when unknown.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub charging: Option<bool>,
}

/// A storage card and what the client has written to it.
#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq)]
pub struct StorageStatus {
    /// Whether a card is in and mounted.
    pub present: bool,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub total_bytes: Option<u64>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub free_bytes: Option<u64>,
    /// Recordings saved since the client started.
    #[serde(default)]
    pub recordings: u32,
    /// Bytes of recordings saved since the client started.
    #[serde(default)]
    pub recorded_bytes: u64,
    /// Writes that failed since the client started (a card pulled out, or full).
    #[serde(default)]
    pub errors: u32,
    /// Bytes not written since the client started: no card, or the card was
    /// too slow to keep up.
    #[serde(default)]
    pub dropped_bytes: u64,
    /// The client closed the card because its battery is low (a card that
    /// loses power while it writes can be ruined); `present` is then false.
    /// It opens the card again once the battery recovers.
    #[serde(default, skip_serializing_if = "std::ops::Not::not")]
    pub paused: bool,
}

/// A peripheral on one of the client's buses.
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct DeviceStatus {
    /// Its driver, e.g. `max17048` or `sd`.
    pub kind: String,
    /// `i2c` or `spi`.
    pub bus: String,
    /// The I2C address, or the SPI chip select pin.
    pub address: i32,
    /// Whether it answers.
    pub present: bool,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct AudioAccepted {
    pub duration_s: f32,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ErrorBody {
    pub error: String,
}
