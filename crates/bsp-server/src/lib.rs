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
        let cfg = ServerConfig::default();
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
