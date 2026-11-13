//! Endpoints used by client devices.

use super::{ApiError, ApiResult};
use crate::audio_buffer::TsfStamp;
use crate::clip_audio;
use crate::clock_history::ClockReport;
use crate::compat::{self, ApiCompat};
use crate::db::{ClientHealth, ClientRecord, RebootInfo};
use crate::firmware::{self, LastUpdate, UpdateResult};
use crate::state::{PositioningRequest, SharedState};
use axum::Json;
use axum::Router;
use axum::body::Bytes;
use axum::extract::{ConnectInfo, Path, State};
use axum::http::{HeaderMap, StatusCode, header};
use axum::response::IntoResponse;
use axum::routing::{get, post};
use bsp_core::audio::AudioSample;
use bsp_proto::{
    API_VERSION, AudioAccepted, ClientId, FirmwareOffer, FirmwareStatus, FirmwareUpdateState,
    HEADER_END_NS, HEADER_START_NS, HEADER_TSF, Heartbeat, HeartbeatResponse, LogBatch,
    NANOS_PER_MS, NANOS_PER_SEC, RebootReason, RegisterRequest, RegisterResponse, Timestamp,
    TriggerConfig, ValidationConfig, ValidationReport, now_ns,
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
        .route("/{id}/validation", post(validation_report))
        .route("/firmware/{file}", get(download_firmware))
}

async fn register(
    State(state): State<SharedState>,
    ConnectInfo(addr): ConnectInfo<SocketAddr>,
    Json(req): Json<RegisterRequest>,
) -> ApiResult<Json<RegisterResponse>> {
    let compat = compat::check(req.api_version.as_deref())
        .map_err(|e| ApiError::bad_request(format!("invalid api_version: {e}")))?;
    let name = clean_name(req.name.as_deref().unwrap_or(""));
    let before = state.db.get_client(req.client_id)?;
    // A client already refused for its version is not logged at each retry.
    let was_refused = before
        .as_ref()
        .is_some_and(|c| compat::check(c.api_version.as_deref()) == Ok(ApiCompat::Incompatible));
    // A refused client is kept too, so that the Clients page shows it.
    let is_new = state
        .db
        .register_client(&req, &name, &addr.to_string(), now_ns())?;
    let detail = json!({ "new": is_new, "addr": addr.to_string(), "hostname": req.hostname,
                         "version": req.version, "api_version": req.api_version,
                         "api_compat": compat, "capabilities": req.capabilities, "name": name,
                         "build": req.build, "ota": req.ota });
    if compat == ApiCompat::Incompatible {
        let msg = compat::refusal(req.api_version.as_deref().unwrap_or_default());
        if !was_refused {
            tracing::warn!(client = %req.client_id, "registration refused: {msg}");
            state.event(Some(req.client_id), "register_refused", detail);
        }
        return Err(ApiError(StatusCode::CONFLICT, msg));
    }
    if compat == ApiCompat::ClientNewer {
        tracing::warn!(
            client = %req.client_id,
            "client speaks API {}, newer than the server's {API_VERSION}",
            req.api_version.as_deref().unwrap_or_default()
        );
    }
    state.event(Some(req.client_id), "register", detail);
    if let Some(before) = &before {
        note_firmware_update(&state, before, &req)?;
    }
    let d = &state.cfg.clients;
    let (trigger, validation) = capture_settings(&state).await;
    Ok(Json(RegisterResponse {
        api_version: Some(API_VERSION.into()),
        udp_timesync_port: state.udp_port,
        heartbeat_interval_s: d.heartbeat_interval_s,
        chunk_secs: d.chunk_secs,
        gate: trigger.gate(),
        flac_uploads: true,
        trigger: Some(trigger),
        validation: Some(validation),
    }))
}

/// A registration with another build than before: logged as an update when
/// it is the build chosen for the client.
fn note_firmware_update(
    state: &SharedState,
    before: &ClientRecord,
    req: &RegisterRequest,
) -> ApiResult<()> {
    let Some(new) = &req.build else {
        return Ok(());
    };
    if before.build.as_ref().is_some_and(|b| b.same_build(new)) || before.firmware_policy.is_none()
    {
        return Ok(());
    }
    let builds = state.db.list_firmware_builds()?;
    let Some(t) = firmware::target(before.firmware_policy, Some(new), &builds) else {
        return Ok(());
    };
    if !t.is(new) {
        return Ok(());
    }
    let mut fw = before.firmware.clone();
    fw.last = Some(LastUpdate {
        at: now_ns(),
        result: UpdateResult::Updated,
        build_id: t.id,
        detail: None,
    });
    state.db.set_firmware_state(before.id, &fw)?;
    tracing::info!(client = %before.id, build = t.id, describe = %t.describe, "client updated");
    state.event(
        Some(before.id),
        "firmware_updated",
        json!({ "build_id": t.id, "describe": t.describe,
                "from": before.build.as_ref().map(|b| &b.describe) }),
    );
    Ok(())
}

/// Longest firmware status detail kept, in characters.
const MAX_FIRMWARE_DETAIL_CHARS: usize = 200;

/// A heartbeat's report on an update it was offered: kept for the Clients
/// page, and logged once. A failed build is not offered again until the
/// client's selection is set again.
fn note_firmware_status(
    state: &SharedState,
    c: &mut ClientRecord,
    st: &FirmwareStatus,
    now: Timestamp,
) {
    let result = match st.state {
        FirmwareUpdateState::Deferred => UpdateResult::Deferred,
        FirmwareUpdateState::Failed => UpdateResult::Failed,
        FirmwareUpdateState::RolledBack => UpdateResult::RolledBack,
        FirmwareUpdateState::Unknown => {
            tracing::warn!(client = %c.id, build = st.build_id, "unknown firmware status, ignored");
            return;
        }
    };
    let detail = st.detail.as_deref().map(|d| {
        clean_name(d)
            .chars()
            .take(MAX_FIRMWARE_DETAIL_CHARS)
            .collect::<String>()
    });
    // The same report again (the reply to the first was lost) is not news.
    let repeated = c
        .firmware
        .last
        .as_ref()
        .is_some_and(|l| l.result == result && l.build_id == st.build_id);
    c.firmware.last = Some(LastUpdate {
        at: now,
        result,
        build_id: st.build_id,
        detail: detail.clone(),
    });
    if result != UpdateResult::Deferred && !c.firmware.blocked.contains(&st.build_id) {
        c.firmware.blocked.push(st.build_id);
    }
    if repeated {
        return;
    }
    let (kind, detail) = match result {
        UpdateResult::Deferred => ("firmware_update_deferred", detail),
        UpdateResult::RolledBack => (
            "firmware_update_failed",
            Some(match detail {
                Some(d) => format!("rolled back: {d}"),
                None => "rolled back".into(),
            }),
        ),
        _ => ("firmware_update_failed", detail),
    };
    state.event(
        Some(c.id),
        kind,
        json!({ "build_id": st.build_id, "state": st.state, "detail": detail }),
    );
}

/// The firmware offer for a heartbeat reply (see [`firmware::offer`]), after
/// taking in the heartbeat's report on an earlier one. Each new offer is
/// logged once.
fn firmware_reply(
    state: &SharedState,
    mut c: ClientRecord,
    status: Option<&FirmwareStatus>,
    now: Timestamp,
) -> ApiResult<Option<FirmwareOffer>> {
    let before = c.firmware.clone();
    if let Some(st) = status {
        note_firmware_status(state, &mut c, st, now);
    }
    let builds = state.db.list_firmware_builds()?;
    let offer = firmware::offer(&c, &builds);
    if let Some(b) = offer
        && c.firmware.offered != Some(b.id)
    {
        c.firmware.offered = Some(b.id);
        state.event(
            Some(c.id),
            "firmware_offered",
            json!({ "build_id": b.id, "describe": b.describe,
                    "from": c.build.as_ref().map(|r| &r.describe) }),
        );
    }
    if c.firmware != before {
        state.db.set_firmware_state(c.id, &c.firmware)?;
    }
    Ok(offer.map(|b| b.offer()))
}

/// `GET /firmware/{id}.bin`: a firmware image, for clients updating
/// themselves (the URL in a `firmware_offer`), with its MD5 in `x-MD5`.
async fn download_firmware(
    State(state): State<SharedState>,
    Path(file): Path<String>,
) -> ApiResult<impl IntoResponse> {
    let b = file
        .strip_suffix(".bin")
        .and_then(|id| id.parse::<i64>().ok())
        .filter(|&id| id > 0)
        .map(|id| state.db.get_firmware_build(id))
        .transpose()?
        .flatten()
        .ok_or_else(|| ApiError::not_found("firmware build"))?;
    let path = state.cfg.firmware_dir.join(b.file_name());
    let image = tokio::fs::read(&path).await.map_err(|e| {
        tracing::error!(path = %path.display(), "firmware image missing: {e}");
        ApiError::not_found("firmware image")
    })?;
    Ok((
        [
            (header::CONTENT_TYPE, "application/octet-stream".to_string()),
            (header::CONTENT_LENGTH, image.len().to_string()),
            (header::HeaderName::from_static("x-md5"), b.md5),
        ],
        image,
    ))
}

/// The trigger and validation the operator wants clients to run.
async fn capture_settings(state: &SharedState) -> (TriggerConfig, ValidationConfig) {
    let s = state.settings.lock().await;
    (s.trigger_or(&state.cfg.clients.gate), s.validation.clone())
}

/// What a client's validation model decided about one recording: kept as an
/// event (shown in the log, and per client), for checking the model's results
/// and cost.
async fn validation_report(
    State(state): State<SharedState>,
    Path(id): Path<ClientId>,
    Json(report): Json<ValidationReport>,
) -> ApiResult<StatusCode> {
    state
        .db
        .get_client(id)?
        .ok_or_else(|| ApiError::not_found("client"))?;
    state.event(Some(id), "validation", serde_json::to_value(&report)?);
    Ok(StatusCode::NO_CONTENT)
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
/// storage card put in or taken out, closed for a low battery and opened
/// again, or running out of room, and its battery running low.
fn health_events(
    before: Option<&ClientHealth>,
    now: &ClientHealth,
) -> Vec<(&'static str, serde_json::Value)> {
    let mut events = Vec::new();
    let card = |h: Option<&ClientHealth>| h.and_then(|h| h.storage);
    // A card closed for a low battery is still in: it is paused, not removed.
    let paused = |h| card(h).is_some_and(|s| s.paused);
    let present = |h| card(h).is_some_and(|s| s.present || s.paused);
    if card(Some(now)).is_some() && paused(Some(now)) != paused(before) {
        let kind = if paused(Some(now)) {
            "storage_paused"
        } else {
            "storage_resumed"
        };
        events.push((kind, json!({ "storage": now.storage })));
    }
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
    // A client that reports no mode runs live.
    let mode = |h: Option<&ClientHealth>| h.and_then(|h| h.mode.as_ref()).map(|m| m.mode);
    let (was, is) = (mode(before).unwrap_or_default(), mode(Some(now)));
    if let Some(m) = now.mode.as_ref().filter(|_| is.is_some_and(|is| is != was)) {
        events.push((
            "mode_changed",
            json!({ "from": was, "to": m.mode, "interval_s": m.interval_s, "note": m.note }),
        ));
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

/// Two reports of a start this close together are of the same one (a
/// heartbeat repeated because its reply was lost).
const SAME_BOOT_NS: i64 = 10 * NANOS_PER_SEC;

/// A heartbeat's report of why the client started: kept, and logged once.
fn note_reboot(
    state: &SharedState,
    client: &ClientRecord,
    reason: RebootReason,
    uptime_s: u64,
    now: Timestamp,
) -> ApiResult<()> {
    let at = now - uptime_s as i64 * NANOS_PER_SEC;
    if client
        .last_reboot
        .is_some_and(|b| b.reason == reason && (b.at - at).abs() < SAME_BOOT_NS)
    {
        return Ok(());
    }
    state
        .db
        .set_last_reboot(client.id, &RebootInfo { reason, at })?;
    tracing::info!(client = %client.id, ?reason, "client restarted");
    state.event(
        Some(client.id),
        "client_rebooted",
        json!({ "reason": reason, "at": at, "uptime_s": uptime_s }),
    );
    Ok(())
}

/// A heartbeat's log lines, each kept as a `client_log` event at the time
/// the client logged it (its clock mapped onto the server's by the
/// heartbeat's own time; the network delay is left in).
fn store_log(state: &SharedState, id: ClientId, log: &LogBatch, now: Timestamp) {
    if log.dropped > 0 {
        state.event(
            Some(id),
            "client_log_dropped",
            json!({ "count": log.dropped }),
        );
    }
    for line in &log.lines {
        let ago_ms = log.now_ms.saturating_sub(line.0).min(i64::MAX as u64 / 2) as i64;
        let ts = now.saturating_sub(ago_ms.saturating_mul(NANOS_PER_MS));
        state.event_at(
            ts,
            Some(id),
            "client_log",
            json!({ "text": line.1, "ms": line.0 }),
        );
    }
}

async fn heartbeat(
    State(state): State<SharedState>,
    ConnectInfo(addr): ConnectInfo<SocketAddr>,
    Path(id): Path<ClientId>,
    Json(hb): Json<Heartbeat>,
) -> ApiResult<Json<HeartbeatResponse>> {
    let now = now_ns();
    let before = state
        .db
        .get_client(id)?
        .ok_or_else(|| ApiError::not_found("client"))?;
    let health = ClientHealth::of(&hb, now);
    state
        .db
        .record_heartbeat(id, &addr.to_string(), hb.clock.as_ref(), &health)?;
    // The log's lines are kept as events of their own.
    let mut detail = serde_json::to_value(&hb)?;
    if let Some(log) = detail.get_mut("log").and_then(|l| l.as_object_mut()) {
        let n = log.remove("lines").and_then(|l| l.as_array().map(Vec::len));
        log.insert("line_count".into(), n.unwrap_or(0).into());
    }
    state.event(Some(id), "heartbeat", detail);
    if let Some(reason) = hb.reboot_reason {
        note_reboot(&state, &before, reason, hb.uptime_s, now)?;
    }
    for ack in &hb.acks {
        if let Some(c) = state.db.ack_command(id, ack.command_id, ack.result, now)? {
            state.event(
                Some(id),
                "command_acked",
                json!({ "command_id": c.id, "command": c.command, "result": c.result }),
            );
        }
    }
    if let Some(log) = &hb.log {
        store_log(&state, id, log, now);
    }
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
    let (trigger, validation) = capture_settings(&state).await;
    // Commands go only to clients that speak an API version with them.
    let mut commands = Vec::new();
    if compat::takes_commands(after.api_version.as_deref()) {
        let pending = state.db.pending_commands(id)?;
        commands = pending.iter().filter_map(|c| c.to_command()).collect();
        let ids: Vec<i64> = pending.iter().map(|c| c.id).collect();
        state.db.mark_delivered(&ids, now)?;
    }
    let mode = after.mode;
    let firmware_offer = firmware_reply(&state, after, hb.firmware_status.as_ref(), now)?;
    Ok(Json(HeartbeatResponse {
        mode,
        trigger: Some(trigger),
        validation: Some(validation),
        commands,
        firmware_offer,
    }))
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
    use bsp_proto::{BatteryStatus, ModeStatus, OperationMode, StorageStatus};

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
                charging: None,
            }),
            storage: card.map(|(present, free)| StorageStatus {
                present,
                total_bytes: present.then_some(32 << 30),
                free_bytes: present.then_some(free),
                recordings: 0,
                recorded_bytes: 0,
                errors: 0,
                dropped_bytes: 0,
                paused: false,
            }),
            devices: Vec::new(),
            mode: None,
            trigger: None,
            validation: None,
            validation_note: None,
            chunks_rejected: 0,
            logging: None,
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
    fn a_card_closed_for_a_low_battery_is_paused_not_removed() {
        let roomy = Some((true, 8 << 30));
        let fine = health(Some(80.0), roomy);
        let mut paused = health(Some(10.0), Some((false, 0)));
        paused.storage.as_mut().unwrap().paused = true;
        assert_eq!(kinds(Some(&fine), &paused), ["storage_paused", "battery_low"]);
        assert!(kinds(Some(&paused), &paused).is_empty());
        // Opened again: resumed, not inserted.
        assert_eq!(kinds(Some(&paused), &fine), ["storage_resumed"]);
        // The battery recovered but the card does not mount: resumed, then removed.
        assert_eq!(
            kinds(Some(&paused), &health(Some(10.0), Some((false, 0)))),
            ["storage_resumed", "storage_removed"]
        );
    }

    #[test]
    fn reports_mode_changes() {
        let with_mode = |mode| ClientHealth {
            mode: Some(ModeStatus {
                mode,
                interval_s: 300,
                backlog: 0,
                backlog_bytes: 0,
                delivered_through: None,
                next_contact_s: None,
                radio_on_pct: None,
                gap: None,
                note: None,
            }),
            ..health(None, None)
        };
        let (live, batch) = (
            with_mode(OperationMode::Live),
            with_mode(OperationMode::Batch),
        );
        // Live is what a client without modes runs.
        assert!(kinds(None, &live).is_empty());
        assert!(kinds(Some(&health(None, None)), &live).is_empty());
        assert_eq!(kinds(Some(&live), &batch), ["mode_changed"]);
        assert!(kinds(Some(&batch), &batch).is_empty());
        assert_eq!(kinds(Some(&batch), &live), ["mode_changed"]);
        assert_eq!(kinds(None, &batch), ["mode_changed"]);
        let low = with_mode(OperationMode::LowPower);
        assert_eq!(kinds(Some(&live), &low), ["mode_changed"]);
        assert!(kinds(Some(&low), &low).is_empty());
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
