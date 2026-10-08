use axum::Json;
use axum::http::StatusCode;
use axum::response::{IntoResponse, Response};
use bsp_proto::ErrorBody;

pub mod client;
pub mod control;

/// An HTTP error with a JSON `{"error": ...}` body.
#[derive(Debug)]
pub struct ApiError(pub StatusCode, pub String);

impl ApiError {
    pub fn not_found(what: impl std::fmt::Display) -> Self {
        Self(StatusCode::NOT_FOUND, format!("{what} not found"))
    }

    pub fn bad_request(msg: impl std::fmt::Display) -> Self {
        Self(StatusCode::BAD_REQUEST, msg.to_string())
    }
}

impl<E: std::fmt::Display> From<E> for ApiError {
    fn from(e: E) -> Self {
        tracing::error!("internal error: {e}");
        Self(StatusCode::INTERNAL_SERVER_ERROR, e.to_string())
    }
}

impl IntoResponse for ApiError {
    fn into_response(self) -> Response {
        (self.0, Json(ErrorBody { error: self.1 })).into_response()
    }
}

pub type ApiResult<T> = Result<T, ApiError>;
