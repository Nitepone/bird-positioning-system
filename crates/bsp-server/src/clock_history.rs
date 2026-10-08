//! Recent clock reports per client (one per heartbeat), for the web UI's
//! clock page. Kept in memory only: it is for watching a client's
//! synchronisation, and restarts with the server.

use bsp_proto::{ClientId, ClockStatus, NANOS_PER_SEC, Timestamp};
use serde::Serialize;
use std::collections::{HashMap, VecDeque};
use std::sync::Mutex;

/// How far back reports are kept.
pub const RETENTION_NS: i64 = 3600 * NANOS_PER_SEC;

#[derive(Debug, Clone, Copy, Serialize)]
pub struct ClockReport {
    /// When the server received the heartbeat.
    pub at: Timestamp,
    /// `None` if the client had no measurement to report.
    pub clock: Option<ClockStatus>,
}

#[derive(Default)]
pub struct ClockHistory {
    clients: Mutex<HashMap<ClientId, VecDeque<ClockReport>>>,
}

impl ClockHistory {
    pub fn push(&self, client: ClientId, report: ClockReport) {
        let mut clients = self.clients.lock().unwrap();
        let q = clients.entry(client).or_default();
        q.push_back(report);
        while q.front().is_some_and(|r| r.at < report.at - RETENTION_NS) {
            q.pop_front();
        }
    }

    /// Reports for `client` received at or after `since`, oldest first.
    pub fn since(&self, client: ClientId, since: Timestamp) -> Vec<ClockReport> {
        self.clients
            .lock()
            .unwrap()
            .get(&client)
            .map(|q| q.iter().filter(|r| r.at >= since).copied().collect())
            .unwrap_or_default()
    }

    pub fn forget(&self, client: ClientId) {
        self.clients.lock().unwrap().remove(&client);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use bsp_proto::Uuid;

    #[test]
    fn keeps_the_last_hour_per_client() {
        let h = ClockHistory::default();
        let (a, b) = (Uuid::new_v4(), Uuid::new_v4());
        for i in 0..=120 {
            let at = i * 60 * NANOS_PER_SEC;
            h.push(a, ClockReport { at, clock: None });
        }
        h.push(b, ClockReport { at: 0, clock: None });
        let kept = h.since(a, 0);
        assert_eq!(kept.len(), 61);
        assert_eq!(kept[0].at, 60 * 60 * NANOS_PER_SEC);
        assert_eq!(h.since(a, 110 * 60 * NANOS_PER_SEC).len(), 11);
        assert_eq!(h.since(b, 0).len(), 1);
        h.forget(a);
        assert!(h.since(a, 0).is_empty());
    }
}
