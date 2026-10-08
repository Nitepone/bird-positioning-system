//! SQLite persistence for clients, detections and raw events.
//!
//! All queries are small; they run directly on the caller's thread behind a mutex.

use bsp_core::detection::Detection;
use bsp_core::locator::Vec3;
use bsp_proto::{Capabilities, ClientId, ClockStatus, Timestamp, Uuid};
use rusqlite::{Connection, OptionalExtension, params};
use serde::Serialize;
use std::path::Path;
use std::sync::Mutex;

pub struct Db {
    conn: Mutex<Connection>,
}

#[derive(Debug, Clone, Serialize)]
pub struct ClientRecord {
    pub id: ClientId,
    pub name: String,
    pub position: Option<Vec3>,
    pub hostname: String,
    pub version: String,
    pub capabilities: Option<Capabilities>,
    pub first_seen: Timestamp,
    pub last_seen: Timestamp,
    pub last_addr: String,
    pub clock: Option<ClockStatus>,
}

/// Audio of one client around a detection.
pub struct Clip {
    /// Timestamp of the first sample.
    pub start: Timestamp,
    pub wav: Vec<u8>,
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
    x REAL, y REAL, z REAL,
    hostname TEXT NOT NULL DEFAULT '',
    version TEXT NOT NULL DEFAULT '',
    capabilities TEXT,
    first_seen INTEGER NOT NULL,
    last_seen INTEGER NOT NULL,
    last_addr TEXT NOT NULL DEFAULT '',
    clock TEXT
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
    wav BLOB NOT NULL,
    PRIMARY KEY (detection_id, client_id)
);
CREATE TABLE IF NOT EXISTS settings (
    key TEXT PRIMARY KEY,
    value TEXT NOT NULL
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

fn row_to_client(r: &rusqlite::Row) -> rusqlite::Result<ClientRecord> {
    let id: String = r.get("id")?;
    let (x, y, z): (Option<f64>, Option<f64>, Option<f64>) =
        (r.get("x")?, r.get("y")?, r.get("z")?);
    let caps: Option<String> = r.get("capabilities")?;
    let clock: Option<String> = r.get("clock")?;
    Ok(ClientRecord {
        id: id.parse().unwrap_or_default(),
        name: r.get("name")?,
        position: match (x, y, z) {
            (Some(x), Some(y), Some(z)) => Some(Vec3 { x, y, z }),
            _ => None,
        },
        hostname: r.get("hostname")?,
        version: r.get("version")?,
        capabilities: caps.and_then(|c| serde_json::from_str(&c).ok()),
        first_seen: r.get("first_seen")?,
        last_seen: r.get("last_seen")?,
        last_addr: r.get("last_addr")?,
        clock: clock.and_then(|c| serde_json::from_str(&c).ok()),
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
        conn.execute_batch(SCHEMA)?;
        Ok(Self {
            conn: Mutex::new(conn),
        })
    }

    /// Records a registration. Returns true if the client is new.
    pub fn register_client(
        &self,
        id: ClientId,
        hostname: &str,
        version: &str,
        caps: &Capabilities,
        addr: &str,
        now: Timestamp,
    ) -> rusqlite::Result<bool> {
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
            "INSERT INTO clients (id, hostname, version, capabilities, first_seen, last_seen, last_addr)
             VALUES (?1, ?2, ?3, ?4, ?5, ?5, ?6)
             ON CONFLICT(id) DO UPDATE SET hostname = ?2, version = ?3, capabilities = ?4,
                                           last_seen = ?5, last_addr = ?6",
            params![id.to_string(), hostname, version, json(caps), now, addr],
        )?;
        Ok(!existed)
    }

    /// Updates liveness. Returns false if the client is unknown.
    pub fn touch_client(
        &self,
        id: ClientId,
        addr: &str,
        clock: Option<&ClockStatus>,
        now: Timestamp,
    ) -> rusqlite::Result<bool> {
        let conn = self.conn.lock().unwrap();
        let n = match clock {
            Some(c) => conn.execute(
                "UPDATE clients SET last_seen = ?2, last_addr = ?3, clock = ?4 WHERE id = ?1",
                params![id.to_string(), now, addr, json(c)],
            )?,
            None => conn.execute(
                "UPDATE clients SET last_seen = ?2, last_addr = ?3 WHERE id = ?1",
                params![id.to_string(), now, addr],
            )?,
        };
        Ok(n > 0)
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

    pub fn delete_client(&self, id: ClientId) -> rusqlite::Result<bool> {
        let conn = self.conn.lock().unwrap();
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
    pub fn list_events(
        &self,
        after_id: Option<i64>,
        client_id: Option<ClientId>,
        kind: Option<&str>,
        exclude_kind: Option<&str>,
        limit: u32,
    ) -> rusqlite::Result<Vec<Event>> {
        let conn = self.conn.lock().unwrap();
        let mut stmt = conn.prepare(
            "SELECT id, ts, client_id, kind, detail FROM events
             WHERE (?1 IS NULL OR id > ?1) AND (?2 IS NULL OR client_id = ?2)
               AND (?3 IS NULL OR kind = ?3) AND (?4 IS NULL OR kind != ?4)
             ORDER BY id DESC LIMIT ?5",
        )?;
        stmt.query_map(
            params![
                after_id,
                client_id.map(|c| c.to_string()),
                kind,
                exclude_kind,
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
            "INSERT OR REPLACE INTO detection_clips (detection_id, client_id, start, wav) VALUES (?1, ?2, ?3, ?4)",
            params![detection.to_string(), client.to_string(), clip.start, clip.wav],
        )?;
        Ok(())
    }

    pub fn get_clip(&self, detection: Uuid, client: ClientId) -> rusqlite::Result<Option<Clip>> {
        let conn = self.conn.lock().unwrap();
        conn.query_row(
            "SELECT start, wav FROM detection_clips WHERE detection_id = ?1 AND client_id = ?2",
            [detection.to_string(), client.to_string()],
            |r| {
                Ok(Clip {
                    start: r.get(0)?,
                    wav: r.get(1)?,
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

    /// Newest-first detections, optionally older than `before`.
    pub fn list_detections(
        &self,
        before: Option<Timestamp>,
        limit: u32,
    ) -> rusqlite::Result<Vec<Detection>> {
        let conn = self.conn.lock().unwrap();
        let mut stmt =
            conn.prepare("SELECT body FROM detections WHERE (?1 IS NULL OR time < ?1) ORDER BY time DESC LIMIT ?2")?;
        let rows = stmt.query_map(params![before, limit], |r| r.get::<_, String>(0))?;
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
