//! HTTP communication with the server: registration, heartbeats, audio uploads.

use anyhow::{Context, bail};
use bsp_proto::{
    CLIENT_API_PREFIX, Capabilities, ClientId, HEADER_START_NS, Heartbeat, RegisterRequest,
    RegisterResponse, Timestamp,
};
use reqwest::StatusCode;
use std::collections::VecDeque;
use std::sync::Arc;
use std::sync::atomic::{AtomicU32, AtomicU64, Ordering};
use std::time::{Duration, Instant};
use tokio::sync::{Mutex, Notify};

use crate::timesync::SharedClock;

#[derive(Default)]
pub struct Stats {
    pub chunks_sent: AtomicU64,
    pub chunks_gated: AtomicU64,
    pub queue_len: AtomicU32,
    pub dropouts: AtomicU32,
}

pub struct Session {
    pub http: reqwest::Client,
    pub base: String,
    pub id: ClientId,
    pub hostname: String,
    pub name: Option<String>,
    pub caps: Capabilities,
}

#[derive(Debug)]
pub enum SendError {
    /// The server does not know us (e.g. its database was reset): re-register.
    Unknown,
    Other(anyhow::Error),
}

impl std::fmt::Display for SendError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Unknown => write!(f, "server does not know this client"),
            Self::Other(e) => write!(f, "{e:#}"),
        }
    }
}

impl From<reqwest::Error> for SendError {
    fn from(e: reqwest::Error) -> Self {
        Self::Other(e.into())
    }
}

fn check(resp: reqwest::Response) -> Result<reqwest::Response, SendError> {
    match resp.status() {
        StatusCode::NOT_FOUND => Err(SendError::Unknown),
        s if s.is_success() => Ok(resp),
        s => Err(SendError::Other(anyhow::anyhow!("server returned {s}"))),
    }
}

impl Session {
    fn url(&self, path: &str) -> String {
        format!("{}{CLIENT_API_PREFIX}{path}", self.base)
    }

    pub async fn register(&self) -> anyhow::Result<RegisterResponse> {
        let req = RegisterRequest {
            client_id: self.id,
            hostname: self.hostname.clone(),
            version: env!("CARGO_PKG_VERSION").into(),
            capabilities: self.caps.clone(),
            name: self.name.clone(),
        };
        let resp = self
            .http
            .post(self.url("/register"))
            .json(&req)
            .send()
            .await?;
        if !resp.status().is_success() {
            bail!("register failed: {}", resp.status());
        }
        resp.json().await.context("decoding register response")
    }

    /// Registers, retrying with backoff until the server answers.
    pub async fn register_until_ok(&self) -> RegisterResponse {
        let mut delay = Duration::from_secs(1);
        loop {
            match self.register().await {
                Ok(r) => {
                    tracing::info!(id = %self.id, "registered with {}", self.base);
                    return r;
                }
                Err(e) => {
                    tracing::warn!("registration failed: {e:#}; retrying in {delay:?}");
                    tokio::time::sleep(delay).await;
                    delay = (delay * 2).min(Duration::from_secs(30));
                }
            }
        }
    }

    pub async fn heartbeat(&self, hb: &Heartbeat) -> Result<(), SendError> {
        check(
            self.http
                .post(self.url(&format!("/{}/heartbeat", self.id)))
                .json(hb)
                .send()
                .await?,
        )?;
        Ok(())
    }

    pub async fn upload(&self, start: Timestamp, wav: Vec<u8>) -> Result<(), SendError> {
        let resp = self
            .http
            .post(self.url(&format!("/{}/audio", self.id)))
            .header(HEADER_START_NS, start.to_string())
            .header(reqwest::header::CONTENT_TYPE, "audio/wav")
            .body(wav)
            .send()
            .await?;
        check(resp)?;
        Ok(())
    }

    pub async fn request_positioning(&self) -> Result<(), SendError> {
        check(
            self.http
                .post(self.url(&format!("/{}/position-request", self.id)))
                .send()
                .await?,
        )?;
        Ok(())
    }

    /// Re-registers after the server reported it does not know us.
    pub async fn recover(&self, e: SendError) {
        match e {
            SendError::Unknown => {
                tracing::warn!("server forgot this client; re-registering");
                self.register_until_ok().await;
            }
            SendError::Other(e) => tracing::warn!("request failed: {e:#}"),
        }
    }
}

pub async fn heartbeat_loop(
    session: Arc<Session>,
    interval: Duration,
    clock: SharedClock,
    stats: Arc<Stats>,
) {
    let started = Instant::now();
    let mut tick = tokio::time::interval(interval);
    loop {
        tick.tick().await;
        let hb = Heartbeat {
            clock: *clock.lock().unwrap(),
            uptime_s: started.elapsed().as_secs(),
            chunks_sent: stats.chunks_sent.load(Ordering::Relaxed),
            chunks_gated: stats.chunks_gated.load(Ordering::Relaxed),
            queue_len: stats.queue_len.load(Ordering::Relaxed),
            capture_dropouts: stats.dropouts.swap(0, Ordering::Relaxed),
        };
        if let Err(e) = session.heartbeat(&hb).await {
            stats
                .dropouts
                .fetch_add(hb.capture_dropouts, Ordering::Relaxed);
            session.recover(e).await;
        }
    }
}

/// Bounded queue of encoded chunks waiting to be uploaded; the oldest are
/// dropped when the server is unreachable for long.
pub struct UploadQueue {
    items: Mutex<VecDeque<(Timestamp, Vec<u8>)>>,
    notify: Notify,
    capacity: usize,
}

impl UploadQueue {
    pub fn new(capacity: usize) -> Self {
        Self {
            items: Mutex::default(),
            notify: Notify::new(),
            capacity,
        }
    }

    pub async fn push(&self, start: Timestamp, wav: Vec<u8>, stats: &Stats) {
        let mut q = self.items.lock().await;
        if q.len() >= self.capacity {
            q.pop_front();
            tracing::warn!("upload queue full; dropping oldest chunk");
        }
        q.push_back((start, wav));
        stats.queue_len.store(q.len() as u32, Ordering::Relaxed);
        self.notify.notify_one();
    }

    pub async fn run(&self, session: Arc<Session>, stats: Arc<Stats>) {
        loop {
            let next = self.items.lock().await.front().cloned();
            let Some((start, wav)) = next else {
                self.notify.notified().await;
                continue;
            };
            match session.upload(start, wav).await {
                Ok(()) => {
                    let mut q = self.items.lock().await;
                    // The chunk may already have been evicted while uploading.
                    if q.front().is_some_and(|(s, _)| *s == start) {
                        q.pop_front();
                    }
                    stats.queue_len.store(q.len() as u32, Ordering::Relaxed);
                    stats.chunks_sent.fetch_add(1, Ordering::Relaxed);
                }
                Err(e) => {
                    session.recover(e).await;
                    tokio::time::sleep(Duration::from_secs(2)).await;
                }
            }
        }
    }
}
