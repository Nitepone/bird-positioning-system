//! Endpoints used by client devices.

use super::{ApiError, ApiResult};
use crate::state::{PositioningRequest, SharedState};
use axum::Json;
use axum::Router;
use axum::body::Bytes;
use axum::extract::{ConnectInfo, Path, State};
use axum::http::{HeaderMap, StatusCode};
use axum::routing::{get, post};
use bsp_core::audio::{self, AudioSample};
use bsp_proto::{
    AudioAccepted, ClientId, HEADER_START_NS, Heartbeat, NANOS_PER_SEC, RegisterRequest,
    RegisterResponse, now_ns,
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
    let is_new = state.db.register_client(
        req.client_id,
        &req.hostname,
        &req.version,
        &req.capabilities,
        &addr.to_string(),
        now_ns(),
    )?;
    state.event(
        Some(req.client_id),
        "register",
        json!({ "new": is_new, "addr": addr.to_string(), "hostname": req.hostname,
                "version": req.version, "capabilities": req.capabilities }),
    );
    let d = &state.cfg.clients;
    Ok(Json(RegisterResponse {
        udp_timesync_port: state.udp_port,
        heartbeat_interval_s: d.heartbeat_interval_s,
        chunk_secs: d.chunk_secs,
        gate: d.gate.clone(),
    }))
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
    let start: i64 = headers
        .get(HEADER_START_NS)
        .and_then(|v| v.to_str().ok())
        .and_then(|v| v.parse().ok())
        .ok_or_else(|| {
            ApiError::bad_request(format!("missing or invalid {HEADER_START_NS} header"))
        })?;
    if !state
        .db
        .touch_client(id, &addr.to_string(), None, now_ns())?
    {
        return Err(ApiError::not_found("client"));
    }
    let bytes = body.len();
    let (sample_rate, pcm) = tokio::task::spawn_blocking(move || audio::decode_wav(&body))
        .await?
        .map_err(ApiError::bad_request)?;
    let sample = AudioSample {
        client_id: id,
        start,
        sample_rate,
        pcm: pcm.into(),
    };
    let duration_s = sample.duration_ns() as f32 / NANOS_PER_SEC as f32;
    state.audio.push(sample.clone());
    state.event(
        Some(id),
        "audio",
        json!({ "bytes": bytes, "sample_rate": sample_rate, "duration_s": duration_s, "start_ns": start,
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
