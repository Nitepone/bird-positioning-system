//! bsp server: receives audio from clients, identifies calls, groups them into
//! detections, locates them, and serves the control API + web UI.

pub mod api;
pub mod audio_buffer;
pub mod beacon_clock;
pub mod clip_audio;
pub mod clock_history;
pub mod compat;
pub mod config;
pub mod db;
pub mod firmware;
pub mod pipeline;
pub mod settings;
pub mod state;
pub mod timesync;
pub mod waveform;
pub mod web;

use anyhow::Context;
use axum::Router;
use axum::extract::DefaultBodyLimit;
use axum::http::{HeaderName, header};
use axum::response::{Html, IntoResponse};
use axum::routing::get;
use bsp_core::identifier::{Identifier, LocalSpeciesFilter, MockIdentifier, RangeModel};
use bsp_core::locator::TdoaLocator;
use bsp_proto::{CLIENT_API_PREFIX, ClientId, now_ns};
use config::{IdentifierKind, ServerConfig};
use db::Db;
use state::{AppState, SharedState};
use std::collections::HashMap;
use std::net::SocketAddr;
use std::sync::Arc;
use std::time::Duration;
use tokio::net::{TcpListener, UdpSocket};
use tokio::sync::mpsc;

/// This build of the server, from `git describe` (see build.rs).
pub const BUILD: &str = env!("BPS_GIT_DESCRIBE");

const INDEX_HTML: &str = include_str!("../web/index.html");
/// Charting library for the web UI, embedded so the UI works offline.
const ECHARTS_JS: &str = include_str!("../web/vendor/echarts-6.1.0/echarts.min.js");
/// Detections page: grouping detections into the audio samples they were heard in.
const DETECTION_SAMPLES_JS: &str = include_str!("../web/detection-samples.js");
const CLIENT_HTML: &str = include_str!("../web/client.html");
const CLIENT_WORKLET_JS: &str = include_str!("../web/client-worklet.js");

/// Cross-origin isolation for the browser client: it raises the resolution of
/// `performance.now()` (otherwise coarsened to 0.1-1 ms), which the client's
/// timestamps and clock measurement rely on.
const ISOLATION_HEADERS: [(HeaderName, &str); 2] = [
    (
        HeaderName::from_static("cross-origin-opener-policy"),
        "same-origin",
    ),
    (
        HeaderName::from_static("cross-origin-embedder-policy"),
        "require-corp",
    ),
];

pub fn build_identifier(cfg: &ServerConfig) -> anyhow::Result<Arc<dyn Identifier>> {
    Ok(match cfg.identifier.kind {
        IdentifierKind::Mock => {
            tracing::warn!(
                "using the mock identifier: every loud burst is reported as \"Unknown bird\""
            );
            Arc::new(MockIdentifier {
                threshold_db: cfg.identifier.mock_threshold_db,
            })
        }
        #[cfg(feature = "birdnet")]
        IdentifierKind::Birdnet => {
            let b = &cfg.identifier.birdnet;
            for path in [&b.model_path, &b.labels_path] {
                anyhow::ensure!(
                    path.is_file(),
                    "BirdNET file {} not found. Run scripts/fetch-birdnet.sh to download the model, \
                     or set [identifier] kind = \"mock\" to run without one",
                    path.display()
                );
            }
            Arc::new(bsp_core::identifier::BirdNetIdentifier::load(b.clone())?)
        }
        #[cfg(not(feature = "birdnet"))]
        IdentifierKind::Birdnet => {
            anyhow::bail!("this build has no BirdNET support (feature `birdnet`)")
        }
    })
}

/// Loads the geo model if its files are present; without it, every species is
/// treated as expected.
pub fn build_geo(cfg: &ServerConfig) -> Option<Arc<dyn RangeModel>> {
    let g = &cfg.identifier.geo;
    if !g.model_path.is_file() || !g.labels_path.is_file() {
        tracing::warn!(
            path = %g.model_path.display(),
            "geo model not found (run scripts/fetch-birdnet.sh); unexpected species will not be flagged"
        );
        return None;
    }
    #[cfg(feature = "birdnet")]
    match bsp_core::geo::GeoModel::load(&g.model_path, &g.labels_path) {
        Ok(m) => return Some(Arc::new(m)),
        Err(e) => tracing::error!("loading geo model: {e:#}"),
    }
    None
}

fn client_api() -> Router<SharedState> {
    Router::new()
        .nest(CLIENT_API_PREFIX, api::client::router())
        .layer(DefaultBodyLimit::max(32 * 1024 * 1024))
}

/// API for client devices (served on `client_listen`).
pub fn client_router(state: SharedState) -> Router {
    client_api().with_state(state)
}

/// Web UI, browser client and control API (served on the `[web]` listeners).
/// The client API is served here too: browsers only allow microphone access
/// on HTTPS pages, and those pages cannot call the plain-HTTP client port.
pub fn web_router(state: SharedState) -> Router {
    Router::new()
        .route("/", get(|| async { Html(INDEX_HTML) }))
        .route(
            "/static/echarts-6.1.0.min.js",
            get(|| async {
                (
                    [
                        (
                            axum::http::header::CONTENT_TYPE,
                            "text/javascript; charset=utf-8",
                        ),
                        // Versioned path: the content never changes.
                        (
                            axum::http::header::CACHE_CONTROL,
                            "public, max-age=31536000, immutable",
                        ),
                    ],
                    ECHARTS_JS,
                )
            }),
        )
        .route(
            "/static/detection-samples.js",
            get(|| async {
                (
                    [(header::CONTENT_TYPE, "text/javascript; charset=utf-8")],
                    DETECTION_SAMPLES_JS,
                )
            }),
        )
        .route(
            "/client",
            get(|| async { (ISOLATION_HEADERS, Html(CLIENT_HTML)) }),
        )
        .route(
            "/client-worklet.js",
            get(|| async {
                (
                    ISOLATION_HEADERS,
                    [(header::CONTENT_TYPE, "text/javascript")],
                    CLIENT_WORKLET_JS,
                )
                    .into_response()
            }),
        )
        .nest("/api/v1/control", api::control::router())
        .merge(client_api())
        .with_state(state)
}

/// Logs a raw event whenever a client stops sending heartbeats, and keeps
/// audio long enough for batch clients' late uploads to be located.
async fn monitor_clients(state: SharedState) {
    let mut was_active: HashMap<ClientId, bool> = HashMap::new();
    let mut tick = tokio::time::interval(Duration::from_secs(2));
    loop {
        tick.tick().await;
        let Ok(clients) = state.db.list_clients() else {
            continue;
        };
        let now = now_ns();
        let hold = state.batch_hold(&clients, now);
        state
            .audio
            .set_extra_retention(hold.map_or(0, |h| h.max_wait_ns));
        for c in clients {
            let active = state.is_active(&c, now);
            if was_active.insert(c.id, active) == Some(true) && !active {
                state.event(
                    Some(c.id),
                    "inactive",
                    serde_json::json!({ "last_seen": c.last_seen }),
                );
            }
        }
    }
}

pub async fn run(cfg: ServerConfig) -> anyhow::Result<()> {
    let species_filter = Arc::new(LocalSpeciesFilter::new(
        build_identifier(&cfg)?,
        settings::Settings::default().thresholds(),
    ));
    let geo = build_geo(&cfg);
    let db = Db::open(&cfg.db_path)
        .with_context(|| format!("opening database {}", cfg.db_path.display()))?;
    let udp = UdpSocket::bind(&cfg.udp_listen)
        .await
        .with_context(|| format!("binding UDP {}", cfg.udp_listen))?;
    let client_listener = TcpListener::from_std(web::bind(&cfg.client_listen, "client API")?)?;
    let (calls_tx, calls_rx) = mpsc::unbounded_channel();
    let locator = Arc::new(TdoaLocator {
        clock_tolerance_s: 2.0 * cfg.max_clock_offset_us as f64 * 1e-6,
        ..TdoaLocator::default()
    });

    let beacon_clocks = Arc::new(beacon_clock::BeaconClocks::default());
    let state: SharedState = Arc::new(AppState {
        audio: audio_buffer::AudioBuffers::new(cfg.audio_retention_s, beacon_clocks.clone()),
        beacon_clocks,
        clock_history: Default::default(),
        udp_port: udp.local_addr()?.port(),
        started_at: now_ns(),
        positioning: Default::default(),
        calls_tx,
        identifier: species_filter.clone(),
        species_filter,
        geo,
        settings: Default::default(),
        local_species: std::sync::RwLock::new(settings::LocalSpeciesStatus::NoLocation),
        db,
        cfg,
    });
    let saved = settings::load(&state);
    settings::apply(&state, &saved, None).await;
    *state.settings.lock().await = saved;
    state.event(
        None,
        "server_start",
        serde_json::json!({ "identifier": state.identifier.name() }),
    );

    let maintenance = state.clone();
    tokio::task::spawn_blocking(move || {
        match maintenance.db.maintain(settings::ConfidenceLevel::MIN) {
            Ok(m) if m != db::Maintenance::default() => tracing::info!(
                detections_removed = m.detections_removed,
                clips_converted_to_flac = m.clips_converted,
                "database maintenance done"
            ),
            Ok(_) => {}
            Err(e) => tracing::error!("database maintenance failed: {e:#}"),
        }
    });

    tokio::spawn(timesync::run(udp));
    tokio::spawn(pipeline::run(state.clone(), calls_rx, locator));
    tokio::spawn(monitor_clients(state.clone()));

    tracing::info!(tcp = %client_listener.local_addr()?, udp = state.udp_port, "client API");
    let web_handle = axum_server::Handle::new();
    let mut web_task = tokio::spawn(web::serve(
        state.cfg.web.clone(),
        web_router(state.clone()),
        web_handle.clone(),
    ));
    let clients = axum::serve(
        client_listener,
        client_router(state).into_make_service_with_connect_info::<SocketAddr>(),
    )
    .with_graceful_shutdown(async {
        let _ = tokio::signal::ctrl_c().await;
    });
    tokio::select! {
        result = clients => {
            result.context("client API")?;
            web_handle.graceful_shutdown(Some(Duration::from_secs(5)));
            (&mut web_task).await??;
        }
        // The web listeners failing (e.g. a port they cannot bind) stops the server.
        result = &mut web_task => result??,
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use axum::body::Body;
    use axum::extract::ConnectInfo;
    use axum::http::{Request, StatusCode};
    use bsp_proto::{API_VERSION, Uuid};
    use serde_json::{Value, json};
    use tower::ServiceExt;

    fn test_state() -> SharedState {
        let species_filter = Arc::new(LocalSpeciesFilter::new(
            Arc::new(MockIdentifier { threshold_db: 10.0 }),
            settings::Settings::default().thresholds(),
        ));
        let beacon_clocks = Arc::new(beacon_clock::BeaconClocks::default());
        let (calls_tx, _calls_rx) = mpsc::unbounded_channel();
        let cfg = ServerConfig {
            firmware_dir: std::env::temp_dir().join(format!("bsp-firmware-{}", Uuid::new_v4())),
            ..ServerConfig::default()
        };
        Arc::new(AppState {
            audio: audio_buffer::AudioBuffers::new(cfg.audio_retention_s, beacon_clocks.clone()),
            beacon_clocks,
            clock_history: Default::default(),
            udp_port: 2473,
            started_at: now_ns(),
            positioning: Default::default(),
            calls_tx,
            identifier: species_filter.clone(),
            species_filter,
            geo: None,
            settings: Default::default(),
            local_species: std::sync::RwLock::new(settings::LocalSpeciesStatus::NoLocation),
            db: Db::in_memory().unwrap(),
            cfg,
        })
    }

    async fn call(
        state: &SharedState,
        method: &str,
        path: &str,
        body: Option<Value>,
    ) -> (StatusCode, Value) {
        let mut req = Request::builder()
            .method(method)
            .uri(path)
            .header("content-type", "application/json")
            .body(body.map_or_else(Body::empty, |b| Body::from(b.to_string())))
            .unwrap();
        req.extensions_mut()
            .insert(ConnectInfo("10.0.0.9:5000".parse::<SocketAddr>().unwrap()));
        let resp = web_router(state.clone()).oneshot(req).await.unwrap();
        let status = resp.status();
        let bytes = axum::body::to_bytes(resp.into_body(), usize::MAX)
            .await
            .unwrap();
        (
            status,
            serde_json::from_slice(&bytes).unwrap_or(Value::Null),
        )
    }

    fn registration(id: u128, version: &str, api_version: Option<&str>) -> Value {
        let mut r = json!({
            "client_id": Uuid::from_u128(id), "hostname": "h", "version": version,
            "capabilities": { "sample_rate": 48000, "channels": 1, "sample_format": "I32",
                              "clock_source": "c", "device_name": "d" },
        });
        if let Some(v) = api_version {
            r["api_version"] = v.into();
        }
        r
    }

    async fn register(
        state: &SharedState,
        id: u128,
        api_version: Option<&str>,
    ) -> (StatusCode, Value) {
        let body = registration(id, "bps-mcu abc1234", api_version);
        call(state, "POST", "/api/v1/client/register", Some(body)).await
    }

    /// The client as the control API lists it.
    async fn listed(state: &SharedState, id: u128) -> Value {
        let (status, c) = call(
            state,
            "GET",
            &format!("/api/v1/control/clients/{}", Uuid::from_u128(id)),
            None,
        )
        .await;
        assert_eq!(status, StatusCode::OK);
        c
    }

    #[tokio::test]
    async fn registers_clients_of_the_same_major_api_version() {
        let state = test_state();
        let (status, reply) = register(&state, 1, Some(API_VERSION)).await;
        assert_eq!(status, StatusCode::OK);
        assert_eq!(reply["api_version"], API_VERSION);
        let c = listed(&state, 1).await;
        assert_eq!(c["version"], "bps-mcu abc1234");
        assert_eq!(c["api_version"], API_VERSION);
        assert_eq!(c["api_compat"], "compatible");

        // A newer minor is accepted, but flagged.
        let (major, _) = API_VERSION.split_once('.').unwrap();
        let (status, _) = register(&state, 2, Some(&format!("{major}.99.0"))).await;
        assert_eq!(status, StatusCode::OK);
        assert_eq!(listed(&state, 2).await["api_compat"], "client_newer");
    }

    #[tokio::test]
    async fn registers_old_firmware_without_an_api_version_as_legacy() {
        let state = test_state();
        let (status, reply) = register(&state, 1, None).await;
        assert_eq!(status, StatusCode::OK);
        assert_eq!(reply["api_version"], API_VERSION);
        let c = listed(&state, 1).await;
        assert_eq!(c["api_version"], Value::Null);
        assert_eq!(c["api_compat"], "legacy");
    }

    #[tokio::test]
    async fn refuses_another_major_api_version_and_says_why() {
        let state = test_state();
        let (status, body) = register(&state, 1, Some("9.0.0")).await;
        assert_eq!(status, StatusCode::CONFLICT);
        let msg = body["error"].as_str().unwrap();
        assert!(msg.contains("9.0.0") && msg.contains(API_VERSION), "{msg}");
        // The Clients page still lists it, flagged.
        let c = listed(&state, 1).await;
        assert_eq!(c["api_compat"], "incompatible");
        assert_eq!(c["version"], "bps-mcu abc1234");
        // Retries do not flood the log.
        register(&state, 1, Some("9.0.0")).await;
        let (_, events) = call(&state, "GET", "/api/v1/control/events?limit=50", None).await;
        let refused = events
            .as_array()
            .unwrap()
            .iter()
            .filter(|e| e["kind"] == "register_refused")
            .count();
        assert_eq!(refused, 1);
        // Once updated, it registers.
        let (status, _) = register(&state, 1, Some(API_VERSION)).await;
        assert_eq!(status, StatusCode::OK);
        assert_eq!(listed(&state, 1).await["api_compat"], "compatible");
    }

    #[tokio::test]
    async fn refuses_a_malformed_api_version() {
        let state = test_state();
        let (status, body) = register(&state, 1, Some("latest")).await;
        assert_eq!(status, StatusCode::BAD_REQUEST);
        assert!(body["error"].as_str().unwrap().contains("latest"));
    }

    #[tokio::test]
    async fn status_reports_the_servers_versions() {
        let (status, s) = call(&test_state(), "GET", "/api/v1/control/status", None).await;
        assert_eq!(status, StatusCode::OK);
        assert_eq!(s["api_version"], API_VERSION);
        assert_eq!(s["version"], env!("CARGO_PKG_VERSION"));
        assert_eq!(s["build"], BUILD);
    }

    /// A heartbeat from client `id`, with `extra` fields; the reply.
    async fn beat(state: &SharedState, id: u128, extra: Value) -> Value {
        let mut hb = json!({ "clock": null, "uptime_s": 30, "chunks_sent": 0, "chunks_gated": 0,
                             "queue_len": 0, "capture_dropouts": 0 });
        for (k, v) in extra.as_object().unwrap() {
            hb[k] = v.clone();
        }
        let path = format!("/api/v1/client/{}/heartbeat", Uuid::from_u128(id));
        let (status, reply) = call(state, "POST", &path, Some(hb)).await;
        assert_eq!(status, StatusCode::OK, "{reply}");
        reply
    }

    async fn control(
        state: &SharedState,
        method: &str,
        id: u128,
        what: &str,
        body: Option<Value>,
    ) -> (StatusCode, Value) {
        let path = format!("/api/v1/control/clients/{}/{what}", Uuid::from_u128(id));
        call(state, method, &path, body).await
    }

    async fn events_of(state: &SharedState, kind: &str) -> Vec<Value> {
        let (_, evs) = call(
            state,
            "GET",
            &format!("/api/v1/control/events?kind={kind}&limit=500"),
            None,
        )
        .await;
        evs.as_array().unwrap().clone()
    }

    #[tokio::test]
    async fn a_reboot_is_delivered_until_acknowledged() {
        let state = test_state();
        register(&state, 1, Some(API_VERSION)).await;
        let (status, cmd) = control(&state, "POST", 1, "reboot", None).await;
        assert_eq!(status, StatusCode::ACCEPTED);
        let id = cmd["id"].as_i64().unwrap();
        // Ids follow the clock (ms), so they keep increasing across database resets.
        assert!((id - now_ns() / 1_000_000).abs() < 60_000, "{id}");
        // Asking again while it is pending gives the same command.
        let (status, again) = control(&state, "POST", 1, "reboot", None).await;
        assert_eq!((status, again["id"].as_i64()), (StatusCode::OK, Some(id)));
        assert_eq!(
            listed(&state, 1).await["maintenance"]["reboot"]["result"],
            Value::Null
        );

        // Every heartbeat reply carries it until the client answers.
        for _ in 0..2 {
            let reply = beat(&state, 1, json!({})).await;
            assert_eq!(
                reply["commands"],
                json!([{ "command_id": id, "command": "reboot" }])
            );
        }
        let m = &listed(&state, 1).await["maintenance"];
        assert_eq!(m["supported"], true);
        assert!(m["reboot"]["delivered_at"].is_i64());
        assert_eq!(m["reboot"]["result"], Value::Null);

        // The client restarts, and says so in its first heartbeat after.
        let ack = json!({ "acks": [{ "command_id": id, "result": "done" }],
                          "reboot_reason": "requested", "uptime_s": 4 });
        let reply = beat(&state, 1, ack.clone()).await;
        assert!(reply.get("commands").is_none(), "{reply}");
        let c = listed(&state, 1).await;
        assert_eq!(c["maintenance"]["reboot"]["result"], "done");
        assert!(c["maintenance"]["reboot"]["acked_at"].is_i64());
        assert_eq!(c["last_reboot"]["reason"], "requested");
        // An answer repeated (its reply was lost) changes nothing, nor is logged twice.
        beat(&state, 1, ack).await;
        assert_eq!(events_of(&state, "command_acked").await.len(), 1);
        assert_eq!(events_of(&state, "client_rebooted").await.len(), 1);
        assert_eq!(events_of(&state, "reboot_requested").await.len(), 1);
        // A new reboot gets a higher id.
        let (status, next) = control(&state, "POST", 1, "reboot", None).await;
        assert_eq!(status, StatusCode::ACCEPTED);
        assert!(next["id"].as_i64().unwrap() > id);
        // Answers from another client do not count.
        register(&state, 2, Some(API_VERSION)).await;
        beat(
            &state,
            2,
            json!({ "acks": [{ "command_id": next["id"], "result": "done" }] }),
        )
        .await;
        assert_eq!(
            listed(&state, 1).await["maintenance"]["reboot"]["result"],
            Value::Null
        );
        // Unknown clients.
        let (status, _) = control(&state, "POST", 9, "reboot", None).await;
        assert_eq!(status, StatusCode::NOT_FOUND);
    }

    #[tokio::test]
    async fn clients_without_commands_are_never_sent_any() {
        let state = test_state();
        register(&state, 1, None).await; // old firmware
        register(&state, 2, Some("1.0.0")).await;
        for id in [1, 2] {
            let (status, body) = control(&state, "POST", id, "reboot", None).await;
            assert_eq!(status, StatusCode::CONFLICT);
            assert!(
                body["error"]
                    .as_str()
                    .unwrap()
                    .contains("not supported by this client's firmware")
            );
            let on = json!({ "level": "on", "duration_s": 60 });
            let (status, _) = control(&state, "PUT", id, "logging", Some(on)).await;
            assert_eq!(status, StatusCode::CONFLICT);
            assert_eq!(listed(&state, id).await["maintenance"]["supported"], false);
            // Even one queued somehow stays with the server.
            state
                .db
                .queue_command(Uuid::from_u128(id), "reboot", &json!({}), now_ns())
                .unwrap();
            let reply = beat(&state, id, json!({})).await;
            assert!(reply.get("commands").is_none(), "{reply}");
        }
    }

    #[tokio::test]
    async fn trace_logging_shows_as_active_until_it_expires() {
        let state = test_state();
        register(&state, 1, Some(API_VERSION)).await;
        for bad in [
            json!({ "level": "on" }),
            json!({ "level": "on", "duration_s": 0 }),
            json!({ "level": "on", "duration_s": 86_401 }),
            json!({ "level": "trace", "duration_s": 60 }),
        ] {
            let (status, _) = control(&state, "PUT", 1, "logging", Some(bad.clone())).await;
            assert_eq!(status, StatusCode::BAD_REQUEST, "{bad}");
        }
        let (status, first) = control(
            &state,
            "PUT",
            1,
            "logging",
            Some(json!({ "level": "on", "duration_s": 60 })),
        )
        .await;
        assert_eq!(status, StatusCode::ACCEPTED);
        // A second setting takes the place of the first, still pending.
        let (_, second) = control(
            &state,
            "PUT",
            1,
            "logging",
            Some(json!({ "level": "on", "duration_s": 600 })),
        )
        .await;
        let reply = beat(&state, 1, json!({})).await;
        assert_eq!(
            reply["commands"],
            json!([{ "command_id": second["id"], "command": "logging",
                                               "log_level": "on", "log_duration_s": 600 }])
        );
        assert!(second["id"].as_i64() > first["id"].as_i64());
        let m = listed(&state, 1).await["maintenance"].clone();
        assert_eq!(m["logging"]["id"], second["id"]);
        assert_eq!(m["logging_until"], Value::Null); // not on yet

        // On, as the client reports it: active until it turns itself off.
        let on = json!({ "acks": [{ "command_id": second["id"], "result": "done" }],
                         "log": { "level": "on", "remaining_s": 598, "now_ms": 1000 } });
        let t = now_ns();
        beat(&state, 1, on).await;
        let until = listed(&state, 1).await["maintenance"]["logging_until"]
            .as_i64()
            .unwrap();
        assert!((until - (t + 598 * NANOS)).abs() < 5 * NANOS, "{until}");
        // A report from long ago has expired by now.
        let c = state.db.get_client(Uuid::from_u128(1)).unwrap().unwrap();
        let mut old = c.health.clone().unwrap();
        old.at -= 600 * NANOS;
        state.db.record_heartbeat(c.id, "a", None, &old).unwrap();
        assert_eq!(
            listed(&state, 1).await["maintenance"]["logging_until"],
            Value::Null
        );
        // And off as soon as the client says so.
        beat(
            &state,
            1,
            json!({ "log": { "level": "on", "remaining_s": 30, "now_ms": 1 } }),
        )
        .await;
        assert!(listed(&state, 1).await["maintenance"]["logging_until"].is_i64());
        beat(&state, 1, json!({ "log": { "level": "off", "now_ms": 2 } })).await;
        assert_eq!(
            listed(&state, 1).await["maintenance"]["logging_until"],
            Value::Null
        );
        // Turning it off needs no duration.
        let (status, _) =
            control(&state, "PUT", 1, "logging", Some(json!({ "level": "off" }))).await;
        assert_eq!(status, StatusCode::ACCEPTED);
    }

    const NANOS: i64 = bsp_proto::NANOS_PER_SEC;

    #[tokio::test]
    async fn log_lines_are_kept_as_events_at_the_time_they_were_logged() {
        let state = test_state();
        register(&state, 1, Some(API_VERSION)).await;
        let t = now_ns();
        beat(&state, 1, json!({ "log": { "level": "on", "remaining_s": 60, "now_ms": 100_000,
                                         "dropped": 3,
                                         "lines": [[40_000, "wifi: connected"], [99_000, "a \"line\""]] } })).await;
        let logs = events_of(&state, "client_log").await;
        assert_eq!(logs.len(), 2);
        // Newest first; each at the client's time on the server's clock.
        assert_eq!(logs[0]["detail"]["text"], "a \"line\"");
        assert_eq!(logs[1]["detail"]["text"], "wifi: connected");
        assert_eq!(logs[1]["client_id"], Uuid::from_u128(1).to_string());
        let ts = logs[1]["ts"].as_i64().unwrap();
        assert!((ts - (t - 60 * NANOS)).abs() < 2 * NANOS, "{}", ts - t);
        assert_eq!(
            events_of(&state, "client_log_dropped").await[0]["detail"]["count"],
            3
        );
        // Filtered by client, as the Logs page does.
        let (_, evs) = call(
            &state,
            "GET",
            &format!(
                "/api/v1/control/events?client_id={}&kind=client_log",
                Uuid::from_u128(1)
            ),
            None,
        )
        .await;
        assert_eq!(evs.as_array().unwrap().len(), 2);
        // The heartbeat event keeps a count, not the lines again.
        let hb = &events_of(&state, "heartbeat").await[0]["detail"]["log"];
        assert_eq!((hb.get("lines"), &hb["line_count"]), (None, &json!(2)));
    }

    #[tokio::test]
    async fn restarts_are_kept_and_logged_once_each() {
        let state = test_state();
        register(&state, 1, Some(API_VERSION)).await;
        let t = now_ns();
        beat(
            &state,
            1,
            json!({ "reboot_reason": "power_on", "uptime_s": 20 }),
        )
        .await;
        // The same report again (the reply to the first was lost).
        beat(
            &state,
            1,
            json!({ "reboot_reason": "power_on", "uptime_s": 25 }),
        )
        .await;
        let c = listed(&state, 1).await;
        assert_eq!(c["last_reboot"]["reason"], "power_on");
        let at = c["last_reboot"]["at"].as_i64().unwrap();
        assert!((at - (t - 20 * NANOS)).abs() < 2 * NANOS);
        beat(
            &state,
            1,
            json!({ "reboot_reason": "watchdog", "uptime_s": 2 }),
        )
        .await;
        let evs = events_of(&state, "client_rebooted").await;
        assert_eq!(evs.len(), 2);
        assert_eq!(evs[0]["detail"]["reason"], "watchdog");
        assert_eq!(listed(&state, 1).await["last_reboot"]["reason"], "watchdog");
    }

    /// A request with a raw body; the status, headers and body.
    async fn call_raw(
        state: &SharedState,
        method: &str,
        path: &str,
        content_type: &str,
        body: Vec<u8>,
    ) -> (StatusCode, axum::http::HeaderMap, Vec<u8>) {
        let mut req = Request::builder()
            .method(method)
            .uri(path)
            .header("content-type", content_type)
            .body(Body::from(body))
            .unwrap();
        req.extensions_mut()
            .insert(ConnectInfo("10.0.0.9:5000".parse::<SocketAddr>().unwrap()));
        let resp = web_router(state.clone()).oneshot(req).await.unwrap();
        let (parts, body) = resp.into_parts();
        let bytes = axum::body::to_bytes(body, usize::MAX).await.unwrap();
        (parts.status, parts.headers, bytes.to_vec())
    }

    /// Uploads a synthetic image of this build; the status and the build as listed.
    async fn upload(
        state: &SharedState,
        hw: &str,
        tag: Option<&str>,
        commit: &str,
        built: &str,
    ) -> (StatusCode, Value, Vec<u8>) {
        let img =
            firmware::tests::image(&firmware::tests::info_json(hw, tag, commit, false, built));
        let (status, _, body) = call_raw(
            state,
            "POST",
            "/api/v1/control/firmware",
            "application/octet-stream",
            img.clone(),
        )
        .await;
        (
            status,
            serde_json::from_slice(&body).unwrap_or(Value::Null),
            img,
        )
    }

    /// The registration's `build` for this build (see [`upload`]).
    fn build_json(hw: &str, tag: Option<&str>, commit: &str, built: &str) -> Value {
        json!({ "hardware_id": hw, "tag": tag, "commit": commit, "dirty": false,
                "describe": format!("v1.0.0-{commit}"), "built_at": built })
    }

    async fn register_ota(
        state: &SharedState,
        id: u128,
        api_version: &str,
        build: Value,
        ota: bool,
    ) {
        let mut body = registration(id, "bps-mcu x", Some(api_version));
        body["build"] = build;
        if ota {
            body["ota"] = true.into();
        }
        let (status, reply) = call(state, "POST", "/api/v1/client/register", Some(body)).await;
        assert_eq!(status, StatusCode::OK, "{reply}");
    }

    const T0: &str = "2026-01-01T00:00:00Z";
    const T1: &str = "2026-03-01T00:00:00Z";
    const T2: &str = "2026-02-01T00:00:00Z";
    const HW: &str = "esp32-s3 on xiao-proto-rev0.1";

    #[tokio::test]
    async fn firmware_builds_are_uploaded_listed_downloaded_and_deleted() {
        let state = test_state();
        let (status, b0, img) = upload(&state, HW, None, "aaa1111", T0).await;
        assert_eq!(status, StatusCode::CREATED, "{b0}");
        let id = b0["id"].as_i64().unwrap();
        assert_eq!(b0["hardware_id"], HW);
        assert_eq!(b0["commit"], "aaa1111");
        assert_eq!(b0["built_at"], T0);
        assert_eq!(b0["stable"], false);
        assert_eq!(b0["size"], img.len());
        assert_eq!(b0["md5"], firmware::md5_hex(&img));
        assert_eq!(b0["url"], format!("/api/v1/client/firmware/{id}.bin"));
        // The same image again changes nothing.
        let (status, again, _) = upload(&state, HW, None, "aaa1111", T0).await;
        assert_eq!((status, &again["id"]), (StatusCode::OK, &b0["id"]));
        // Newest build time first.
        let (status, b1, _) = upload(&state, HW, Some("v1.1.0"), "bbb2222", T1).await;
        assert_eq!((status, &b1["stable"]), (StatusCode::CREATED, &json!(true)));
        let (_, list) = call(&state, "GET", "/api/v1/control/firmware", None).await;
        let ids: Vec<_> = list
            .as_array()
            .unwrap()
            .iter()
            .map(|b| b["id"].clone())
            .collect();
        assert_eq!(ids, [b1["id"].clone(), b0["id"].clone()]);

        // Refused: not an image, no build information, a form upload.
        let (status, _, body) = call_raw(
            &state,
            "POST",
            "/api/v1/control/firmware",
            "application/octet-stream",
            b"hello".to_vec(),
        )
        .await;
        assert_eq!(status, StatusCode::UNPROCESSABLE_ENTITY);
        assert!(String::from_utf8_lossy(&body).contains("not an ESP32"));
        let mut bare = img.clone();
        let at = bare
            .windows(15)
            .position(|w| w == bsp_proto::BUILD_INFO_MARKER)
            .unwrap();
        bare[at] = b'x';
        let (status, _, body) = call_raw(
            &state,
            "POST",
            "/api/v1/control/firmware",
            "application/octet-stream",
            bare,
        )
        .await;
        assert_eq!(status, StatusCode::UNPROCESSABLE_ENTITY);
        assert!(String::from_utf8_lossy(&body).contains("no build information"));
        let (status, _, _) = call_raw(
            &state,
            "POST",
            "/api/v1/control/firmware",
            "multipart/form-data; boundary=x",
            img.clone(),
        )
        .await;
        assert_eq!(status, StatusCode::UNSUPPORTED_MEDIA_TYPE);

        // Downloaded from the client port.
        let req = Request::get(format!("/api/v1/client/firmware/{id}.bin"))
            .body(Body::empty())
            .unwrap();
        let resp = client_router(state.clone()).oneshot(req).await.unwrap();
        assert_eq!(resp.status(), StatusCode::OK);
        let h = resp.headers().clone();
        assert_eq!(h["content-type"], "application/octet-stream");
        assert_eq!(h["content-length"], img.len().to_string().as_str());
        assert_eq!(h["x-md5"], firmware::md5_hex(&img).as_str());
        let body = axum::body::to_bytes(resp.into_body(), usize::MAX)
            .await
            .unwrap();
        assert_eq!(&body[..], &img[..]);
        for missing in ["999.bin", "0.bin", "abc.bin", &format!("{id}")] {
            let (status, _, _) = call_raw(
                &state,
                "GET",
                &format!("/api/v1/client/firmware/{missing}"),
                "",
                Vec::new(),
            )
            .await;
            assert_eq!(status, StatusCode::NOT_FOUND, "{missing}");
        }

        // A build a client is pinned to stays.
        register_ota(
            &state,
            1,
            API_VERSION,
            build_json(HW, None, "aaa1111", T0),
            true,
        )
        .await;
        let pin = json!({ "policy": "manual", "build_id": id });
        let (status, _) = control(&state, "PUT", 1, "firmware", Some(pin)).await;
        assert_eq!(status, StatusCode::OK);
        let path = format!("/api/v1/control/firmware/{id}");
        let (status, body) = call(&state, "DELETE", &path, None).await;
        assert_eq!(status, StatusCode::CONFLICT);
        assert!(body["error"].as_str().unwrap().contains("manual choice"));
        let dev = json!({ "policy": "latest_dev" });
        control(&state, "PUT", 1, "firmware", Some(dev)).await;
        let (status, _) = call(&state, "DELETE", &path, None).await;
        assert_eq!(status, StatusCode::NO_CONTENT);
        let (status, _) = call(&state, "DELETE", &path, None).await;
        assert_eq!(status, StatusCode::NOT_FOUND);
        let (status, _, _) = call_raw(
            &state,
            "GET",
            &format!("/api/v1/client/firmware/{id}.bin"),
            "",
            Vec::new(),
        )
        .await;
        assert_eq!(status, StatusCode::NOT_FOUND);
        assert_eq!(events_of(&state, "firmware_uploaded").await.len(), 2);
        assert_eq!(events_of(&state, "firmware_deleted").await.len(), 1);
    }

    #[tokio::test]
    async fn clients_are_offered_the_build_chosen_for_them() {
        let state = test_state();
        let running = build_json(HW, None, "aaa1111", T0);
        register_ota(&state, 1, API_VERSION, running.clone(), true).await;
        let (_, b0, _) = upload(&state, HW, None, "aaa1111", T0).await;
        let (_, b1, img1) = upload(&state, HW, None, "bbb2222", T1).await;
        let (_, b2, _) = upload(&state, HW, Some("v1.2.0"), "ccc3333", T2).await;
        let (_, other_hw, _) = upload(&state, "esp32 on devkit", None, "ddd4444", T1).await;

        // Until a build is chosen, the client stays on its own.
        let c = listed(&state, 1).await;
        assert_eq!(c["build"], running);
        assert_eq!(c["ota"], true);
        let fw = &c["firmware"];
        assert_eq!(fw["supported"], true);
        assert_eq!(fw["policy"], Value::Null);
        assert_eq!(fw["running"]["id"], b0["id"]);
        assert_eq!(fw["target"]["id"], b0["id"]);
        assert!(
            beat(&state, 1, json!({}))
                .await
                .get("firmware_offer")
                .is_none()
        );

        // Latest dev: the newest build.
        let set = |p: Value| control(&state, "PUT", 1, "firmware", Some(p));
        assert_eq!(
            set(json!({ "policy": "latest_dev" })).await.0,
            StatusCode::OK
        );
        for _ in 0..2 {
            let reply = beat(&state, 1, json!({})).await;
            let id = b1["id"].as_i64().unwrap();
            assert_eq!(
                reply["firmware_offer"],
                json!({ "firmware_build_id": id,
                        "firmware_url": format!("/api/v1/client/firmware/{id}.bin"),
                        "firmware_size": img1.len(), "firmware_md5": firmware::md5_hex(&img1),
                        "firmware_describe": "v1.0.0-bbb2222" })
            );
        }
        assert_eq!(events_of(&state, "firmware_offered").await.len(), 1);
        assert_eq!(listed(&state, 1).await["firmware"]["offering"], b1["id"]);
        // Latest stable; and a newer stable upload moves it along.
        set(json!({ "policy": "latest_stable" })).await;
        assert_eq!(
            beat(&state, 1, json!({})).await["firmware_offer"]["firmware_build_id"],
            b2["id"]
        );
        let (_, b3, _) = upload(
            &state,
            HW,
            Some("v1.3.0"),
            "eee5555",
            "2026-04-01T00:00:00Z",
        )
        .await;
        assert_eq!(
            beat(&state, 1, json!({})).await["firmware_offer"]["firmware_build_id"],
            b3["id"]
        );
        // Manual: only builds for its hardware.
        let (status, _) = set(json!({ "policy": "manual", "build_id": other_hw["id"] })).await;
        assert_eq!(status, StatusCode::BAD_REQUEST);
        let (status, _) = set(json!({ "policy": "manual", "build_id": 999 })).await;
        assert_eq!(status, StatusCode::NOT_FOUND);
        set(json!({ "policy": "manual", "build_id": b2["id"] })).await;
        assert_eq!(
            beat(&state, 1, json!({})).await["firmware_offer"]["firmware_build_id"],
            b2["id"]
        );

        // Deferred: offered still.
        let st = |state: &str| {
            json!({ "firmware_status": { "build_id": b2["id"], "state": state,
                                                            "detail": "battery 34%" } })
        };
        for _ in 0..2 {
            let reply = beat(&state, 1, st("deferred")).await;
            assert_eq!(reply["firmware_offer"]["firmware_build_id"], b2["id"]);
        }
        assert_eq!(events_of(&state, "firmware_update_deferred").await.len(), 1);
        assert_eq!(
            listed(&state, 1).await["firmware"]["last"]["result"],
            "deferred"
        );
        // Failed: not offered again, until the choice is set again.
        assert!(
            beat(&state, 1, st("failed"))
                .await
                .get("firmware_offer")
                .is_none()
        );
        assert!(
            beat(&state, 1, st("failed"))
                .await
                .get("firmware_offer")
                .is_none()
        );
        assert!(
            beat(&state, 1, json!({}))
                .await
                .get("firmware_offer")
                .is_none()
        );
        let failed = events_of(&state, "firmware_update_failed").await;
        assert_eq!(failed.len(), 1);
        assert_eq!(failed[0]["detail"]["detail"], "battery 34%");
        let fw = listed(&state, 1).await["firmware"].clone();
        assert_eq!(
            (&fw["last"]["result"], &fw["blocked"]),
            (&json!("failed"), &json!([b2["id"]]))
        );
        assert_eq!(fw["offering"], Value::Null);
        // Latest dev targets another build, so it is offered.
        set(json!({ "policy": "latest_dev" })).await;
        assert_eq!(
            beat(&state, 1, json!({})).await["firmware_offer"]["firmware_build_id"],
            b3["id"]
        );
        // A rollback is a failure too.
        let rb = json!({ "firmware_status": { "build_id": b3["id"], "state": "rolled_back" } });
        assert!(beat(&state, 1, rb).await.get("firmware_offer").is_none());
        let failed = events_of(&state, "firmware_update_failed").await;
        assert_eq!(failed[0]["detail"]["detail"], "rolled back");
        // Unknown states are ignored.
        let odd = json!({ "firmware_status": { "build_id": 1, "state": "on_fire" } });
        assert!(beat(&state, 1, odd).await.get("firmware_offer").is_none());
        // Back to the failed build by choosing it again; it installs.
        set(json!({ "policy": "manual", "build_id": b2["id"] })).await;
        assert_eq!(
            beat(&state, 1, json!({})).await["firmware_offer"]["firmware_build_id"],
            b2["id"]
        );
        register_ota(
            &state,
            1,
            API_VERSION,
            build_json(HW, Some("v1.2.0"), "ccc3333", T2),
            true,
        )
        .await;
        let updated = events_of(&state, "firmware_updated").await;
        assert_eq!(updated.len(), 1);
        assert_eq!(updated[0]["detail"]["build_id"], b2["id"]);
        assert!(
            beat(&state, 1, json!({}))
                .await
                .get("firmware_offer")
                .is_none()
        );
        let fw = listed(&state, 1).await["firmware"].clone();
        assert_eq!(
            (&fw["running"]["id"], &fw["last"]["result"]),
            (&b2["id"], &json!("updated"))
        );
    }

    #[tokio::test]
    async fn only_clients_that_take_updates_are_offered_them() {
        let state = test_state();
        let (_, b0, _) = upload(&state, HW, None, "aaa1111", T0).await;
        let (_, b1, _) = upload(&state, HW, None, "bbb2222", T1).await;
        let running = build_json(HW, None, "aaa1111", T0);
        register_ota(&state, 1, "1.1.0", running.clone(), true).await; // too old an API
        register_ota(&state, 2, API_VERSION, running.clone(), false).await; // no ota
        register_ota(
            &state,
            3,
            API_VERSION,
            build_json("other", None, "x", T0),
            true,
        )
        .await;
        register_ota(
            &state,
            4,
            API_VERSION,
            build_json(HW, None, "bbb2222", T1),
            true,
        )
        .await;
        register(&state, 5, Some(API_VERSION)).await; // no build
        for id in [1, 2, 5] {
            let fw = listed(&state, id).await["firmware"].clone();
            assert_eq!(fw["supported"], false);
            assert!(fw["unsupported"].is_string());
            let (status, _) = control(
                &state,
                "PUT",
                id,
                "firmware",
                Some(json!({ "policy": "latest_dev" })),
            )
            .await;
            assert_eq!(status, StatusCode::CONFLICT);
        }
        // Even with a build chosen somehow, none is offered to them.
        let pin = firmware::FirmwarePolicy::Manual {
            build_id: b1["id"].as_i64().unwrap(),
        };
        for id in [1, 2, 3, 4, 5] {
            let fs = firmware::FirmwareState::default();
            state
                .db
                .set_firmware_policy(Uuid::from_u128(id), Some(&pin), &fs)
                .unwrap();
            let reply = beat(&state, id, json!({})).await;
            assert!(reply.get("firmware_offer").is_none(), "{id}: {reply}");
        }
        // Client 4 runs it already; the list says so.
        let (_, list) = call(&state, "GET", "/api/v1/control/firmware", None).await;
        let b1v = list
            .as_array()
            .unwrap()
            .iter()
            .find(|b| b["id"] == b1["id"])
            .unwrap();
        assert_eq!(b1v["clients_running"], json!([Uuid::from_u128(4)]));
        assert_eq!(b1v["clients_pinned"].as_array().unwrap().len(), 5);
        let b0v = list
            .as_array()
            .unwrap()
            .iter()
            .find(|b| b["id"] == b0["id"])
            .unwrap();
        assert_eq!(b0v["clients_running"].as_array().unwrap().len(), 2);
    }

    /// The browser client's copy of the API version is the same.
    #[test]
    fn browser_client_speaks_the_same_api_version() {
        assert!(CLIENT_HTML.contains(&format!("const API_VERSION = \"{API_VERSION}\";")));
    }

    /// The web UI's own unit tests (`web/*.test.js`), when node is installed.
    #[test]
    fn web_ui_js_tests() {
        let web = concat!(env!("CARGO_MANIFEST_DIR"), "/web");
        let out = match std::process::Command::new("node")
            .args(["--test", "detection-samples.test.js"])
            .current_dir(web)
            .output()
        {
            Ok(out) => out,
            Err(e) => {
                eprintln!("skipping the web UI's tests: cannot run node: {e}");
                return;
            }
        };
        assert!(
            out.status.success(),
            "web UI tests failed:\n{}{}",
            String::from_utf8_lossy(&out.stdout),
            String::from_utf8_lossy(&out.stderr)
        );
    }
}
