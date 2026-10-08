use crate::audio_buffer::AudioBuffers;
use crate::clock_history::ClockHistory;
use crate::config::ServerConfig;
use crate::db::{ClientRecord, Db};
use crate::settings::{LocalSpeciesStatus, Settings};
use bsp_core::identifier::{Call, Identifier, LocalSpeciesFilter, RangeModel};
use bsp_proto::{ClientId, NANOS_PER_SEC, Timestamp, now_ns};
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

    /// Whether the client's audio timestamps are trustworthy enough for
    /// positioning: their worst-case error is within the limit.
    pub fn is_synced(&self, c: &ClientRecord, now: Timestamp) -> bool {
        self.is_active(c, now)
            && c.clock.is_some_and(|k| {
                k.error_bound_ns() <= self.cfg.max_clock_offset_us * 1000
                    && now - k.measured_at <= 60 * NANOS_PER_SEC
            })
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
