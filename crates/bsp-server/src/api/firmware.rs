//! Control endpoints for firmware updates over the air (API 1.2.0): the
//! firmware builds the server keeps, and the build chosen for each client.
//! The clients download builds from the client API (`/firmware/{id}.bin`).

use super::{ApiError, ApiResult};
use crate::db::ClientRecord;
use crate::firmware::{self, FirmwareBuild, FirmwarePolicy, LastUpdate};
use crate::state::SharedState;
use axum::Json;
use axum::Router;
use axum::body::Bytes;
use axum::extract::{DefaultBodyLimit, Path, State};
use axum::http::{HeaderMap, StatusCode, header};
use axum::routing::{delete, get, put};
use bsp_proto::{ClientId, firmware_path, now_ns};
use serde::Serialize;
use serde_json::json;

/// Largest firmware image taken (an ESP32 app partition is smaller).
const MAX_IMAGE_BYTES: usize = 16 * 1024 * 1024;

pub fn router() -> Router<SharedState> {
    Router::new()
        .route(
            "/firmware",
            get(list_builds)
                .post(upload_build)
                .layer(DefaultBodyLimit::max(MAX_IMAGE_BYTES)),
        )
        .route("/firmware/{id}", delete(delete_build))
        .route("/clients/{id}/firmware", put(set_client_policy))
}

/// A build as listed, with the clients that run it or are to update to it.
#[derive(Serialize)]
pub struct BuildView {
    #[serde(flatten)]
    build: FirmwareBuild,
    /// Tagged and built from a clean tree.
    stable: bool,
    /// Where clients download it, on the client port.
    url: String,
    clients_running: Vec<ClientId>,
    /// Clients this is the target of, that run another build: offered it at
    /// their heartbeats (unless it failed on them).
    clients_targeting: Vec<ClientId>,
    /// Clients the operator pinned to it: it cannot be deleted meanwhile.
    clients_pinned: Vec<ClientId>,
}

fn build_view(b: FirmwareBuild, all: &[FirmwareBuild], clients: &[ClientRecord]) -> BuildView {
    let mut v = BuildView {
        stable: b.is_stable(),
        url: firmware_path(b.id),
        clients_running: Vec::new(),
        clients_targeting: Vec::new(),
        clients_pinned: Vec::new(),
        build: b,
    };
    for c in clients {
        let running = c.build.as_ref().is_some_and(|r| v.build.is(r));
        if running {
            v.clients_running.push(c.id);
        }
        let target = firmware::target(c.firmware_policy, c.build.as_ref(), all);
        if !running && target.is_some_and(|t| t.id == v.build.id) {
            v.clients_targeting.push(c.id);
        }
        if c.firmware_policy
            == Some(FirmwarePolicy::Manual {
                build_id: v.build.id,
            })
        {
            v.clients_pinned.push(c.id);
        }
    }
    v
}

/// `GET /firmware`: every build, newest build time first.
async fn list_builds(State(state): State<SharedState>) -> ApiResult<Json<Vec<BuildView>>> {
    let builds = state.db.list_firmware_builds()?;
    let clients = state.db.list_clients()?;
    Ok(Json(
        builds
            .iter()
            .cloned()
            .map(|b| build_view(b, &builds, &clients))
            .collect(),
    ))
}

/// `POST /firmware`: a firmware image (`firmware.bin`) as the raw request
/// body. Everything about it is read from the image. `201` with the new
/// build; `200` with the existing one when the same image is there already.
async fn upload_build(
    State(state): State<SharedState>,
    headers: HeaderMap,
    body: Bytes,
) -> ApiResult<(StatusCode, Json<BuildView>)> {
    let multipart = headers
        .get(header::CONTENT_TYPE)
        .and_then(|v| v.to_str().ok())
        .is_some_and(|v| v.starts_with("multipart/"));
    if multipart {
        return Err(ApiError(
            StatusCode::UNSUPPORTED_MEDIA_TYPE,
            "send the image as the raw request body, e.g. curl --data-binary @firmware.bin \
             (not a form upload)"
                .into(),
        ));
    }
    if body.is_empty() {
        return Err(ApiError::bad_request("no image: the request body is empty"));
    }
    let info = firmware::read_build_info(&body)
        .map_err(|e| ApiError(StatusCode::UNPROCESSABLE_ENTITY, format!("refused: {e}")))?;
    let md5 = firmware::md5_hex(&body);
    let respond = |state: &SharedState, status, b: FirmwareBuild| -> ApiResult<_> {
        let builds = state.db.list_firmware_builds()?;
        let clients = state.db.list_clients()?;
        Ok((status, Json(build_view(b, &builds, &clients))))
    };
    if let Some(b) = state.db.firmware_build_by_md5(&md5)? {
        return respond(&state, StatusCode::OK, b);
    }
    if let Some(b) = state
        .db
        .list_firmware_builds()?
        .into_iter()
        .find(|b| b.is(&info))
    {
        return Err(ApiError(
            StatusCode::CONFLICT,
            format!(
                "build {} ({}) has the same hardware id, commit and build time but other \
                 contents; delete it first to replace it",
                b.id, b.describe
            ),
        ));
    }
    let dir = &state.cfg.firmware_dir;
    tokio::fs::create_dir_all(dir).await?;
    let path = dir.join(format!("{md5}.bin"));
    let tmp = dir.join(format!("{md5}.bin.part"));
    tokio::fs::write(&tmp, &body).await?;
    tokio::fs::rename(&tmp, &path).await?;
    let b = state
        .db
        .insert_firmware_build(&info, body.len() as u64, &md5, now_ns())?;
    tracing::info!(id = b.id, describe = %b.describe, hardware_id = %b.hardware_id, "firmware uploaded");
    state.event(
        None,
        "firmware_uploaded",
        json!({ "build_id": b.id, "hardware_id": b.hardware_id, "describe": b.describe,
                "size": b.size, "md5": b.md5 }),
    );
    respond(&state, StatusCode::CREATED, b)
}

/// `DELETE /firmware/{id}`: refused (`409`) while a client is pinned to it.
async fn delete_build(
    State(state): State<SharedState>,
    Path(id): Path<i64>,
) -> ApiResult<StatusCode> {
    let b = state
        .db
        .get_firmware_build(id)?
        .ok_or_else(|| ApiError::not_found("firmware build"))?;
    let pinned: Vec<String> = state
        .db
        .list_clients()?
        .into_iter()
        .filter(|c| c.firmware_policy == Some(FirmwarePolicy::Manual { build_id: id }))
        .map(|c| {
            if c.name.is_empty() {
                c.id.to_string()
            } else {
                c.name
            }
        })
        .collect();
    if !pinned.is_empty() {
        return Err(ApiError(
            StatusCode::CONFLICT,
            format!(
                "build {id} is the manual choice of {}; choose another build for them first",
                pinned.join(", ")
            ),
        ));
    }
    state.db.delete_firmware_build(id)?;
    let path = state.cfg.firmware_dir.join(b.file_name());
    if let Err(e) = tokio::fs::remove_file(&path).await {
        tracing::warn!(path = %path.display(), "removing firmware image: {e}");
    }
    state.event(
        None,
        "firmware_deleted",
        json!({ "build_id": id, "hardware_id": b.hardware_id, "describe": b.describe }),
    );
    Ok(StatusCode::NO_CONTENT)
}

/// A client's firmware: what it runs, what it is to run, and how updating
/// it went (API 1.2.0).
#[derive(Serialize)]
pub struct FirmwareView {
    /// It can be updated over the air; if not, `unsupported` says why.
    supported: bool,
    unsupported: Option<String>,
    /// The kept build it runs, if the server has it.
    running: Option<FirmwareBuild>,
    /// The build chosen for it; null until one is chosen (it then stays on
    /// the build it runs, `running`).
    policy: Option<FirmwarePolicy>,
    /// The build it is to run under `policy`.
    target: Option<FirmwareBuild>,
    /// The build its heartbeat replies offer it now.
    offering: Option<i64>,
    /// Builds that failed on it: not offered again until `policy` is set again.
    blocked: Vec<i64>,
    last: Option<LastUpdate>,
}

pub fn firmware_view(state: &SharedState, c: &ClientRecord) -> FirmwareView {
    let builds = state.db.list_firmware_builds().unwrap_or_default();
    let running = c
        .build
        .as_ref()
        .and_then(|r| builds.iter().find(|b| b.is(r)))
        .cloned();
    let unsupported = firmware::unsupported_reason(c);
    FirmwareView {
        supported: unsupported.is_none(),
        unsupported,
        running,
        policy: c.firmware_policy,
        target: firmware::target(c.firmware_policy, c.build.as_ref(), &builds).cloned(),
        offering: firmware::offer(c, &builds).map(|b| b.id),
        blocked: c.firmware.blocked.clone(),
        last: c.firmware.last.clone(),
    }
}

/// `PUT /clients/{id}/firmware`: the build the client is to run,
/// `{"policy": "latest_dev"}`, `{"policy": "latest_stable"}` or
/// `{"policy": "manual", "build_id": 42}`. Setting it, even to the same,
/// lets builds that failed on the client be offered again.
async fn set_client_policy(
    State(state): State<SharedState>,
    Path(id): Path<ClientId>,
    Json(policy): Json<FirmwarePolicy>,
) -> ApiResult<Json<FirmwareView>> {
    let c = state
        .db
        .get_client(id)?
        .ok_or_else(|| ApiError::not_found("client"))?;
    if let Some(why) = firmware::unsupported_reason(&c) {
        return Err(ApiError(
            StatusCode::CONFLICT,
            format!("not supported by this client: {why}"),
        ));
    }
    if let FirmwarePolicy::Manual { build_id } = policy {
        let b = state
            .db
            .get_firmware_build(build_id)?
            .ok_or_else(|| ApiError::not_found("firmware build"))?;
        let hw = c.build.as_ref().map(|r| r.hardware_id.as_str());
        if hw != Some(b.hardware_id.as_str()) {
            return Err(ApiError::bad_request(format!(
                "build {build_id} is for \"{}\", but the client is \"{}\"",
                b.hardware_id,
                hw.unwrap_or("unknown")
            )));
        }
    }
    let mut fw = c.firmware.clone();
    fw.blocked.clear();
    fw.offered = None;
    state.db.set_firmware_policy(id, Some(&policy), &fw)?;
    state.event(
        Some(id),
        "firmware_policy_set",
        serde_json::to_value(policy)?,
    );
    let c = state
        .db
        .get_client(id)?
        .ok_or_else(|| ApiError::not_found("client"))?;
    Ok(Json(firmware_view(&state, &c)))
}
