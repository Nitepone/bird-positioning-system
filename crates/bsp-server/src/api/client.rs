//! Endpoints used by client devices.

use super::{ApiError, ApiResult};
use crate::audio_buffer::TsfStamp;
use crate::clip_audio;
use crate::clock_history::ClockReport;
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
    state
        .db
        .touch_client(id, &addr.to_string(), hb.clock.as_ref(), now)?;
    state.event(Some(id), "heartbeat", serde_json::to_value(&hb)?);
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
    if !state
        .db
        .touch_client(id, &addr.to_string(), None, now_ns())?
    {
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
    use super::{clean_name, parse_tsf};

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
