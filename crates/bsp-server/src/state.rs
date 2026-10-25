use crate::audio_buffer::AudioBuffers;
use crate::beacon_clock::{BeaconClocks, TsfLine};
use crate::clock_history::ClockHistory;
use crate::config::ServerConfig;
use crate::db::{ClientRecord, Db};
use crate::settings::{LocalSpeciesStatus, Settings};
use bsp_core::identifier::{Call, Identifier, LocalSpeciesFilter, RangeModel};
use bsp_proto::{Bssid, ClientId, ClockStatus, NANOS_PER_SEC, Timestamp, now_ns};
use serde::Serialize;
use std::sync::{Arc, Mutex, RwLock};
use tokio::sync::mpsc;

pub type SharedState = Arc<AppState>;

pub struct AppState {
    pub cfg: ServerConfig,
    pub db: Db,
    /// The species identifier, already wrapped by `species_filter`.
    pub identifier: Arc<dyn Identifier>,
    pub species_filter: Arc<LocalSpeciesFilter>,
    pub geo: Option<Arc<dyn RangeModel>>,
    /// Settings edited from the web UI.
    pub settings: tokio::sync::Mutex<Settings>,
    pub local_species: RwLock<LocalSpeciesStatus>,
    pub audio: AudioBuffers,
    /// Access points' clocks, for clients that follow them.
    pub beacon_clocks: Arc<BeaconClocks>,
    pub clock_history: ClockHistory,
    pub calls_tx: mpsc::UnboundedSender<Vec<Call>>,
    pub positioning: Mutex<Option<PositioningRequest>>,
    pub udp_port: u16,
    pub started_at: Timestamp,
}

/// The client most recently asking to have its position configured.
#[derive(Debug, Clone, Copy, Serialize)]
pub struct PositioningRequest {
    pub client_id: ClientId,
    pub requested_at: Timestamp,
    pub expires_at: Timestamp,
}

impl AppState {
    /// Records a raw event; failures are logged, never propagated.
    pub fn event(&self, client_id: Option<ClientId>, kind: &str, detail: serde_json::Value) {
        tracing::debug!(?client_id, kind, %detail, "event");
        if let Err(e) =
            self.db
                .insert_event(now_ns(), client_id, kind, &detail, self.cfg.event_retention)
        {
            tracing::error!("failed to store event: {e}");
        }
    }

    pub fn is_active(&self, c: &ClientRecord, now: Timestamp) -> bool {
        now - c.last_seen <= self.cfg.active_timeout_s as i64 * NANOS_PER_SEC
    }

    /// The line converting the TSF of the access point the client follows,
    /// if it follows one and the server has a line for it.
    pub fn beacon_line(&self, c: &ClientRecord) -> Option<TsfLine> {
        self.beacon_clocks.line(c.clock?.beacon?.bssid)
    }

    /// Worst-case error of the client's audio timestamps (ns). A client that
    /// follows its access point's clock has its audio converted by the AP's
    /// line, so that line's error counts, plus how closely the client follows
    /// the AP (three times the scatter of its beacons).
    pub fn clock_error_ns(&self, c: &ClientRecord) -> Option<i64> {
        c.clock.map(|k| self.status_error_ns(&k))
    }

    /// [`Self::clock_error_ns`] for one clock report.
    pub fn status_error_ns(&self, k: &ClockStatus) -> i64 {
        match k
            .beacon
            .and_then(|b| Some((b, self.beacon_clocks.line(b.bssid)?)))
        {
            Some((b, line)) => line.error_ns + 3 * b.scatter_ns,
            None => k.error_bound_ns(),
        }
    }

    /// The access point a client follows, if another client follows it too,
    /// and the error of its audio timestamps against theirs (ns). They share
    /// the AP's clock, so only how closely each follows it counts: three
    /// times the scatter of its beacons, however far the AP's line is from
    /// the server clock.
    pub fn ap_error_ns(&self, c: &ClientRecord) -> Option<(Bssid, i64)> {
        let b = c.clock?.beacon?;
        let line = self.beacon_clocks.line(b.bssid)?;
        (line.clients >= 2).then_some((b.bssid, 3 * b.scatter_ns))
    }

    fn clock_fresh(&self, c: &ClientRecord, now: Timestamp) -> bool {
        self.is_active(c, now)
            && c.clock
                .is_some_and(|k| now - k.measured_at <= 60 * NANOS_PER_SEC)
    }

    /// Whether the client's audio timestamps are within the limit of the
    /// server clock, and so usable for positioning with any other such client.
    pub fn is_synced_to_server(&self, c: &ClientRecord, now: Timestamp) -> bool {
        self.clock_fresh(c, now)
            && self
                .clock_error_ns(c)
                .is_some_and(|e| e <= self.cfg.max_clock_offset_us * 1000)
    }

    /// The access point whose other clients this client's audio timestamps
    /// are within the limit of, and so usable for positioning with them.
    pub fn synced_ap(&self, c: &ClientRecord, now: Timestamp) -> Option<Bssid> {
        let (bssid, e) = self.ap_error_ns(c)?;
        (self.clock_fresh(c, now) && e <= self.cfg.max_clock_offset_us * 1000).then_some(bssid)
    }

    /// Whether the client's audio timestamps are trustworthy enough for
    /// positioning, with all other synced clients or with its AP's.
    pub fn is_synced(&self, c: &ClientRecord, now: Timestamp) -> bool {
        self.is_synced_to_server(c, now) || self.synced_ap(c, now).is_some()
    }

    /// The current positioning request, expiring stale ones.
    pub fn positioning_request(&self) -> Option<PositioningRequest> {
        let mut slot = self.positioning.lock().unwrap();
        if slot.is_some_and(|r| r.expires_at <= now_ns()) {
            *slot = None;
        }
        *slot
    }
}
