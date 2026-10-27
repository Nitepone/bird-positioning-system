//! Endpoints used by client devices.

use super::{ApiError, ApiResult};
use crate::audio_buffer::TsfStamp;
use crate::clip_audio;
use crate::clock_history::ClockReport;
use crate::db::ClientHealth;
use crate::state::{PositioningRequest, SharedState};
use axum::Json;
use axum::Router;
use axum::body::Bytes;
use axum::extract::{ConnectInfo, Path, State};
use axum::http::{HeaderMap, StatusCode};
use axum::routing::{get, post};
use bsp_core::audio::AudioSample;
use bsp_proto::{
    AudioAccepted, ClientId, HEADER_END_NS, HEADER_START_NS, HEADER_TSF, Heartbeat, NANOS_PER_SEC,
    RegisterRequest, RegisterResponse, now_ns,
};
use serde_json::json;
use std::net::SocketAddr;

pub fn router() -> Router<SharedState> {
    Router::new()
        .route("/register", post(register))
        .route("/timesync", get(crate::timesync::websocket))
        .route("/{id}/heartbeat", post(heartbeat))
        .route("/{id}/audio", post(upload_audio))
        .route("/{id}/position-request", post(position_request))
}

async fn register(
    State(state): State<SharedState>,
    ConnectInfo(addr): ConnectInfo<SocketAddr>,
    Json(req): Json<RegisterRequest>,
) -> ApiResult<Json<RegisterResponse>> {
    let name = clean_name(req.name.as_deref().unwrap_or(""));
    let is_new = state
        .db
        .register_client(&req, &name, &addr.to_string(), now_ns())?;
    state.event(
        Some(req.client_id),
        "register",
        json!({ "new": is_new, "addr": addr.to_string(), "hostname": req.hostname,
                "version": req.version, "capabilities": req.capabilities, "name": name }),
    );
    let d = &state.cfg.clients;
    Ok(Json(RegisterResponse {
        udp_timesync_port: state.udp_port,
        heartbeat_interval_s: d.heartbeat_interval_s,
        chunk_secs: d.chunk_secs,
        gate: d.gate.clone(),
        flac_uploads: true,
    }))
}

/// `<bssid> <start> <end>` from [`HEADER_TSF`].
fn parse_tsf(v: &str) -> Option<TsfStamp> {
    let mut it = v.split_whitespace();
    let stamp = TsfStamp {
        bssid: it.next()?.parse().ok()?,
        start: it.next()?.parse().ok()?,
        end: it.next()?.parse().ok()?,
    };
    (it.next().is_none() && stamp.end > stamp.start).then_some(stamp)
}

/// Below this the battery counts as low (percent).
const BATTERY_LOW_PCT: f32 = 20.0;
/// Below this a storage card counts as nearly full (bytes).
const STORAGE_LOW_BYTES: u64 = 1 << 30;

/// Events for changes in a client's health between two heartbeats: its
/// storage card put in or taken out, or running out of room, and its battery
/// running low.
fn health_events(
    before: Option<&ClientHealth>,
    now: &ClientHealth,
) -> Vec<(&'static str, serde_json::Value)> {
    let mut events = Vec::new();
    let card = |h: Option<&ClientHealth>| h.and_then(|h| h.storage);
    let present = |h| card(h).is_some_and(|s| s.present);
    // A first report of an empty slot says nothing new.
    if card(Some(now)).is_some() && present(before) != present(Some(now)) {
        let kind = if present(Some(now)) {
            "storage_inserted"
        } else {
            "storage_removed"
        };
        if present(Some(now)) || card(before).is_some() {
            events.push((kind, json!({ "storage": now.storage })));
        }
    }
    let low_room = |h| {
        card(h)
            .and_then(|s| s.free_bytes)
            .is_some_and(|f| f < STORAGE_LOW_BYTES)
    };
    if low_room(Some(now)) && !low_room(before) {
        events.push(("storage_low", json!({ "storage": now.storage })));
    }
    let low = |h: Option<&ClientHealth>| {
        h.and_then(|h| h.battery)
            .is_some_and(|b| b.percent < BATTERY_LOW_PCT)
    };
    if low(Some(now)) && !low(before) {
        events.push(("battery_low", json!({ "battery": now.battery })));
    }
    events
}

/// Longest suggested name kept, in characters.
const MAX_NAME_CHARS: usize = 64;

/// A client's suggested name, made safe to show: one line, trimmed, bounded.
fn clean_name(name: &str) -> String {
    let one_line: String = name
        .chars()
        .map(|c| if c.is_control() { ' ' } else { c })
        .collect();
    one_line
        .split_whitespace()
        .collect::<Vec<_>>()
        .join(" ")
        .chars()
        .take(MAX_NAME_CHARS)
        .collect()
}

async fn heartbeat(
    State(state): State<SharedState>,
    ConnectInfo(addr): ConnectInfo<SocketAddr>,
    Path(id): Path<ClientId>,
    Json(hb): Json<Heartbeat>,
) -> ApiResult<StatusCode> {
    let now = now_ns();
    let before = state
        .db
        .get_client(id)?
        .ok_or_else(|| ApiError::not_found("client"))?;
    let health = ClientHealth::of(&hb, now);
    state
        .db
        .record_heartbeat(id, &addr.to_string(), hb.clock.as_ref(), &health)?;
    state.event(Some(id), "heartbeat", serde_json::to_value(&hb)?);
    for (kind, detail) in health_events(before.health.as_ref(), &health) {
        state.event(Some(id), kind, detail);
    }
    if let Some(b) = hb.clock.and_then(|k| k.beacon) {
        state.beacon_clocks.push(id, &b, now);
    }
    state.clock_history.push(
        id,
        ClockReport {
            at: now,
            clock: hb.clock,
        },
    );

    let after = state
        .db
        .get_client(id)?
        .ok_or_else(|| ApiError::not_found("client"))?;
    let (was, is) = (state.is_synced(&before, now), state.is_synced(&after, now));
    if was != is {
        state.event(
            Some(id),
            if is { "clock_synced" } else { "clock_unsynced" },
            json!({ "clock": hb.clock }),
        );
    }
    if hb.capture_dropouts > 0 {
        state.event(
            Some(id),
            "capture_dropout",
            json!({ "count": hb.capture_dropouts }),
        );
    }
    Ok(StatusCode::NO_CONTENT)
}

async fn upload_audio(
    State(state): State<SharedState>,
    ConnectInfo(addr): ConnectInfo<SocketAddr>,
    Path(id): Path<ClientId>,
    headers: HeaderMap,
    body: Bytes,
) -> ApiResult<(StatusCode, Json<AudioAccepted>)> {
    let header = |name: &str| -> Option<i64> { headers.get(name)?.to_str().ok()?.parse().ok() };
    let start = header(HEADER_START_NS).ok_or_else(|| {
        ApiError::bad_request(format!("missing or invalid {HEADER_START_NS} header"))
    })?;
    let end = header(HEADER_END_NS);
    let tsf = headers
        .get(HEADER_TSF)
        .and_then(|v| v.to_str().ok())
        .and_then(parse_tsf);
    if !state.db.touch_client(id, &addr.to_string(), now_ns())? {
        return Err(ApiError::not_found("client"));
    }
    let bytes = body.len();
    // WAV, or FLAC from clients that compress (sent with chunked encoding).
    let (sample_rate, pcm) = tokio::task::spawn_blocking(move || clip_audio::decode(&body))
        .await?
        .map_err(ApiError::bad_request)?;
    // Audio stamped on an access point's clock is converted by its line.
    let (start, end) = match tsf.and_then(|t| Some((t, state.beacon_clocks.line(t.bssid)?))) {
        Some((t, line)) => (line.server_at(t.start), Some(line.server_at(t.end))),
        None => (start, end),
    };
    let clock_ppb = end
        .and_then(|end| AudioSample::clock_ppb_from(sample_rate, pcm.len(), start, end))
        .unwrap_or(0);
    let sample = AudioSample {
        client_id: id,
        start,
        sample_rate,
        pcm: pcm.into(),
        clock_ppb,
    };
    let duration_s = sample.duration_ns() as f32 / NANOS_PER_SEC as f32;
    state.audio.push(sample.clone(), tsf);
    state.event(
        Some(id),
        "audio",
        json!({ "bytes": bytes, "sample_rate": sample_rate, "duration_s": duration_s, "start_ns": start,
                "clock_ppb": clock_ppb, "tsf": tsf.map(|t| t.bssid.to_string()),
                "latency_ms": (now_ns() - sample.end()) / 1_000_000 }),
    );

    let st = state.clone();
    tokio::spawn(async move {
        let identifier = st.identifier.clone();
        let result = tokio::task::spawn_blocking(move || identifier.identify(&sample)).await;
        match result {
            Ok(Ok(calls)) => {
                if !calls.is_empty() {
                    let species: Vec<_> = calls
                        .iter()
                        .map(|c| (&c.species.common, c.confidence))
                        .collect();
                    st.event(
                        Some(id),
                        "identified",
                        json!({ "calls": calls.len(), "species": species }),
                    );
                    let _ = st.calls_tx.send(calls);
                }
            }
            Ok(Err(e)) => st.event(
                Some(id),
                "identify_error",
                json!({ "error": e.to_string() }),
            ),
            Err(e) => tracing::error!("identifier panicked: {e}"),
        }
    });

    Ok((StatusCode::ACCEPTED, Json(AudioAccepted { duration_s })))
}

async fn position_request(
    State(state): State<SharedState>,
    Path(id): Path<ClientId>,
) -> ApiResult<StatusCode> {
    if state.db.get_client(id)?.is_none() {
        return Err(ApiError::not_found("client"));
    }
    let now = now_ns();
    let req = PositioningRequest {
        client_id: id,
        requested_at: now,
        expires_at: now + state.cfg.positioning_request_ttl_s as i64 * NANOS_PER_SEC,
    };
    *state.positioning.lock().unwrap() = Some(req);
    state.event(
        Some(id),
        "position_request",
        json!({ "expires_at": req.expires_at }),
    );
    Ok(StatusCode::NO_CONTENT)
}

#[cfg(test)]
mod tests {
    use super::{clean_name, health_events, parse_tsf};
    use crate::db::ClientHealth;
    use bsp_proto::{BatteryStatus, StorageStatus};

    fn health(battery: Option<f32>, card: Option<(bool, u64)>) -> ClientHealth {
        ClientHealth {
            at: 0,
            uptime_s: 0,
            chunks_sent: 0,
            chunks_gated: 0,
            battery: battery.map(|percent| BatteryStatus {
                percent,
                voltage_v: 3.7,
                rate_pct_per_h: None,
            }),
            storage: card.map(|(present, free)| StorageStatus {
                present,
                total_bytes: present.then_some(32 << 30),
                free_bytes: present.then_some(free),
                recordings: 0,
                recorded_bytes: 0,
                errors: 0,
                dropped_bytes: 0,
            }),
            devices: Vec::new(),
        }
    }

    fn kinds(before: Option<&ClientHealth>, now: &ClientHealth) -> Vec<&'static str> {
        health_events(before, now)
            .into_iter()
            .map(|(k, _)| k)
            .collect()
    }

    #[test]
    fn reports_changes_in_health() {
        let roomy = Some((true, 8 << 30));
        let fine = health(Some(80.0), roomy);
        assert!(kinds(None, &fine).contains(&"storage_inserted"));
        assert!(kinds(Some(&fine), &fine).is_empty());
        // An empty slot at first is not news; a card taken out is.
        assert!(kinds(None, &health(Some(80.0), Some((false, 0)))).is_empty());
        assert_eq!(
            kinds(Some(&fine), &health(Some(80.0), Some((false, 0)))),
            ["storage_removed"]
        );
        assert_eq!(
            kinds(Some(&fine), &health(Some(80.0), Some((true, 1 << 20)))),
            ["storage_low"]
        );
        // Low once, on the way down.
        let low = health(Some(19.5), roomy);
        assert_eq!(kinds(Some(&fine), &low), ["battery_low"]);
        assert!(kinds(Some(&low), &health(Some(18.0), roomy)).is_empty());
        // No gauge, no card: nothing.
        assert!(kinds(None, &health(None, None)).is_empty());
    }

    #[test]
    fn cleans_suggested_names() {
        assert_eq!(clean_name("  North\tfence\n "), "North fence");
        assert_eq!(clean_name(""), "");
        assert_eq!(clean_name(&"é".repeat(100)).chars().count(), 64);
    }

    #[test]
    fn parses_tsf_stamps() {
        let t = parse_tsf("04:f4:1c:07:93:52 19946541465989000 19946550465989000").unwrap();
        assert_eq!(t.bssid.to_string(), "04:f4:1c:07:93:52");
        assert_eq!((t.start, t.end), (19946541465989000, 19946550465989000));
        assert!(parse_tsf("04:f4:1c:07:93:52 5 4").is_none());
        assert!(parse_tsf("04:f4:1c:07:93:52 5").is_none());
        assert!(parse_tsf("nope 1 2").is_none());
    }
}
