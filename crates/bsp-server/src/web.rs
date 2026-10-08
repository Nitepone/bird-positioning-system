//! Listeners for the web UI / control API (HTTPS, with an HTTP redirect) and
//! for client devices.

use crate::config::WebConfig;
use anyhow::{Context, bail};
use axum::Router;
use axum::extract::Request;
use axum::http::{StatusCode, Uri, header};
use axum::response::{IntoResponse, Redirect, Response};
use axum_server::Handle;
use axum_server::tls_rustls::RustlsConfig;
use std::net::{SocketAddr, TcpListener};
use std::path::Path;

/// Binds a TCP listener, explaining the usual fix when a privileged port is refused.
pub fn bind(addr: &str, what: &str) -> anyhow::Result<TcpListener> {
    let listener = TcpListener::bind(addr).map_err(|e| {
        let port = addr.rsplit(':').next().and_then(|p| p.parse::<u16>().ok()).unwrap_or(0);
        if e.kind() == std::io::ErrorKind::PermissionDenied && port < 1024 {
            anyhow::anyhow!(
                "binding {what} on {addr}: {e}. Ports below 1024 need privileges; either run \
                 `sudo setcap cap_net_bind_service=+ep <path to bsp-server>` (again after each build), \
                 run `sudo sysctl net.ipv4.ip_unprivileged_port_start=80`, or choose other ports in [web]"
            )
        } else {
            anyhow::Error::new(e).context(format!("binding {what} on {addr}"))
        }
    })?;
    listener.set_nonblocking(true)?;
    Ok(listener)
}

fn host_name() -> Option<String> {
    let name = std::fs::read_to_string("/etc/hostname").ok()?;
    let name = name.trim();
    (!name.is_empty()).then(|| name.to_string())
}

/// Loads the configured certificate, generating a self-signed one when
/// neither the certificate nor the key exists yet.
pub async fn tls_config(cfg: &WebConfig) -> anyhow::Result<RustlsConfig> {
    let (cert, key) = (&cfg.cert_path, &cfg.key_path);
    match (cert.exists(), key.exists()) {
        (true, true) => {}
        (false, false) => generate_self_signed(cfg)?,
        _ => bail!(
            "only one of {} and {} exists; provide both, or remove both to generate a self-signed certificate",
            cert.display(),
            key.display()
        ),
    }
    RustlsConfig::from_pem_chain_file(cert, key)
        .await
        .with_context(|| {
            format!(
                "loading TLS certificate {} / key {}",
                cert.display(),
                key.display()
            )
        })
}

fn generate_self_signed(cfg: &WebConfig) -> anyhow::Result<()> {
    let mut names = vec![
        "localhost".to_string(),
        "127.0.0.1".to_string(),
        "::1".to_string(),
    ];
    names.extend(host_name());
    names.extend(cfg.self_signed_names.iter().cloned());
    names.dedup();
    let certified =
        rcgen::generate_simple_self_signed(names.clone()).context("generating certificate")?;
    for path in [&cfg.cert_path, &cfg.key_path] {
        if let Some(dir) = path.parent().filter(|d| !d.as_os_str().is_empty()) {
            std::fs::create_dir_all(dir).with_context(|| format!("creating {}", dir.display()))?;
        }
    }
    write_private(&cfg.key_path, &certified.signing_key.serialize_pem())?;
    std::fs::write(&cfg.cert_path, certified.cert.pem())
        .with_context(|| format!("writing {}", cfg.cert_path.display()))?;
    tracing::warn!(
        cert = %cfg.cert_path.display(), ?names,
        "generated a self-signed certificate; browsers will ask you to accept it once"
    );
    Ok(())
}

fn write_private(path: &Path, contents: &str) -> anyhow::Result<()> {
    use std::io::Write;
    let mut opts = std::fs::OpenOptions::new();
    opts.write(true).create_new(true);
    #[cfg(unix)]
    std::os::unix::fs::OpenOptionsExt::mode(&mut opts, 0o600);
    let mut f = opts
        .open(path)
        .with_context(|| format!("creating {}", path.display()))?;
    f.write_all(contents.as_bytes())?;
    Ok(())
}

/// Redirects every request to the same host and path over HTTPS.
pub fn redirect_router(https_port: u16) -> Router {
    Router::new().fallback(move |req: Request| async move { https_redirect(req, https_port) })
}

fn https_redirect(req: Request, https_port: u16) -> Response {
    let Some(host) = req
        .headers()
        .get(header::HOST)
        .and_then(|h| h.to_str().ok())
    else {
        return (StatusCode::BAD_REQUEST, "missing Host header").into_response();
    };
    // Drop any port from the Host header (keeping IPv6 brackets intact).
    let host = match host.rsplit_once(':') {
        Some((h, p)) if !p.contains(']') && p.chars().all(|c| c.is_ascii_digit()) => h,
        _ => host,
    };
    let authority = if https_port == 443 {
        host.to_string()
    } else {
        format!("{host}:{https_port}")
    };
    let path = req
        .uri()
        .path_and_query()
        .map(|p| p.as_str())
        .unwrap_or("/");
    match format!("https://{authority}{path}").parse::<Uri>() {
        Ok(uri) => Redirect::permanent(&uri.to_string()).into_response(),
        Err(_) => (StatusCode::BAD_REQUEST, "invalid Host header").into_response(),
    }
}

/// Serves the web UI as configured, until `handle` is shut down.
pub async fn serve(cfg: WebConfig, app: Router, handle: Handle<SocketAddr>) -> anyhow::Result<()> {
    let mut tasks = tokio::task::JoinSet::new();
    if cfg.https {
        let tls = tls_config(&cfg).await?;
        let listener = bind(&cfg.https_listen, "HTTPS")?;
        let https_port = listener.local_addr()?.port();
        tracing::info!(addr = %listener.local_addr()?, "web UI (HTTPS)");
        let server = axum_server::from_tcp_rustls(listener, tls)?.handle(handle.clone());
        tasks.spawn(server.serve(app.into_make_service_with_connect_info::<SocketAddr>()));
        if !cfg.http_listen.is_empty() {
            let listener = bind(&cfg.http_listen, "HTTP redirect")?;
            tracing::info!(addr = %listener.local_addr()?, "redirecting HTTP to HTTPS");
            let server = axum_server::from_tcp(listener)?.handle(handle.clone());
            tasks.spawn(server.serve(redirect_router(https_port).into_make_service()));
        }
    } else {
        let listener = bind(&cfg.http_listen, "HTTP")?;
        tracing::warn!(addr = %listener.local_addr()?, "web UI over plain HTTP ([web] https = false)");
        let server = axum_server::from_tcp(listener)?.handle(handle.clone());
        tasks.spawn(server.serve(app.into_make_service_with_connect_info::<SocketAddr>()));
    }
    while let Some(result) = tasks.join_next().await {
        result?.context("web server")?;
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use axum::body::Body;
    use tower::ServiceExt;

    async fn location(host: &str, path: &str, port: u16) -> String {
        let req = axum::http::Request::builder()
            .uri(path)
            .header("host", host)
            .body(Body::empty())
            .unwrap();
        let resp = redirect_router(port).oneshot(req).await.unwrap();
        assert_eq!(resp.status(), StatusCode::PERMANENT_REDIRECT);
        resp.headers()[header::LOCATION]
            .to_str()
            .unwrap()
            .to_string()
    }

    #[tokio::test]
    async fn redirects_to_https() {
        // Browsers re-apply the #fragment (the UI's page) after a redirect.
        assert_eq!(
            location("bird.local", "/", 443).await,
            "https://bird.local/"
        );
        assert_eq!(
            location("bird.local:80", "/api?x=1", 443).await,
            "https://bird.local/api?x=1"
        );
        assert_eq!(
            location("10.0.0.5:8080", "/", 8443).await,
            "https://10.0.0.5:8443/"
        );
        assert_eq!(location("[::1]:80", "/", 443).await, "https://[::1]/");
    }
}
