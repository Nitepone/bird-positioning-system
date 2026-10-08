//! bsp server: receives audio from clients, identifies calls, groups them into
//! detections, locates them, and serves the control API + web UI.

pub mod api;
pub mod audio_buffer;
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
use axum::response::Html;
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

const INDEX_HTML: &str = include_str!("../web/index.html");

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

/// API for client devices (served on `client_listen`).
pub fn client_router(state: SharedState) -> Router {
    Router::new()
        .nest(CLIENT_API_PREFIX, api::client::router())
        .layer(DefaultBodyLimit::max(32 * 1024 * 1024))
        .with_state(state)
}

/// Web UI and control API (served on the `[web]` listeners).
pub fn web_router(state: SharedState) -> Router {
    Router::new()
        .route("/", get(|| async { Html(INDEX_HTML) }))
        .nest("/api/v1/control", api::control::router())
        .with_state(state)
}

/// Logs a raw event whenever a client stops sending heartbeats.
async fn monitor_clients(state: SharedState) {
    let mut was_active: HashMap<ClientId, bool> = HashMap::new();
    let mut tick = tokio::time::interval(Duration::from_secs(2));
    loop {
        tick.tick().await;
        let Ok(clients) = state.db.list_clients() else {
            continue;
        };
        let now = now_ns();
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

    let state: SharedState = Arc::new(AppState {
        audio: audio_buffer::AudioBuffers::new(cfg.audio_retention_s),
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
