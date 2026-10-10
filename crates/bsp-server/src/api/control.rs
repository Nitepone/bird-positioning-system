//! Configuration and monitoring endpoints used by the web UI.

use super::{ApiError, ApiResult};
use crate::clip_audio;
use crate::clock_history::{self, ClockReport};
use crate::db::{ClientRecord, DetectionFilter, Event, SpeciesSeen};
use crate::settings::{self, ConfidenceLevel, LocalSpeciesStatus, Settings};
use crate::state::{PositioningRequest, SharedState};
use crate::waveform;
use axum::Json;
use axum::Router;
use axum::extract::{Path, Query, State};
use axum::http::{StatusCode, header};
use axum::response::IntoResponse;
use axum::routing::get;
use bsp_core::detection::Detection;
use bsp_core::locator::Vec3;
use bsp_proto::{ClientId, NANOS_PER_SEC, Timestamp, Uuid, now_ns};
use serde::{Deserialize, Serialize};
use serde_json::json;

pub fn router() -> Router<SharedState> {
    Router::new()
        .route("/status", get(status))
        .route("/settings", get(get_settings).put(put_settings))
        .route("/local-species", get(local_species))
        .route("/clients", get(list_clients))
        .route(
            "/clients/{id}",
            get(get_client).put(configure_client).delete(delete_client),
        )
        .route("/clients/{id}/clock", get(client_clock))
        .route("/detections", get(list_detections))
        .route("/timeline", get(timeline))
        .route("/species", get(species_seen))
        .route("/detections/{id}/audio/{client_id}", get(detection_audio))
        .route("/detections/{id}/waveform", get(detection_waveform))
        .route("/events", get(list_events))
        .route(
            "/positioning-request",
            get(get_positioning).delete(clear_positioning),
        )
}

#[derive(Serialize)]
struct ClientView {
    #[serde(flatten)]
    record: ClientRecord,
    active: bool,
    synced: bool,
    /// Worst-case error of the client's audio timestamps (ns).
    clock_error_ns: Option<i64>,
    positioning_requested: bool,
}

fn view(state: &SharedState, record: ClientRecord, now: Timestamp) -> ClientView {
    let req = state.positioning_request();
    ClientView {
        active: state.is_active(&record, now),
        synced: state.is_synced(&record, now),
        clock_error_ns: record.clock.map(|k| k.error_bound_ns()),
        positioning_requested: req.is_some_and(|r| r.client_id == record.id),
        record,
    }
}

async fn status(State(state): State<SharedState>) -> ApiResult<Json<serde_json::Value>> {
    let now = now_ns();
    let clients = state.db.list_clients()?;
    Ok(Json(json!({
        "version": env!("CARGO_PKG_VERSION"),
        "server_time": now,
        "started_at": state.started_at,
        "identifier": state.identifier.name(),
        "udp_port": state.udp_port,
        "clients_total": clients.len(),
        "clients_active": clients.iter().filter(|c| state.is_active(c, now)).count(),
        "clients_synced": clients.iter().filter(|c| state.is_synced(c, now)).count(),
        "max_call_skew_ms": state.cfg.max_call_skew_ms,
        "max_clock_offset_us": state.cfg.max_clock_offset_us,
    })))
}

async fn list_clients(State(state): State<SharedState>) -> ApiResult<Json<Vec<ClientView>>> {
    let now = now_ns();
    Ok(Json(
        state
            .db
            .list_clients()?
            .into_iter()
            .map(|c| view(&state, c, now))
            .collect(),
    ))
}

async fn get_client(
    State(state): State<SharedState>,
    Path(id): Path<ClientId>,
) -> ApiResult<Json<ClientView>> {
    let c = state
        .db
        .get_client(id)?
        .ok_or_else(|| ApiError::not_found("client"))?;
    Ok(Json(view(&state, c, now_ns())))
}

#[derive(Deserialize)]
struct ClientConfig {
    name: String,
    position: Option<Vec3>,
}

async fn configure_client(
    State(state): State<SharedState>,
    Path(id): Path<ClientId>,
    Json(cfg): Json<ClientConfig>,
) -> ApiResult<StatusCode> {
    if let Some(p) = cfg.position
        && ![p.x, p.y, p.z].iter().all(|v| v.is_finite())
    {
        return Err(ApiError::bad_request("position must be finite"));
    }
    if !state
        .db
        .configure_client(id, cfg.name.trim(), cfg.position)?
    {
        return Err(ApiError::not_found("client"));
    }
    state.event(
        Some(id),
        "config",
        json!({ "name": cfg.name.trim(), "position": cfg.position }),
    );
    if cfg.position.is_some() {
        let mut slot = state.positioning.lock().unwrap();
        if slot.is_some_and(|r| r.client_id == id) {
            *slot = None;
        }
    }
    Ok(StatusCode::NO_CONTENT)
}

async fn delete_client(
    State(state): State<SharedState>,
    Path(id): Path<ClientId>,
) -> ApiResult<StatusCode> {
    if !state.db.delete_client(id)? {
        return Err(ApiError::not_found("client"));
    }
    state.event(Some(id), "deleted", json!({}));
    state.clock_history.forget(id);
    Ok(StatusCode::NO_CONTENT)
}

#[derive(Deserialize)]
struct ClockQuery {
    minutes: Option<u32>,
}

#[derive(Serialize)]
struct ClockReportView {
    #[serde(flatten)]
    report: ClockReport,
    /// Worst-case timestamp error, filled in for clients that do not report it.
    error_ns: Option<i64>,
}

/// `GET /clients/{id}/clock?minutes=`: the client's clock reports over the
/// last `minutes` (default 30, at most the retained hour), oldest first.
async fn client_clock(
    State(state): State<SharedState>,
    Path(id): Path<ClientId>,
    Query(q): Query<ClockQuery>,
) -> ApiResult<Json<serde_json::Value>> {
    let c = state
        .db
        .get_client(id)?
        .ok_or_else(|| ApiError::not_found("client"))?;
    let now = now_ns();
    let span_ns =
        (q.minutes.unwrap_or(30) as i64 * 60 * NANOS_PER_SEC).min(clock_history::RETENTION_NS);
    let reports: Vec<ClockReportView> = state
        .clock_history
        .since(id, now - span_ns)
        .into_iter()
        .map(|report| ClockReportView {
            error_ns: report.clock.map(|k| k.error_bound_ns()),
            report,
        })
        .collect();
    Ok(Json(json!({
        "client": view(&state, c, now),
        "server_time": now,
        "from": now - span_ns,
        "max_clock_offset_us": state.cfg.max_clock_offset_us,
        "heartbeat_interval_s": state.cfg.clients.heartbeat_interval_s,
        "reports": reports,
    })))
}

#[derive(Deserialize)]
struct Limit {
    limit: Option<u32>,
}

#[derive(Serialize)]
struct DetectionView {
    #[serde(flatten)]
    detection: Detection,
    /// Clients with a playable clip, best first (see [`clip_order`]).
    clips: Vec<ClientId>,
}

/// Orders the clients that have clips: the locator's reference (loudest)
/// first, then by call confidence.
fn clip_order(d: &Detection, mut available: Vec<ClientId>) -> Vec<ClientId> {
    let rank = |id: &ClientId| {
        if d.location.as_ref().is_some_and(|l| l.reference == *id) {
            return f32::MAX;
        }
        d.calls
            .iter()
            .find(|c| c.client_id == *id)
            .map_or(0.0, |c| c.confidence)
    };
    available.sort_by(|a, b| rank(b).total_cmp(&rank(a)));
    available
}

async fn list_detections(
    State(state): State<SharedState>,
    Query(filter): Query<DetectionFilter>,
    Query(q): Query<Limit>,
) -> ApiResult<Json<Vec<DetectionView>>> {
    let detections = state
        .db
        .list_detections(&filter, q.limit.unwrap_or(100).min(1000))?;
    let mut out = Vec::with_capacity(detections.len());
    for d in detections {
        let clips = clip_order(&d, state.db.clip_clients(d.id)?);
        out.push(DetectionView {
            detection: d,
            clips,
        });
    }
    Ok(Json(out))
}

/// Most detections the timeline returns at once.
const TIMELINE_LIMIT: u32 = 20_000;

#[derive(Deserialize)]
struct TimelineQuery {
    from: Timestamp,
    to: Timestamp,
}

/// One detection, reduced to what the dashboard timeline draws.
#[derive(Serialize)]
struct TimelineEntry {
    id: Uuid,
    time: Timestamp,
    end: Timestamp,
    scientific: String,
    common: String,
    confidence: f32,
    /// Flagged unexpected when detected (with the site settings of the time).
    unexpected: bool,
    /// Classification against the current site's range data.
    occurrence: Occurrence,
    clients: Vec<ClientId>,
    cardinal: Option<bsp_core::locator::Cardinal>,
}

/// Whether a species is expected at the site.
#[derive(Clone, Copy, Serialize)]
#[serde(rename_all = "snake_case")]
enum Occurrence {
    Expected,
    Unexpected,
    /// No site location set, or the range data doesn't cover the species
    /// (non-birds, the mock identifier's "Unknown bird", some renamed species).
    Unknown,
}

#[derive(Serialize)]
struct Timeline {
    /// Whether a site location is set, so `occurrence` can be other than unknown.
    site_set: bool,
    detections: Vec<TimelineEntry>,
    /// More than `TIMELINE_LIMIT` detections fell in the range; the oldest were left out.
    truncated: bool,
}

async fn timeline(
    State(state): State<SharedState>,
    Query(q): Query<TimelineQuery>,
) -> ApiResult<Json<Timeline>> {
    if q.to <= q.from {
        return Err(ApiError::bad_request("`to` must be after `from`"));
    }
    let filter = DetectionFilter {
        from: Some(q.from),
        to: Some(q.to),
        ..Default::default()
    };
    let found = state.db.list_detections(&filter, TIMELINE_LIMIT + 1)?;
    let truncated = found.len() > TIMELINE_LIMIT as usize;
    let detections = found
        .into_iter()
        .take(TIMELINE_LIMIT as usize)
        .map(|d| TimelineEntry {
            occurrence: match state.species_filter.is_expected(&d.species) {
                Some(true) => Occurrence::Expected,
                Some(false) => Occurrence::Unexpected,
                None => Occurrence::Unknown,
            },
            id: d.id,
            time: d.time,
            end: d.calls.iter().map(|c| c.end).max().unwrap_or(d.time),
            clients: d.calls.iter().map(|c| c.client_id).collect(),
            cardinal: d.location.as_ref().map(|l| l.cardinal),
            scientific: d.species.scientific,
            common: d.species.common,
            confidence: d.confidence,
            unexpected: d.unexpected,
        })
        .collect();
    let site_set = state.settings.lock().await.site.is_some();
    Ok(Json(Timeline {
        site_set,
        detections,
        truncated,
    }))
}

async fn species_seen(State(state): State<SharedState>) -> ApiResult<Json<Vec<SpeciesSeen>>> {
    Ok(Json(state.db.species_seen()?))
}

/// Stored clips never change, so browsers may cache them indefinitely.
const IMMUTABLE: &str = "public, max-age=31536000, immutable";

/// Where the identified call lies within a clip, in seconds from its start,
/// as `start,end` (for drawing it over the clip in the browser).
const CALL_RANGE: &str = "x-call-range";

async fn detection_audio(
    State(state): State<SharedState>,
    Path((id, client)): Path<(Uuid, ClientId)>,
) -> ApiResult<impl IntoResponse> {
    let clip = state
        .db
        .get_clip(id, client)?
        .ok_or_else(|| ApiError::not_found("clip"))?;
    let call = state
        .db
        .get_detection(id)?
        .and_then(|d| d.calls.into_iter().find(|c| c.client_id == client));
    let range = call.map_or(String::new(), |c| {
        let secs = |t: Timestamp| (t - clip.start) as f64 / NANOS_PER_SEC as f64;
        format!("{:.3},{:.3}", secs(c.start), secs(c.end))
    });
    Ok((
        [
            (header::CONTENT_TYPE, clip_audio::content_type(&clip.audio)),
            (header::CACHE_CONTROL, IMMUTABLE),
        ],
        [(CALL_RANGE, range)],
        clip.audio,
    ))
}

#[derive(Deserialize)]
struct WaveformQuery {
    /// Which client's clip to draw; the best one when omitted.
    client: Option<ClientId>,
}

async fn detection_waveform(
    State(state): State<SharedState>,
    Path(id): Path<Uuid>,
    Query(q): Query<WaveformQuery>,
) -> ApiResult<impl IntoResponse> {
    let d = state
        .db
        .get_detection(id)?
        .ok_or_else(|| ApiError::not_found("detection"))?;
    let client = match q.client {
        Some(c) => c,
        None => *clip_order(&d, state.db.clip_clients(id)?)
            .first()
            .ok_or_else(|| ApiError::not_found("clip"))?,
    };
    let clip = state
        .db
        .get_clip(id, client)?
        .ok_or_else(|| ApiError::not_found("clip"))?;
    let svg = tokio::task::spawn_blocking(move || -> anyhow::Result<String> {
        let (sample_rate, pcm) = clip_audio::decode(&clip.audio)?;
        let len_ns = pcm.len() as f64 * NANOS_PER_SEC as f64 / sample_rate as f64;
        let highlight = d.calls.iter().find(|c| c.client_id == client).map(|c| {
            (
                (c.start - clip.start) as f64 / len_ns,
                (c.end - clip.start) as f64 / len_ns,
            )
        });
        Ok(waveform::render_svg(&pcm, highlight))
    })
    .await??;
    Ok((
        [
            (header::CONTENT_TYPE, "image/svg+xml"),
            (header::CACHE_CONTROL, IMMUTABLE),
        ],
        svg,
    ))
}

#[derive(Deserialize)]
struct EventQuery {
    after_id: Option<i64>,
    client_id: Option<ClientId>,
    kind: Option<String>,
    exclude_kind: Option<String>,
    limit: Option<u32>,
}

async fn list_events(
    State(state): State<SharedState>,
    Query(q): Query<EventQuery>,
) -> ApiResult<Json<Vec<Event>>> {
    let empty = |s: Option<String>| s.filter(|s| !s.is_empty());
    let (kind, exclude) = (empty(q.kind), empty(q.exclude_kind));
    Ok(Json(state.db.list_events(
        q.after_id,
        q.client_id,
        kind.as_deref(),
        exclude.as_deref(),
        q.limit.unwrap_or(200).min(2000),
    )?))
}

async fn get_positioning(State(state): State<SharedState>) -> Json<Option<PositioningRequest>> {
    Json(state.positioning_request())
}

async fn clear_positioning(State(state): State<SharedState>) -> StatusCode {
    *state.positioning.lock().unwrap() = None;
    StatusCode::NO_CONTENT
}

#[derive(Serialize)]
struct LevelView {
    id: ConfidenceLevel,
    label: &'static str,
    value: f32,
}

/// Summary of the expected-species list (the full list is `/local-species`).
#[derive(Serialize)]
struct LocalSpeciesSummary {
    status: &'static str,
    count: usize,
    error: Option<String>,
}

#[derive(Serialize)]
struct SettingsView {
    settings: Settings,
    levels: Vec<LevelView>,
    local_species: LocalSpeciesSummary,
}

async fn settings_view(state: &SharedState) -> SettingsView {
    let settings = state.settings.lock().await.clone();
    let local_species = match &*state.local_species.read().unwrap() {
        LocalSpeciesStatus::NoLocation => LocalSpeciesSummary {
            status: "no_location",
            count: 0,
            error: None,
        },
        LocalSpeciesStatus::NoGeoModel => LocalSpeciesSummary {
            status: "no_geo_model",
            count: 0,
            error: None,
        },
        LocalSpeciesStatus::Ready { species, .. } => LocalSpeciesSummary {
            status: "ready",
            count: species.len(),
            error: None,
        },
        LocalSpeciesStatus::Error { error } => LocalSpeciesSummary {
            status: "error",
            count: 0,
            error: Some(error.clone()),
        },
    };
    SettingsView {
        settings,
        levels: ConfidenceLevel::ALL
            .iter()
            .map(|&l| LevelView {
                id: l,
                label: l.label(),
                value: l.value(),
            })
            .collect(),
        local_species,
    }
}

async fn get_settings(State(state): State<SharedState>) -> Json<SettingsView> {
    Json(settings_view(&state).await)
}

async fn put_settings(
    State(state): State<SharedState>,
    Json(new): Json<Settings>,
) -> ApiResult<Json<SettingsView>> {
    new.validate().map_err(ApiError::bad_request)?;
    // Held across the update so concurrent saves apply in order.
    let mut current = state.settings.lock().await;
    state
        .db
        .set_setting(settings::DB_KEY, &serde_json::to_string(&new)?)?;
    settings::apply(&state, &new, Some(&current)).await;
    *current = new.clone();
    drop(current);
    state.event(None, "settings", serde_json::to_value(&new)?);
    Ok(Json(settings_view(&state).await))
}

async fn local_species(State(state): State<SharedState>) -> Json<serde_json::Value> {
    Json(serde_json::to_value(&*state.local_species.read().unwrap()).unwrap_or_default())
}
