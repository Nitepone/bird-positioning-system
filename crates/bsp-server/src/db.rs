//! SQLite persistence for clients, detections and raw events.
//!
//! All queries are small; they run directly on the caller's thread behind a mutex.

use crate::firmware::{FirmwareBuild, FirmwarePolicy, FirmwareState};
use bsp_core::detection::Detection;
use bsp_core::locator::Vec3;
use bsp_proto::{
    BatteryStatus, BuildInfo, Capabilities, ClientId, ClockStatus, CommandResult, DeviceStatus,
    Heartbeat, LogLevel, ModeSetting, ModeStatus, NANOS_PER_SEC, RebootReason, RegisterRequest,
    StorageStatus, Timestamp, TriggerConfig, Uuid, ValidationConfig,
};
use rusqlite::{Connection, OptionalExtension, params};
use serde::{Deserialize, Serialize};
use std::path::Path;
use std::sync::Mutex;

pub struct Db {
    conn: Mutex<Connection>,
}

#[derive(Debug, Clone, Serialize)]
pub struct ClientRecord {
    pub id: ClientId,
    /// Set by the operator; empty when unset.
    pub name: String,
    /// The client's own suggestion from its latest registration; empty when none.
    pub suggested_name: String,
    pub position: Option<Vec3>,
    pub hostname: String,
    /// The client's own build.
    pub version: String,
    /// The API version it registered with; None when it gave none (old firmware).
    pub api_version: Option<String>,
    pub capabilities: Option<Capabilities>,
    pub first_seen: Timestamp,
    pub last_seen: Timestamp,
    pub last_addr: String,
    pub clock: Option<ClockStatus>,
    /// From the latest heartbeat.
    pub health: Option<ClientHealth>,
    /// The mode the operator set, which heartbeat replies pass on; the mode
    /// the client runs in is in its health.
    pub mode: ModeSetting,
    /// When the client last started, and why, as it reported (clients that
    /// can tell, from API 1.1.0).
    pub last_reboot: Option<RebootInfo>,
    /// The build it runs, from its latest registration (API 1.2.0).
    pub build: Option<BuildInfo>,
    /// Its firmware takes updates over the air (API 1.2.0, ESP32 boards).
    pub ota: bool,
    /// The build the operator chose for it; None until one is chosen (it then
    /// stays on what it runs). Shown through the client's `firmware` view.
    #[serde(skip)]
    pub firmware_policy: Option<FirmwarePolicy>,
    #[serde(skip)]
    pub firmware: FirmwareState,
}

/// A client's latest start, as it reported in its first heartbeat after it.
#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq)]
pub struct RebootInfo {
    pub reason: RebootReason,
    /// When it started (server clock): the heartbeat's arrival less its uptime.
    pub at: Timestamp,
}

/// Whether a client captures its trace log, as its latest heartbeat said.
#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq)]
pub struct LoggingStatus {
    pub level: LogLevel,
    /// Seconds until the client turns capture off by itself, from the
    /// heartbeat's arrival (`ClientHealth::at`).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub remaining_s: Option<u32>,
}

/// A maintenance command for a client (see [`bsp_proto::Command`]).
#[derive(Debug, Clone, Serialize, PartialEq)]
pub struct CommandRecord {
    pub id: i64,
    #[serde(skip)]
    pub client_id: ClientId,
    /// `reboot` or `logging`.
    pub command: String,
    /// The command's own fields (`log_level`, `log_duration_s`).
    pub params: serde_json::Value,
    pub created_at: Timestamp,
    /// When a heartbeat reply last carried it.
    pub delivered_at: Option<Timestamp>,
    pub acked_at: Option<Timestamp>,
    /// How the client answered, or `superseded` when a newer command of the
    /// same kind took its place before it was acknowledged. None while pending.
    pub result: Option<String>,
}

impl CommandRecord {
    /// As carried in heartbeat replies; None if it cannot be (a corrupt row).
    pub fn to_command(&self) -> Option<bsp_proto::Command> {
        let mut v = self.params.clone();
        let obj = v.as_object_mut()?;
        obj.insert("command_id".into(), self.id.into());
        obj.insert("command".into(), self.command.clone().into());
        serde_json::from_value(v).ok()
    }
}

/// What a client's latest heartbeat said about the client itself (its clock
/// is kept apart, in [`ClientRecord::clock`]). Kept, so that a client that has
/// gone quiet still shows how it was, e.g. its battery running down.
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct ClientHealth {
    /// When the heartbeat arrived.
    pub at: Timestamp,
    pub uptime_s: u64,
    pub chunks_sent: u64,
    pub chunks_gated: u64,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub battery: Option<BatteryStatus>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub storage: Option<StorageStatus>,
    #[serde(default)]
    pub devices: Vec<DeviceStatus>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub mode: Option<ModeStatus>,
    /// The trigger and validation the client reports running.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub trigger: Option<TriggerConfig>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub validation: Option<ValidationConfig>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub validation_note: Option<String>,
    #[serde(default)]
    pub chunks_rejected: u64,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub logging: Option<LoggingStatus>,
}

impl ClientHealth {
    pub fn of(hb: &Heartbeat, at: Timestamp) -> Self {
        Self {
            at,
            uptime_s: hb.uptime_s,
            chunks_sent: hb.chunks_sent,
            chunks_gated: hb.chunks_gated,
            battery: hb.battery,
            storage: hb.storage,
            devices: hb.devices.clone(),
            mode: hb.mode.clone(),
            trigger: hb.trigger.clone(),
            validation: hb.validation.clone(),
            validation_note: hb.validation_note.clone(),
            chunks_rejected: hb.chunks_rejected,
            logging: hb.log.as_ref().map(|l| LoggingStatus {
                level: l.level,
                remaining_s: l.remaining_s,
            }),
        }
    }
}

/// Detection search criteria; every field is optional.
#[derive(Debug, Clone, Default, serde::Deserialize)]
pub struct DetectionFilter {
    /// Only detections older than this (pagination cursor).
    pub before: Option<Timestamp>,
    /// Time range `[from, to)`.
    pub from: Option<Timestamp>,
    pub to: Option<Timestamp>,
    /// Exact scientific name (case-insensitive).
    pub species: Option<String>,
    /// Text contained in the common or scientific name (case-insensitive).
    pub q: Option<String>,
    /// Heard by this client.
    pub client: Option<ClientId>,
    pub unexpected: Option<bool>,
    /// Has a direction estimate.
    pub located: Option<bool>,
    pub min_confidence: Option<f32>,
    /// Leave out the species the operator muted (`Settings::muted_species`).
    pub hide_muted: Option<bool>,
    /// Scientific names to leave out (case-insensitive): filled in by the
    /// server from the muted species when `hide_muted` is set.
    #[serde(skip)]
    pub exclude_species: Vec<String>,
}

/// Which events to list; every field is optional.
#[derive(Debug, Clone, Default)]
pub struct EventFilter {
    pub after_id: Option<i64>,
    pub before_id: Option<i64>,
    pub client_id: Option<ClientId>,
    pub kinds: Vec<String>,
    pub exclude_kinds: Vec<String>,
}

/// A species seen at least once, for search suggestions.
#[derive(Debug, Clone, Serialize)]
pub struct SpeciesSeen {
    pub scientific: String,
    pub common: String,
    pub count: i64,
    pub last_seen: Timestamp,
}

/// Audio of one client around a detection.
pub struct Clip {
    /// Timestamp of the first sample.
    pub start: Timestamp,
    /// FLAC, or WAV for clips stored before FLAC (see [`crate::clip_audio`]).
    pub audio: Vec<u8>,
}

/// How long battery readings are kept, for the client page's battery chart.
pub const BATTERY_HISTORY_RETENTION_NS: i64 = 7 * 86_400 * NANOS_PER_SEC;
/// At most one battery reading is kept per this interval and client (plus
/// any reading where the reported charging state changes), so a client's
/// week of heartbeats stays a few thousand rows.
pub const BATTERY_HISTORY_STEP_NS: i64 = 60 * NANOS_PER_SEC;
/// The width of [`Db::detections_hourly`]'s buckets.
pub const HOUR_NS: i64 = 3600 * NANOS_PER_SEC;

/// A battery reading from a heartbeat, as kept in the history.
#[derive(Debug, Clone, Copy, Serialize, PartialEq)]
pub struct BatteryPoint {
    /// When the heartbeat arrived.
    pub at: Timestamp,
    pub percent: f32,
    pub voltage_v: f32,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub charging: Option<bool>,
}

/// What [`Db::maintain`] changed.
#[derive(Debug, Default, PartialEq, Eq)]
pub struct Maintenance {
    pub detections_removed: usize,
    pub clips_converted: usize,
}

#[derive(Debug, Clone, Serialize)]
pub struct Event {
    pub id: i64,
    pub ts: Timestamp,
    pub client_id: Option<ClientId>,
    pub kind: String,
    pub detail: serde_json::Value,
}

const SCHEMA: &str = "
CREATE TABLE IF NOT EXISTS clients (
    id TEXT PRIMARY KEY,
    name TEXT NOT NULL DEFAULT '',
    suggested_name TEXT NOT NULL DEFAULT '',
    x REAL, y REAL, z REAL,
    hostname TEXT NOT NULL DEFAULT '',
    version TEXT NOT NULL DEFAULT '',
    api_version TEXT,
    capabilities TEXT,
    first_seen INTEGER NOT NULL,
    last_seen INTEGER NOT NULL,
    last_addr TEXT NOT NULL DEFAULT '',
    clock TEXT,
    health TEXT,
    mode TEXT,
    last_reboot TEXT,
    build TEXT,
    ota INTEGER NOT NULL DEFAULT 0,
    firmware_policy TEXT,
    firmware_state TEXT
);
CREATE TABLE IF NOT EXISTS detections (
    id TEXT PRIMARY KEY,
    time INTEGER NOT NULL,
    body TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS detections_time ON detections(time);
CREATE TABLE IF NOT EXISTS detection_clips (
    detection_id TEXT NOT NULL,
    client_id TEXT NOT NULL,
    start INTEGER NOT NULL,
    audio BLOB NOT NULL,
    PRIMARY KEY (detection_id, client_id)
);
CREATE INDEX IF NOT EXISTS detection_clips_client_start ON detection_clips(client_id, start);
CREATE TABLE IF NOT EXISTS settings (
    key TEXT PRIMARY KEY,
    value TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS battery_history (
    client_id TEXT NOT NULL,
    ts INTEGER NOT NULL,
    percent REAL NOT NULL,
    voltage_v REAL NOT NULL,
    charging INTEGER
);
CREATE INDEX IF NOT EXISTS battery_history_client_ts ON battery_history(client_id, ts);
CREATE TABLE IF NOT EXISTS client_commands (
    id INTEGER PRIMARY KEY,
    client_id TEXT NOT NULL,
    command TEXT NOT NULL,
    params TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    delivered_at INTEGER,
    acked_at INTEGER,
    result TEXT
);
CREATE INDEX IF NOT EXISTS client_commands_client ON client_commands(client_id, id);
CREATE TABLE IF NOT EXISTS firmware_builds (
    id INTEGER PRIMARY KEY,
    hardware_id TEXT NOT NULL,
    tag TEXT,
    \"commit\" TEXT NOT NULL,
    dirty INTEGER NOT NULL,
    describe TEXT NOT NULL,
    built_at TEXT NOT NULL,
    size INTEGER NOT NULL,
    md5 TEXT NOT NULL UNIQUE,
    uploaded_at INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS events (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    ts INTEGER NOT NULL,
    client_id TEXT,
    kind TEXT NOT NULL,
    detail TEXT NOT NULL
);
";

fn json<T: Serialize>(v: &T) -> String {
    serde_json::to_string(v).expect("serialisable")
}

fn column_exists(conn: &Connection, table: &str, column: &str) -> rusqlite::Result<bool> {
    conn.prepare(&format!(
        "SELECT 1 FROM pragma_table_info('{table}') WHERE name = ?1"
    ))?
    .exists([column])
}

/// Upgrades a database created before `column` was added to `SCHEMA`.
fn add_column_if_missing(
    conn: &Connection,
    table: &str,
    column: &str,
    decl: &str,
) -> rusqlite::Result<()> {
    if !column_exists(conn, table, column)? {
        conn.execute_batch(&format!("ALTER TABLE {table} ADD COLUMN {column} {decl}"))?;
    }
    Ok(())
}

/// Adds a heartbeat's battery reading to the history (unless one was kept
/// less than [`BATTERY_HISTORY_STEP_NS`] before it with the same charging
/// state), and drops the client's readings past the retention.
fn record_battery(
    conn: &Connection,
    id: ClientId,
    at: Timestamp,
    b: &BatteryStatus,
) -> rusqlite::Result<()> {
    let id = id.to_string();
    let last: Option<(i64, Option<bool>)> = conn
        .prepare_cached(
            "SELECT ts, charging FROM battery_history WHERE client_id = ?1 ORDER BY ts DESC LIMIT 1",
        )?
        .query_row([&id], |r| Ok((r.get(0)?, r.get(1)?)))
        .optional()?;
    if last
        .is_some_and(|(ts, charging)| at - ts < BATTERY_HISTORY_STEP_NS && charging == b.charging)
    {
        return Ok(());
    }
    conn.prepare_cached(
        "INSERT INTO battery_history (client_id, ts, percent, voltage_v, charging)
         VALUES (?1, ?2, ?3, ?4, ?5)",
    )?
    .execute(params![
        id,
        at,
        b.percent as f64,
        b.voltage_v as f64,
        b.charging
    ])?;
    conn.prepare_cached("DELETE FROM battery_history WHERE client_id = ?1 AND ts < ?2")?
        .execute(params![id, at - BATTERY_HISTORY_RETENTION_NS])?;
    Ok(())
}

fn row_to_client(r: &rusqlite::Row) -> rusqlite::Result<ClientRecord> {
    let id: String = r.get("id")?;
    let (x, y, z): (Option<f64>, Option<f64>, Option<f64>) =
        (r.get("x")?, r.get("y")?, r.get("z")?);
    let caps: Option<String> = r.get("capabilities")?;
    let clock: Option<String> = r.get("clock")?;
    let health: Option<String> = r.get("health")?;
    let mode: Option<String> = r.get("mode")?;
    let last_reboot: Option<String> = r.get("last_reboot")?;
    let build: Option<String> = r.get("build")?;
    let policy: Option<String> = r.get("firmware_policy")?;
    let firmware: Option<String> = r.get("firmware_state")?;
    Ok(ClientRecord {
        id: id.parse().unwrap_or_default(),
        name: r.get("name")?,
        suggested_name: r.get("suggested_name")?,
        position: match (x, y, z) {
            (Some(x), Some(y), Some(z)) => Some(Vec3 { x, y, z }),
            _ => None,
        },
        hostname: r.get("hostname")?,
        version: r.get("version")?,
        api_version: r.get("api_version")?,
        capabilities: caps.and_then(|c| serde_json::from_str(&c).ok()),
        first_seen: r.get("first_seen")?,
        last_seen: r.get("last_seen")?,
        last_addr: r.get("last_addr")?,
        clock: clock.and_then(|c| serde_json::from_str(&c).ok()),
        health: health.and_then(|h| serde_json::from_str(&h).ok()),
        mode: mode
            .and_then(|m| serde_json::from_str(&m).ok())
            .unwrap_or_default(),
        last_reboot: last_reboot.and_then(|b| serde_json::from_str(&b).ok()),
        build: build.and_then(|b| serde_json::from_str(&b).ok()),
        ota: r.get("ota")?,
        firmware_policy: policy.and_then(|p| serde_json::from_str(&p).ok()),
        firmware: firmware
            .and_then(|f| serde_json::from_str(&f).ok())
            .unwrap_or_default(),
    })
}

fn row_to_firmware(r: &rusqlite::Row) -> rusqlite::Result<FirmwareBuild> {
    Ok(FirmwareBuild {
        id: r.get("id")?,
        hardware_id: r.get("hardware_id")?,
        tag: r.get("tag")?,
        commit: r.get("commit")?,
        dirty: r.get("dirty")?,
        describe: r.get("describe")?,
        built_at: r.get("built_at")?,
        size: r.get::<_, i64>("size")? as u64,
        md5: r.get("md5")?,
        uploaded_at: r.get("uploaded_at")?,
    })
}

/// How long commands are kept once answered.
pub const COMMAND_RETENTION_NS: i64 = 7 * 86_400 * NANOS_PER_SEC;

fn row_to_command(r: &rusqlite::Row) -> rusqlite::Result<CommandRecord> {
    let client: String = r.get("client_id")?;
    let params: String = r.get("params")?;
    Ok(CommandRecord {
        id: r.get("id")?,
        client_id: client.parse().unwrap_or_default(),
        command: r.get("command")?,
        params: serde_json::from_str(&params).unwrap_or_else(|_| serde_json::json!({})),
        created_at: r.get("created_at")?,
        delivered_at: r.get("delivered_at")?,
        acked_at: r.get("acked_at")?,
        result: r.get("result")?,
    })
}

impl Db {
    pub fn open(path: &Path) -> anyhow::Result<Self> {
        Self::init(Connection::open(path)?)
    }

    pub fn in_memory() -> anyhow::Result<Self> {
        Self::init(Connection::open_in_memory()?)
    }

    fn init(conn: Connection) -> anyhow::Result<Self> {
        conn.pragma_update(None, "journal_mode", "WAL")?;
        // Clips were WAV-only before FLAC.
        if column_exists(&conn, "detection_clips", "wav")? {
            conn.execute_batch("ALTER TABLE detection_clips RENAME COLUMN wav TO audio")?;
        }
        conn.execute_batch(SCHEMA)?;
        add_column_if_missing(
            &conn,
            "clients",
            "suggested_name",
            "TEXT NOT NULL DEFAULT ''",
        )?;
        add_column_if_missing(&conn, "clients", "api_version", "TEXT")?;
        add_column_if_missing(&conn, "clients", "health", "TEXT")?;
        add_column_if_missing(&conn, "clients", "mode", "TEXT")?;
        add_column_if_missing(&conn, "clients", "last_reboot", "TEXT")?;
        add_column_if_missing(&conn, "clients", "build", "TEXT")?;
        add_column_if_missing(&conn, "clients", "ota", "INTEGER NOT NULL DEFAULT 0")?;
        add_column_if_missing(&conn, "clients", "firmware_policy", "TEXT")?;
        add_column_if_missing(&conn, "clients", "firmware_state", "TEXT")?;
        Ok(Self {
            conn: Mutex::new(conn),
        })
    }

    /// Records a registration, with the name it suggests already cleaned up.
    /// Returns true if the client is new.
    pub fn register_client(
        &self,
        req: &RegisterRequest,
        suggested_name: &str,
        addr: &str,
        now: Timestamp,
    ) -> rusqlite::Result<bool> {
        let id = req.client_id;
        let conn = self.conn.lock().unwrap();
        let existed = conn
            .query_row(
                "SELECT 1 FROM clients WHERE id = ?1",
                [id.to_string()],
                |_| Ok(()),
            )
            .optional()?
            .is_some();
        conn.execute(
            "INSERT INTO clients (id, hostname, version, api_version, capabilities, first_seen,
                                  last_seen, last_addr, suggested_name, build, ota)
             VALUES (?1, ?2, ?3, ?8, ?4, ?5, ?5, ?6, ?7, ?9, ?10)
             ON CONFLICT(id) DO UPDATE SET hostname = ?2, version = ?3, api_version = ?8,
                                           capabilities = ?4, last_seen = ?5, last_addr = ?6,
                                           suggested_name = ?7, build = ?9, ota = ?10",
            params![
                id.to_string(),
                req.hostname,
                req.version,
                json(&req.capabilities),
                now,
                addr,
                suggested_name,
                req.api_version,
                req.build.as_ref().map(json),
                req.ota
            ],
        )?;
        Ok(!existed)
    }

    /// Updates liveness. Returns false if the client is unknown.
    pub fn touch_client(&self, id: ClientId, addr: &str, now: Timestamp) -> rusqlite::Result<bool> {
        let conn = self.conn.lock().unwrap();
        let n = conn.execute(
            "UPDATE clients SET last_seen = ?2, last_addr = ?3 WHERE id = ?1",
            params![id.to_string(), now, addr],
        )?;
        Ok(n > 0)
    }

    /// Records a heartbeat: liveness, the clock and the client's health.
    /// Returns false if the client is unknown.
    pub fn record_heartbeat(
        &self,
        id: ClientId,
        addr: &str,
        clock: Option<&ClockStatus>,
        health: &ClientHealth,
    ) -> rusqlite::Result<bool> {
        let conn = self.conn.lock().unwrap();
        let n = conn.execute(
            "UPDATE clients SET last_seen = ?2, last_addr = ?3, clock = COALESCE(?4, clock),
                                health = ?5
             WHERE id = ?1",
            params![
                id.to_string(),
                health.at,
                addr,
                clock.map(json),
                json(health)
            ],
        )?;
        if n > 0
            && let Some(b) = health.battery
        {
            record_battery(&conn, id, health.at, &b)?;
        }
        Ok(n > 0)
    }

    /// The client's battery readings at or after `since`, oldest first.
    pub fn battery_history(
        &self,
        id: ClientId,
        since: Timestamp,
    ) -> rusqlite::Result<Vec<BatteryPoint>> {
        let conn = self.conn.lock().unwrap();
        let mut stmt = conn.prepare_cached(
            "SELECT ts, percent, voltage_v, charging FROM battery_history
             WHERE client_id = ?1 AND ts >= ?2 ORDER BY ts",
        )?;
        stmt.query_map(params![id.to_string(), since], |r| {
            Ok(BatteryPoint {
                at: r.get(0)?,
                percent: r.get::<_, f64>(1)? as f32,
                voltage_v: r.get::<_, f64>(2)? as f32,
                charging: r.get(3)?,
            })
        })?
        .collect()
    }

    /// Detections the client heard over the 24 hours up to `now`, counted in
    /// hourly buckets, oldest first: bucket `i` covers
    /// `(now - (24 - i) h, now - (23 - i) h]`. Every bucket is present, empty
    /// hours as 0; older detections (and any after `now`) are left out.
    pub fn detections_hourly(&self, id: ClientId, now: Timestamp) -> rusqlite::Result<[u32; 24]> {
        let from = now - 24 * HOUR_NS;
        let conn = self.conn.lock().unwrap();
        let mut stmt = conn.prepare_cached(
            "SELECT time FROM detections
             WHERE time > ?1 AND time <= ?2 AND EXISTS (
                 SELECT 1 FROM json_each(body, '$.calls') WHERE json_extract(value, '$.client_id') = ?3)",
        )?;
        let mut counts = [0u32; 24];
        for t in stmt.query_map(params![from, now, id.to_string()], |r| r.get::<_, i64>(0))? {
            let i = ((t? - from - 1) / HOUR_NS).clamp(0, 23) as usize;
            counts[i] += 1;
        }
        Ok(counts)
    }

    /// Sets the user-configured name and position. Returns false if unknown.
    pub fn configure_client(
        &self,
        id: ClientId,
        name: &str,
        pos: Option<Vec3>,
    ) -> rusqlite::Result<bool> {
        let conn = self.conn.lock().unwrap();
        let n = conn.execute(
            "UPDATE clients SET name = ?2, x = ?3, y = ?4, z = ?5 WHERE id = ?1",
            params![
                id.to_string(),
                name,
                pos.map(|p| p.x),
                pos.map(|p| p.y),
                pos.map(|p| p.z)
            ],
        )?;
        Ok(n > 0)
    }

    /// Sets the mode the client is to run in. Returns false if unknown.
    pub fn set_client_mode(&self, id: ClientId, mode: &ModeSetting) -> rusqlite::Result<bool> {
        let conn = self.conn.lock().unwrap();
        let n = conn.execute(
            "UPDATE clients SET mode = ?2 WHERE id = ?1",
            params![id.to_string(), json(mode)],
        )?;
        Ok(n > 0)
    }

    pub fn get_client(&self, id: ClientId) -> rusqlite::Result<Option<ClientRecord>> {
        let conn = self.conn.lock().unwrap();
        conn.query_row(
            "SELECT * FROM clients WHERE id = ?1",
            [id.to_string()],
            row_to_client,
        )
        .optional()
    }

    pub fn list_clients(&self) -> rusqlite::Result<Vec<ClientRecord>> {
        let conn = self.conn.lock().unwrap();
        let mut stmt = conn.prepare("SELECT * FROM clients ORDER BY last_seen DESC")?;
        stmt.query_map([], row_to_client)?.collect()
    }

    /// Sets the build the client is to run (None: the one it runs), and its
    /// update state. Returns false if unknown.
    pub fn set_firmware_policy(
        &self,
        id: ClientId,
        policy: Option<&FirmwarePolicy>,
        state: &FirmwareState,
    ) -> rusqlite::Result<bool> {
        let conn = self.conn.lock().unwrap();
        let n = conn.execute(
            "UPDATE clients SET firmware_policy = ?2, firmware_state = ?3 WHERE id = ?1",
            params![id.to_string(), policy.map(json), json(state)],
        )?;
        Ok(n > 0)
    }

    /// Records the client's update state. Returns false if unknown.
    pub fn set_firmware_state(
        &self,
        id: ClientId,
        state: &FirmwareState,
    ) -> rusqlite::Result<bool> {
        let conn = self.conn.lock().unwrap();
        let n = conn.execute(
            "UPDATE clients SET firmware_state = ?2 WHERE id = ?1",
            params![id.to_string(), json(state)],
        )?;
        Ok(n > 0)
    }

    /// Keeps a firmware build (its image is stored apart, by `md5`).
    pub fn insert_firmware_build(
        &self,
        b: &BuildInfo,
        size: u64,
        md5: &str,
        now: Timestamp,
    ) -> rusqlite::Result<FirmwareBuild> {
        let conn = self.conn.lock().unwrap();
        conn.execute(
            "INSERT INTO firmware_builds (hardware_id, tag, \"commit\", dirty, describe, built_at,
                                          size, md5, uploaded_at)
             VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)",
            params![
                b.hardware_id,
                b.tag,
                b.commit,
                b.dirty,
                b.describe,
                b.built_at,
                size as i64,
                md5,
                now
            ],
        )?;
        conn.query_row(
            "SELECT * FROM firmware_builds WHERE id = ?1",
            [conn.last_insert_rowid()],
            row_to_firmware,
        )
    }

    pub fn get_firmware_build(&self, id: i64) -> rusqlite::Result<Option<FirmwareBuild>> {
        let conn = self.conn.lock().unwrap();
        conn.query_row(
            "SELECT * FROM firmware_builds WHERE id = ?1",
            [id],
            row_to_firmware,
        )
        .optional()
    }

    pub fn firmware_build_by_md5(&self, md5: &str) -> rusqlite::Result<Option<FirmwareBuild>> {
        let conn = self.conn.lock().unwrap();
        conn.query_row(
            "SELECT * FROM firmware_builds WHERE md5 = ?1",
            [md5],
            row_to_firmware,
        )
        .optional()
    }

    /// Every firmware build, newest build time first.
    pub fn list_firmware_builds(&self) -> rusqlite::Result<Vec<FirmwareBuild>> {
        let conn = self.conn.lock().unwrap();
        let mut stmt =
            conn.prepare_cached("SELECT * FROM firmware_builds ORDER BY built_at DESC, id DESC")?;
        stmt.query_map([], row_to_firmware)?.collect()
    }

    pub fn delete_firmware_build(&self, id: i64) -> rusqlite::Result<bool> {
        let conn = self.conn.lock().unwrap();
        Ok(conn.execute("DELETE FROM firmware_builds WHERE id = ?1", [id])? > 0)
    }

    /// Records the client's latest start. Returns false if unknown.
    pub fn set_last_reboot(&self, id: ClientId, b: &RebootInfo) -> rusqlite::Result<bool> {
        let conn = self.conn.lock().unwrap();
        let n = conn.execute(
            "UPDATE clients SET last_reboot = ?2 WHERE id = ?1",
            params![id.to_string(), json(b)],
        )?;
        Ok(n > 0)
    }

    /// Queues a command for the client, superseding any pending command of
    /// the same kind, and returns it. Its id is at least `now` in ms and
    /// above every id given before, so it keeps increasing even across a
    /// database reset (clients run only ids above the highest they have run).
    pub fn queue_command(
        &self,
        client: ClientId,
        command: &str,
        params: &serde_json::Value,
        now: Timestamp,
    ) -> rusqlite::Result<CommandRecord> {
        let mut conn = self.conn.lock().unwrap();
        let tx = conn.transaction()?;
        let max: Option<i64> =
            tx.query_row("SELECT max(id) FROM client_commands", [], |r| r.get(0))?;
        let id = (now / 1_000_000).max(max.map_or(0, |m| m + 1));
        tx.execute(
            "UPDATE client_commands SET result = 'superseded'
             WHERE client_id = ?1 AND command = ?2 AND result IS NULL",
            params![client.to_string(), command],
        )?;
        tx.execute(
            "INSERT INTO client_commands (id, client_id, command, params, created_at)
             VALUES (?1, ?2, ?3, ?4, ?5)",
            params![id, client.to_string(), command, params.to_string(), now],
        )?;
        tx.execute(
            "DELETE FROM client_commands WHERE result IS NOT NULL AND created_at < ?1",
            [now - COMMAND_RETENTION_NS],
        )?;
        let rec = tx.query_row(
            "SELECT * FROM client_commands WHERE id = ?1",
            [id],
            row_to_command,
        )?;
        tx.commit()?;
        Ok(rec)
    }

    /// The client's commands not yet answered, oldest first.
    pub fn pending_commands(&self, client: ClientId) -> rusqlite::Result<Vec<CommandRecord>> {
        let conn = self.conn.lock().unwrap();
        let mut stmt = conn.prepare_cached(
            "SELECT * FROM client_commands WHERE client_id = ?1 AND result IS NULL ORDER BY id",
        )?;
        stmt.query_map([client.to_string()], row_to_command)?
            .collect()
    }

    /// Notes that a heartbeat reply carries these commands.
    pub fn mark_delivered(&self, ids: &[i64], now: Timestamp) -> rusqlite::Result<()> {
        let conn = self.conn.lock().unwrap();
        let mut stmt =
            conn.prepare_cached("UPDATE client_commands SET delivered_at = ?2 WHERE id = ?1")?;
        for id in ids {
            stmt.execute(params![id, now])?;
        }
        Ok(())
    }

    /// Records the client's answer to one of its pending commands, and
    /// returns that command; None (and nothing changes) if it is not one,
    /// e.g. an answer repeated after the first was recorded.
    pub fn ack_command(
        &self,
        client: ClientId,
        id: i64,
        result: CommandResult,
        now: Timestamp,
    ) -> rusqlite::Result<Option<CommandRecord>> {
        let conn = self.conn.lock().unwrap();
        let result = serde_json::to_value(result)
            .ok()
            .and_then(|v| v.as_str().map(str::to_owned))
            .unwrap_or_else(|| "unknown".into());
        let n = conn.execute(
            "UPDATE client_commands SET acked_at = ?3, result = ?4
             WHERE id = ?1 AND client_id = ?2 AND result IS NULL",
            params![id, client.to_string(), now, result],
        )?;
        if n == 0 {
            return Ok(None);
        }
        conn.query_row(
            "SELECT * FROM client_commands WHERE id = ?1",
            [id],
            row_to_command,
        )
        .optional()
    }

    /// The client's latest command of a kind, answered or not.
    pub fn latest_command(
        &self,
        client: ClientId,
        command: &str,
    ) -> rusqlite::Result<Option<CommandRecord>> {
        let conn = self.conn.lock().unwrap();
        conn.query_row(
            "SELECT * FROM client_commands WHERE client_id = ?1 AND command = ?2
             ORDER BY id DESC LIMIT 1",
            params![client.to_string(), command],
            row_to_command,
        )
        .optional()
    }

    pub fn delete_client(&self, id: ClientId) -> rusqlite::Result<bool> {
        let conn = self.conn.lock().unwrap();
        conn.execute(
            "DELETE FROM battery_history WHERE client_id = ?1",
            [id.to_string()],
        )?;
        conn.execute(
            "DELETE FROM client_commands WHERE client_id = ?1",
            [id.to_string()],
        )?;
        Ok(conn.execute("DELETE FROM clients WHERE id = ?1", [id.to_string()])? > 0)
    }

    pub fn insert_event(
        &self,
        ts: Timestamp,
        client_id: Option<ClientId>,
        kind: &str,
        detail: &serde_json::Value,
        retention: u32,
    ) -> rusqlite::Result<i64> {
        let conn = self.conn.lock().unwrap();
        conn.execute(
            "INSERT INTO events (ts, client_id, kind, detail) VALUES (?1, ?2, ?3, ?4)",
            params![
                ts,
                client_id.map(|c| c.to_string()),
                kind,
                detail.to_string()
            ],
        )?;
        let id = conn.last_insert_rowid();
        if id % 100 == 0 {
            conn.execute("DELETE FROM events WHERE id <= ?1", [id - retention as i64])?;
        }
        Ok(id)
    }

    /// Newest-first events, optionally only those newer than `after_id`.
    /// Events, newest first. `after_id` and `before_id` bound the ids (for
    /// following new events, and for paging back); `kinds` keeps only those
    /// kinds and `exclude_kinds` leaves kinds out (empty: no restriction).
    pub fn list_events(&self, f: &EventFilter, limit: u32) -> rusqlite::Result<Vec<Event>> {
        let conn = self.conn.lock().unwrap();
        let mut stmt = conn.prepare_cached(
            "SELECT id, ts, client_id, kind, detail FROM events
             WHERE (?1 IS NULL OR id > ?1) AND (?2 IS NULL OR client_id = ?2)
               AND (?3 IS NULL OR kind IN (SELECT value FROM json_each(?3)))
               AND (?4 IS NULL OR kind NOT IN (SELECT value FROM json_each(?4)))
               AND (?5 IS NULL OR id < ?5)
             ORDER BY id DESC LIMIT ?6",
        )?;
        let list = |kinds: &[String]| {
            (!kinds.is_empty()).then(|| serde_json::to_string(kinds).unwrap_or_default())
        };
        stmt.query_map(
            params![
                f.after_id,
                f.client_id.map(|c| c.to_string()),
                list(&f.kinds),
                list(&f.exclude_kinds),
                f.before_id,
                limit
            ],
            |r| {
                let client: Option<String> = r.get(2)?;
                let detail: String = r.get(4)?;
                Ok(Event {
                    id: r.get(0)?,
                    ts: r.get(1)?,
                    client_id: client.and_then(|c| c.parse().ok()),
                    kind: r.get(3)?,
                    detail: serde_json::from_str(&detail).unwrap_or_default(),
                })
            },
        )?
        .collect()
    }

    pub fn insert_detection(&self, d: &Detection) -> rusqlite::Result<()> {
        let conn = self.conn.lock().unwrap();
        conn.execute(
            "INSERT INTO detections (id, time, body) VALUES (?1, ?2, ?3)",
            params![d.id.to_string(), d.time, json(d)],
        )?;
        Ok(())
    }

    /// Every species with at least one detection, by common name.
    pub fn species_seen(&self) -> rusqlite::Result<Vec<SpeciesSeen>> {
        let conn = self.conn.lock().unwrap();
        let mut stmt = conn.prepare_cached(
            "SELECT json_extract(body, '$.species.scientific') AS sci,
                    max(json_extract(body, '$.species.common')), count(*), max(time)
             FROM detections GROUP BY sci ORDER BY 2 COLLATE NOCASE",
        )?;
        stmt.query_map([], |r| {
            Ok(SpeciesSeen {
                scientific: r.get(0)?,
                common: r.get(1)?,
                count: r.get(2)?,
                last_seen: r.get(3)?,
            })
        })?
        .collect()
    }

    pub fn get_setting(&self, key: &str) -> rusqlite::Result<Option<String>> {
        let conn = self.conn.lock().unwrap();
        conn.query_row("SELECT value FROM settings WHERE key = ?1", [key], |r| {
            r.get(0)
        })
        .optional()
    }

    pub fn set_setting(&self, key: &str, value: &str) -> rusqlite::Result<()> {
        let conn = self.conn.lock().unwrap();
        conn.execute(
            "INSERT INTO settings (key, value) VALUES (?1, ?2) ON CONFLICT(key) DO UPDATE SET value = ?2",
            [key, value],
        )?;
        Ok(())
    }

    pub fn get_detection(&self, id: Uuid) -> rusqlite::Result<Option<Detection>> {
        let conn = self.conn.lock().unwrap();
        let body: Option<String> = conn
            .query_row(
                "SELECT body FROM detections WHERE id = ?1",
                [id.to_string()],
                |r| r.get(0),
            )
            .optional()?;
        Ok(body.and_then(|b| serde_json::from_str(&b).ok()))
    }

    pub fn insert_clip(
        &self,
        detection: Uuid,
        client: ClientId,
        clip: &Clip,
    ) -> rusqlite::Result<()> {
        let conn = self.conn.lock().unwrap();
        conn.execute(
            "INSERT OR REPLACE INTO detection_clips (detection_id, client_id, start, audio) VALUES (?1, ?2, ?3, ?4)",
            params![detection.to_string(), client.to_string(), clip.start, clip.audio],
        )?;
        Ok(())
    }

    pub fn get_clip(&self, detection: Uuid, client: ClientId) -> rusqlite::Result<Option<Clip>> {
        let conn = self.conn.lock().unwrap();
        conn.query_row(
            "SELECT start, audio FROM detection_clips WHERE detection_id = ?1 AND client_id = ?2",
            [detection.to_string(), client.to_string()],
            |r| {
                Ok(Clip {
                    start: r.get(0)?,
                    audio: r.get(1)?,
                })
            },
        )
        .optional()
    }

    /// Clients that have a stored clip for `detection`.
    pub fn clip_clients(&self, detection: Uuid) -> rusqlite::Result<Vec<ClientId>> {
        let conn = self.conn.lock().unwrap();
        let mut stmt =
            conn.prepare_cached("SELECT client_id FROM detection_clips WHERE detection_id = ?1")?;
        let rows = stmt.query_map([detection.to_string()], |r| r.get::<_, String>(0))?;
        Ok(rows.filter_map(|r| r.ok()?.parse().ok()).collect())
    }

    /// Start of each stored clip of `detection`, by client.
    pub fn clip_starts(&self, detection: Uuid) -> rusqlite::Result<Vec<(ClientId, Timestamp)>> {
        let conn = self.conn.lock().unwrap();
        let mut stmt = conn.prepare_cached(
            "SELECT client_id, start FROM detection_clips WHERE detection_id = ?1",
        )?;
        let rows = stmt.query_map([detection.to_string()], |r| {
            Ok((r.get::<_, String>(0)?, r.get::<_, Timestamp>(1)?))
        })?;
        Ok(rows
            .filter_map(|r| {
                let (c, start) = r.ok()?;
                Some((c.parse().ok()?, start))
            })
            .collect())
    }

    /// `client`'s clips (of any detection) starting in `from..to`, oldest first.
    pub fn clips_starting_between(
        &self,
        client: ClientId,
        from: Timestamp,
        to: Timestamp,
    ) -> rusqlite::Result<Vec<Clip>> {
        let conn = self.conn.lock().unwrap();
        let mut stmt = conn.prepare_cached(
            "SELECT start, audio FROM detection_clips
             WHERE client_id = ?1 AND start >= ?2 AND start < ?3 ORDER BY start",
        )?;
        stmt.query_map(params![client.to_string(), from, to], |r| {
            Ok(Clip {
                start: r.get(0)?,
                audio: r.get(1)?,
            })
        })?
        .collect()
    }

    /// Removes detections (and their clips) below `min_confidence`, re-encodes
    /// WAV clips as FLAC, and compacts the file if anything changed.
    pub fn maintain(&self, min_confidence: f32) -> anyhow::Result<Maintenance> {
        let mut done = Maintenance::default();
        {
            let mut conn = self.conn.lock().unwrap();
            let tx = conn.transaction()?;
            tx.execute(
                "DELETE FROM detection_clips WHERE detection_id IN
                   (SELECT id FROM detections WHERE json_extract(body, '$.confidence') < ?1)",
                [min_confidence],
            )?;
            done.detections_removed = tx.execute(
                "DELETE FROM detections WHERE json_extract(body, '$.confidence') < ?1",
                [min_confidence],
            )?;
            tx.commit()?;
        }

        // In batches, converting outside the lock so the server keeps running.
        let mut after = 0i64;
        loop {
            let batch: Vec<(i64, Vec<u8>)> = {
                let conn = self.conn.lock().unwrap();
                let mut stmt = conn.prepare_cached(
                    "SELECT rowid, audio FROM detection_clips
                     WHERE rowid > ?1 AND substr(audio, 1, 4) = CAST('RIFF' AS BLOB)
                     ORDER BY rowid LIMIT 32",
                )?;
                stmt.query_map([after], |r| Ok((r.get(0)?, r.get(1)?)))?
                    .collect::<rusqlite::Result<_>>()?
            };
            let Some(&(last, _)) = batch.last() else {
                break;
            };
            after = last;
            let converted: Vec<(i64, Vec<u8>)> = batch
                .into_iter()
                .filter_map(
                    |(rowid, wav)| match crate::clip_audio::flac_from_wav(&wav) {
                        Ok(flac) => Some((rowid, flac)),
                        Err(e) => {
                            tracing::warn!(rowid, "keeping clip as WAV: {e:#}");
                            None
                        }
                    },
                )
                .collect();
            let conn = self.conn.lock().unwrap();
            let mut stmt =
                conn.prepare_cached("UPDATE detection_clips SET audio = ?2 WHERE rowid = ?1")?;
            for (rowid, flac) in converted {
                stmt.execute(params![rowid, flac])?;
                done.clips_converted += 1;
            }
        }

        if done != Maintenance::default() {
            let conn = self.conn.lock().unwrap();
            conn.execute_batch("VACUUM; PRAGMA wal_checkpoint(TRUNCATE);")?;
        }
        Ok(done)
    }

    /// Newest-first detections matching `f`.
    pub fn list_detections(
        &self,
        f: &DetectionFilter,
        limit: u32,
    ) -> rusqlite::Result<Vec<Detection>> {
        let conn = self.conn.lock().unwrap();
        let mut stmt = conn.prepare_cached(
            "SELECT body FROM detections
             WHERE (?1 IS NULL OR time < ?1)
               AND (?2 IS NULL OR time >= ?2)
               AND (?3 IS NULL OR time < ?3)
               AND (?4 IS NULL OR lower(json_extract(body, '$.species.scientific')) = lower(?4))
               AND (?5 IS NULL
                    OR instr(lower(json_extract(body, '$.species.common')), lower(?5)) > 0
                    OR instr(lower(json_extract(body, '$.species.scientific')), lower(?5)) > 0)
               AND (?6 IS NULL OR EXISTS (
                    SELECT 1 FROM json_each(body, '$.calls') WHERE json_extract(value, '$.client_id') = ?6))
               AND (?7 IS NULL OR coalesce(json_extract(body, '$.unexpected'), 0) = ?7)
               AND (?8 IS NULL OR coalesce(json_type(body, '$.location') = 'object', 0) = ?8)
               AND (?9 IS NULL OR json_extract(body, '$.confidence') >= ?9)
               AND (?11 IS NULL OR lower(json_extract(body, '$.species.scientific'))
                    NOT IN (SELECT lower(value) FROM json_each(?11)))
             ORDER BY time DESC LIMIT ?10",
        )?;
        let nonempty = |s: &Option<String>| {
            s.as_deref()
                .map(str::trim)
                .filter(|s| !s.is_empty())
                .map(str::to_string)
        };
        let exclude = (!f.exclude_species.is_empty())
            .then(|| serde_json::to_string(&f.exclude_species).unwrap_or_default());
        let rows = stmt.query_map(
            params![
                f.before,
                f.from,
                f.to,
                nonempty(&f.species),
                nonempty(&f.q),
                f.client.map(|c| c.to_string()),
                f.unexpected,
                f.located,
                f.min_confidence,
                limit,
                exclude
            ],
            |r| r.get::<_, String>(0),
        )?;
        let mut out = Vec::new();
        for body in rows {
            match serde_json::from_str(&body?) {
                Ok(d) => out.push(d),
                Err(e) => tracing::error!("skipping undecodable detection: {e}"),
            }
        }
        Ok(out)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use bsp_core::identifier::{Call, Species};
    use bsp_proto::OperationMode;

    fn register_request(id: ClientId) -> RegisterRequest {
        RegisterRequest {
            client_id: id,
            hostname: "host".into(),
            version: "0".into(),
            api_version: Some("1.0.0".into()),
            capabilities: Capabilities {
                sample_rate: 48000,
                channels: 1,
                sample_format: "F32".into(),
                clock_source: "test".into(),
                device_name: "mic".into(),
            },
            name: None,
            build: None,
            ota: false,
        }
    }

    #[test]
    fn events_filter_by_several_kinds_and_page_back() {
        let db = Db::in_memory().unwrap();
        let a = Uuid::from_u128(1);
        for (i, kind) in [
            "register",
            "heartbeat",
            "audio",
            "heartbeat",
            "client_log",
            "inactive",
        ]
        .iter()
        .enumerate()
        {
            let who = (i % 2 == 0).then_some(a);
            db.insert_event(i as i64, who, kind, &serde_json::json!({}), 1000)
                .unwrap();
        }
        let kinds = |f: EventFilter| -> Vec<String> {
            db.list_events(&f, 100)
                .unwrap()
                .into_iter()
                .map(|e| e.kind)
                .collect()
        };
        let list = |k: &[&str]| k.iter().map(|s| s.to_string()).collect::<Vec<_>>();
        assert_eq!(kinds(EventFilter::default()).len(), 6);
        assert_eq!(
            kinds(EventFilter {
                exclude_kinds: list(&["heartbeat", "audio", "client_log"]),
                ..Default::default()
            }),
            ["inactive", "register"]
        );
        assert_eq!(
            kinds(EventFilter {
                kinds: list(&["audio", "client_log"]),
                ..Default::default()
            }),
            ["client_log", "audio"]
        );
        assert_eq!(
            kinds(EventFilter {
                client_id: Some(a),
                ..Default::default()
            }),
            ["client_log", "audio", "register"]
        );
        // Paging back from the oldest event shown, and following new ones.
        let all = db.list_events(&EventFilter::default(), 2).unwrap();
        let older = db
            .list_events(
                &EventFilter {
                    before_id: Some(all[1].id),
                    ..Default::default()
                },
                2,
            )
            .unwrap();
        assert_eq!(
            older.iter().map(|e| e.kind.as_str()).collect::<Vec<_>>(),
            ["heartbeat", "audio"]
        );
        assert_eq!(
            kinds(EventFilter {
                after_id: Some(all[1].id),
                ..Default::default()
            }),
            ["inactive"]
        );
    }

    #[test]
    fn suggested_name_follows_registrations_and_leaves_name_alone() {
        let db = Db::in_memory().unwrap();
        let id = Uuid::from_u128(1);
        let req = register_request(id);
        assert!(db.register_client(&req, "North fence", "addr", 1).unwrap());
        let c = db.get_client(id).unwrap().unwrap();
        assert_eq!(
            (c.name.as_str(), c.suggested_name.as_str()),
            ("", "North fence")
        );

        db.configure_client(id, "Oak tree", None).unwrap();
        assert!(!db.register_client(&req, "Gate", "addr", 2).unwrap());
        let c = db.get_client(id).unwrap().unwrap();
        assert_eq!(
            (c.name.as_str(), c.suggested_name.as_str()),
            ("Oak tree", "Gate")
        );
    }

    #[test]
    fn keeps_the_latest_registrations_versions() {
        let db = Db::in_memory().unwrap();
        let id = Uuid::from_u128(1);
        let mut req = register_request(id);
        req.version = "bps-mcu 1a2b3c4".into();
        db.register_client(&req, "", "addr", 1).unwrap();
        let c = db.get_client(id).unwrap().unwrap();
        assert_eq!(c.version, "bps-mcu 1a2b3c4");
        assert_eq!(c.api_version.as_deref(), Some("1.0.0"));
        // Old firmware, flashed over the new, leaves the API version out.
        req.api_version = None;
        db.register_client(&req, "", "addr", 2).unwrap();
        assert_eq!(db.get_client(id).unwrap().unwrap().api_version, None);
    }

    #[test]
    fn keeps_the_latest_heartbeats_health() {
        let db = Db::in_memory().unwrap();
        let id = Uuid::from_u128(1);
        db.register_client(&register_request(id), "", "addr", 1)
            .unwrap();
        assert!(db.get_client(id).unwrap().unwrap().health.is_none());
        let clock = ClockStatus {
            offset_ns: 0,
            rtt_ns: 1,
            measured_at: 2,
            error_ns: Some(3),
            detail: None,
            beacon: None,
        };
        let health = ClientHealth {
            at: 10,
            uptime_s: 60,
            chunks_sent: 4,
            chunks_gated: 5,
            battery: Some(BatteryStatus {
                percent: 87.5,
                voltage_v: 4.0,
                rate_pct_per_h: Some(-1.04),
                charging: Some(false),
            }),
            storage: Some(StorageStatus {
                present: true,
                total_bytes: Some(32 << 30),
                free_bytes: Some(1 << 30),
                recordings: 2,
                recorded_bytes: 400_000,
                errors: 0,
                dropped_bytes: 0,
                paused: false,
            }),
            devices: vec![DeviceStatus {
                kind: "max17048".into(),
                bus: "i2c".into(),
                address: 0x36,
                present: true,
            }],
            mode: Some(ModeStatus {
                mode: OperationMode::Batch,
                interval_s: 300,
                backlog: 3,
                backlog_bytes: 120_000,
                delivered_through: Some(9),
                next_contact_s: Some(290),
                radio_on_pct: Some(4.5),
                gap: None,
                note: None,
            }),
            trigger: Some(TriggerConfig::default()),
            validation: Some(ValidationConfig::None),
            validation_note: None,
            chunks_rejected: 0,
            logging: None,
        };
        assert!(
            db.record_heartbeat(id, "addr2", Some(&clock), &health)
                .unwrap()
        );
        let c = db.get_client(id).unwrap().unwrap();
        assert_eq!((c.last_seen, c.last_addr.as_str()), (10, "addr2"));
        assert_eq!(c.health.as_ref(), Some(&health));
        assert_eq!(c.clock.unwrap().rtt_ns, 1);
        // A heartbeat without a clock measurement keeps the last one.
        let later = ClientHealth {
            at: 20,
            battery: None,
            ..health.clone()
        };
        db.record_heartbeat(id, "addr2", None, &later).unwrap();
        let c = db.get_client(id).unwrap().unwrap();
        assert_eq!(c.health, Some(later));
        assert!(c.clock.is_some());
        assert!(
            !db.record_heartbeat(Uuid::from_u128(2), "a", None, &health)
                .unwrap()
        );
    }

    #[test]
    fn keeps_the_mode_set_for_a_client() {
        let db = Db::in_memory().unwrap();
        let id = Uuid::from_u128(1);
        db.register_client(&register_request(id), "", "addr", 1)
            .unwrap();
        assert_eq!(
            db.get_client(id).unwrap().unwrap().mode,
            ModeSetting::default()
        );
        let batch = ModeSetting {
            mode: OperationMode::Batch,
            interval_s: 120,
        };
        assert!(db.set_client_mode(id, &batch).unwrap());
        assert_eq!(db.get_client(id).unwrap().unwrap().mode, batch);
        // Registering again keeps it.
        db.register_client(&register_request(id), "", "addr", 2)
            .unwrap();
        assert_eq!(db.get_client(id).unwrap().unwrap().mode, batch);
        assert!(!db.set_client_mode(Uuid::from_u128(2), &batch).unwrap());
    }

    #[test]
    fn upgrades_clients_table_without_suggested_name() {
        let path = std::env::temp_dir().join(format!("bsp-upgrade-{}.db", Uuid::new_v4()));
        {
            let conn = Connection::open(&path).unwrap();
            conn.execute_batch(
                "CREATE TABLE clients (id TEXT PRIMARY KEY, name TEXT NOT NULL DEFAULT '',
                   x REAL, y REAL, z REAL, hostname TEXT NOT NULL DEFAULT '',
                   version TEXT NOT NULL DEFAULT '', capabilities TEXT,
                   first_seen INTEGER NOT NULL, last_seen INTEGER NOT NULL,
                   last_addr TEXT NOT NULL DEFAULT '', clock TEXT);
                 INSERT INTO clients (id, name, first_seen, last_seen)
                   VALUES ('00000000-0000-0000-0000-000000000001', 'Old', 1, 1);",
            )
            .unwrap();
        }
        let db = Db::open(&path).unwrap();
        let c = db.get_client(Uuid::from_u128(1)).unwrap().unwrap();
        assert_eq!((c.name.as_str(), c.suggested_name.as_str()), ("Old", ""));
        assert!(c.health.is_none());
        assert_eq!(c.api_version, None);
        drop(db);
        // Opening again finds the column already there.
        Db::open(&path).unwrap();
        for ext in ["", "-wal", "-shm"] {
            let _ = std::fs::remove_file(format!("{}{ext}", path.display()));
        }
    }

    fn detection(
        time: i64,
        sci: &str,
        common: &str,
        clients: &[u128],
        confidence: f32,
        unexpected: bool,
    ) -> Detection {
        let species = Species {
            scientific: sci.into(),
            common: common.into(),
        };
        Detection {
            id: Uuid::new_v4(),
            time,
            species: species.clone(),
            confidence,
            calls: clients
                .iter()
                .map(|&c| Call {
                    client_id: Uuid::from_u128(c),
                    species: species.clone(),
                    confidence,
                    start: time,
                    end: time + 1,
                    unexpected,
                })
                .collect(),
            unexpected,
            location: None,
            created_at: time,
        }
    }

    #[test]
    fn maintenance_drops_low_confidence_and_converts_wav() {
        let path = std::env::temp_dir().join(format!("bsp-maintain-{}.db", Uuid::new_v4()));
        let pcm: Vec<f32> = (0..20_000).map(|i| (i as f32 / 9.0).sin() * 0.3).collect();
        let wav = bsp_core::audio::encode_wav(48_000, &pcm);
        let (low, high) = (
            detection(100, "a", "A", &[1], 0.55, false),
            detection(200, "b", "B", &[1], 0.70, false),
        );
        {
            // A database from before FLAC, with the clip column still named `wav`.
            let conn = Connection::open(&path).unwrap();
            conn.execute_batch(
                "CREATE TABLE detection_clips (detection_id TEXT NOT NULL, client_id TEXT NOT NULL,
                   start INTEGER NOT NULL, wav BLOB NOT NULL, PRIMARY KEY (detection_id, client_id));",
            )
            .unwrap();
        }
        let db = Db::open(&path).unwrap();
        for d in [&low, &high] {
            db.insert_detection(d).unwrap();
            let clip = Clip {
                start: d.time,
                audio: wav.clone(),
            };
            db.insert_clip(d.id, Uuid::from_u128(1), &clip).unwrap();
        }

        let done = db.maintain(0.7).unwrap();
        assert_eq!(
            done,
            Maintenance {
                detections_removed: 1,
                clips_converted: 1
            }
        );
        assert!(db.get_detection(low.id).unwrap().is_none());
        assert!(db.clip_clients(low.id).unwrap().is_empty());
        let clip = db.get_clip(high.id, Uuid::from_u128(1)).unwrap().unwrap();
        assert!(crate::clip_audio::is_flac(&clip.audio));
        assert_eq!(
            crate::clip_audio::decode(&clip.audio).unwrap(),
            crate::clip_audio::decode(&wav).unwrap()
        );
        assert_eq!(db.maintain(0.7).unwrap(), Maintenance::default());
        drop(db);
        for ext in ["", "-wal", "-shm"] {
            let _ = std::fs::remove_file(format!("{}{ext}", path.display()));
        }
    }

    #[test]
    fn filters_detections() {
        let db = Db::in_memory().unwrap();
        for d in [
            detection(
                100,
                "Corvus brachyrhynchos",
                "American Crow",
                &[1, 2],
                0.95,
                false,
            ),
            detection(200, "Corvus corax", "Common Raven", &[2], 0.92, true),
            detection(
                300,
                "Turdus migratorius",
                "American Robin",
                &[3],
                0.81,
                false,
            ),
            detection(
                400,
                "Corvus brachyrhynchos",
                "American Crow",
                &[3],
                0.85,
                false,
            ),
        ] {
            db.insert_detection(&d).unwrap();
        }
        let times = |f: DetectionFilter| -> Vec<i64> {
            db.list_detections(&f, 100)
                .unwrap()
                .iter()
                .map(|d| d.time)
                .collect()
        };
        assert_eq!(times(DetectionFilter::default()), vec![400, 300, 200, 100]);
        // Muted species are left out, whatever the case of the name.
        assert_eq!(
            times(DetectionFilter {
                exclude_species: vec!["CORVUS brachyrhynchos".into(), "Nobody nowhere".into()],
                ..Default::default()
            }),
            vec![300, 200]
        );
        assert_eq!(
            times(DetectionFilter {
                species: Some("corvus BRACHYRHYNCHOS".into()),
                ..Default::default()
            }),
            vec![400, 100]
        );
        assert_eq!(
            times(DetectionFilter {
                q: Some("corvus".into()),
                ..Default::default()
            }),
            vec![400, 200, 100]
        );
        assert_eq!(
            times(DetectionFilter {
                q: Some("american".into()),
                ..Default::default()
            }),
            vec![400, 300, 100]
        );
        assert_eq!(
            times(DetectionFilter {
                q: Some("  ".into()),
                ..Default::default()
            })
            .len(),
            4
        );
        assert_eq!(
            times(DetectionFilter {
                client: Some(Uuid::from_u128(2)),
                ..Default::default()
            }),
            vec![200, 100]
        );
        assert_eq!(
            times(DetectionFilter {
                unexpected: Some(true),
                ..Default::default()
            }),
            vec![200]
        );
        assert_eq!(
            times(DetectionFilter {
                located: Some(true),
                ..Default::default()
            }),
            Vec::<i64>::new()
        );
        assert_eq!(
            times(DetectionFilter {
                located: Some(false),
                ..Default::default()
            })
            .len(),
            4
        );
        assert_eq!(
            times(DetectionFilter {
                min_confidence: Some(0.9),
                ..Default::default()
            }),
            vec![200, 100]
        );
        assert_eq!(
            times(DetectionFilter {
                from: Some(200),
                to: Some(400),
                ..Default::default()
            }),
            vec![300, 200]
        );
        assert_eq!(
            times(DetectionFilter {
                before: Some(300),
                ..Default::default()
            }),
            vec![200, 100]
        );

        let seen = db.species_seen().unwrap();
        let summary: Vec<_> = seen
            .iter()
            .map(|s| (s.common.as_str(), s.count, s.last_seen))
            .collect();
        assert_eq!(
            summary,
            vec![
                ("American Crow", 2, 400),
                ("American Robin", 1, 300),
                ("Common Raven", 1, 200)
            ]
        );
    }

    #[test]
    fn counts_a_clients_detections_by_hour_over_the_last_day() {
        let db = Db::in_memory().unwrap();
        let now = 100 * HOUR_NS + 123;
        let ago = |h: f64| now - (h * HOUR_NS as f64) as i64;
        for d in [
            detection(ago(30.0), "a", "A", &[1], 0.9, false), // over a day ago
            detection(now - 24 * HOUR_NS, "a", "A", &[1], 0.9, false), // exactly a day: out
            detection(ago(23.5), "a", "A", &[1, 2], 0.9, false), // oldest bucket
            detection(ago(2.5), "a", "A", &[1], 0.9, false),
            detection(ago(2.2), "a", "A", &[1], 0.9, false),
            detection(ago(2.1), "a", "A", &[2], 0.9, false), // another client's
            detection(ago(0.1), "a", "A", &[1], 0.9, false), // newest bucket
            detection(now, "a", "A", &[1], 0.9, false),
            detection(now + 5, "a", "A", &[1], 0.9, false), // after now: out
        ] {
            db.insert_detection(&d).unwrap();
        }
        let counts = db.detections_hourly(Uuid::from_u128(1), now).unwrap();
        assert_eq!(counts.len(), 24);
        let mut want = [0u32; 24];
        want[0] = 1; // 23.5 h ago
        want[21] = 2; // 2.5 and 2.2 h ago
        want[23] = 2; // 0.1 h ago, and now
        assert_eq!(counts, want);
        // Hours with nothing are there, as zero.
        assert_eq!(counts.iter().filter(|&&n| n == 0).count(), 21);
        let other = db.detections_hourly(Uuid::from_u128(2), now).unwrap();
        assert_eq!(other.iter().sum::<u32>(), 2);
        assert_eq!((other[0], other[21]), (1, 1));
        assert_eq!(
            db.detections_hourly(Uuid::from_u128(9), now).unwrap(),
            [0; 24]
        );
    }

    #[test]
    fn keeps_battery_history_per_client() {
        let db = Db::in_memory().unwrap();
        let (a, b) = (Uuid::from_u128(1), Uuid::from_u128(2));
        for id in [a, b] {
            db.register_client(&register_request(id), "", "addr", 1)
                .unwrap();
        }
        let s = 1_000_000_000i64;
        let beat = |id, at: i64, percent: f32, charging: Option<bool>| {
            let health = ClientHealth {
                at,
                uptime_s: 0,
                chunks_sent: 0,
                chunks_gated: 0,
                battery: Some(BatteryStatus {
                    percent,
                    voltage_v: 3.8,
                    rate_pct_per_h: None,
                    charging,
                }),
                storage: None,
                devices: Vec::new(),
                mode: None,
                trigger: None,
                validation: None,
                validation_note: None,
                chunks_rejected: 0,
                logging: None,
            };
            db.record_heartbeat(id, "addr", None, &health).unwrap();
        };
        beat(a, 1000 * s, 80.0, None);
        beat(a, 1010 * s, 79.9, None); // within a minute: not kept
        beat(a, 1020 * s, 79.8, Some(true)); // charging state changed: kept
        beat(a, 1090 * s, 81.0, Some(true));
        beat(b, 1000 * s, 50.0, Some(false));
        let no_battery = ClientHealth {
            at: 1200 * s,
            uptime_s: 0,
            chunks_sent: 0,
            chunks_gated: 0,
            battery: None,
            storage: None,
            devices: Vec::new(),
            mode: None,
            trigger: None,
            validation: None,
            validation_note: None,
            chunks_rejected: 0,
            logging: None,
        };
        db.record_heartbeat(a, "addr", None, &no_battery).unwrap();

        let h = db.battery_history(a, 0).unwrap();
        let at: Vec<i64> = h.iter().map(|p| p.at / s).collect();
        assert_eq!(at, [1000, 1020, 1090]);
        assert_eq!(h[1].charging, Some(true));
        assert_eq!(h[0].charging, None);
        assert_eq!(db.battery_history(a, 1050 * s).unwrap().len(), 1);
        assert_eq!(db.battery_history(b, 0).unwrap().len(), 1);

        // Readings older than the retention go as new ones come in.
        beat(
            a,
            1090 * s + BATTERY_HISTORY_RETENTION_NS - s,
            70.0,
            Some(true),
        );
        let at: Vec<i64> = db
            .battery_history(a, 0)
            .unwrap()
            .iter()
            .map(|p| p.at)
            .collect();
        assert_eq!(at, [1090 * s, 1090 * s + BATTERY_HISTORY_RETENTION_NS - s]);

        db.delete_client(a).unwrap();
        assert!(db.battery_history(a, 0).unwrap().is_empty());
        assert_eq!(db.battery_history(b, 0).unwrap().len(), 1);
    }
}
