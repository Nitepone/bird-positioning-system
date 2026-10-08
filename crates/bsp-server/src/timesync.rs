//! Responder for client clock measurement (see `bsp_proto::timesync`): over
//! UDP for native clients, and over a WebSocket for browser clients, which
//! cannot send UDP.

use axum::extract::WebSocketUpgrade;
use axum::extract::ws::{Message, WebSocket};
use axum::response::Response;
use bsp_proto::now_ns;
use bsp_proto::timesync::{REQUEST_LEN, Request, Response as SyncResponse};
use tokio::net::UdpSocket;

/// Answers one request packet received at `t2`, or `None` if it is malformed.
fn reply(packet: &[u8], t2: i64) -> Option<Vec<u8>> {
    if packet.len() != REQUEST_LEN {
        return None;
    }
    let req = Request::decode(packet)?;
    let mut resp = SyncResponse {
        seq: req.seq,
        t1: req.t1,
        t2,
        t3: 0,
    };
    resp.t3 = now_ns();
    Some(resp.encode())
}

pub async fn run(socket: UdpSocket) -> anyhow::Result<()> {
    let mut buf = [0u8; 64];
    loop {
        let (n, peer) = socket.recv_from(&mut buf).await?;
        let t2 = now_ns();
        let Some(resp) = reply(&buf[..n], t2) else {
            continue;
        };
        if let Err(e) = socket.send_to(&resp, peer).await {
            tracing::debug!(%peer, "timesync send failed: {e}");
        }
    }
}

/// `GET {CLIENT_API_PREFIX}/timesync`: the same packets as the UDP responder,
/// as binary WebSocket messages.
pub async fn websocket(ws: WebSocketUpgrade) -> Response {
    ws.on_upgrade(serve_ws)
}

async fn serve_ws(mut socket: WebSocket) {
    while let Some(Ok(msg)) = socket.recv().await {
        let t2 = now_ns();
        let Message::Binary(packet) = msg else {
            continue;
        };
        let Some(resp) = reply(&packet, t2) else {
            continue;
        };
        if socket.send(Message::Binary(resp.into())).await.is_err() {
            return;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use bsp_proto::Uuid;

    #[test]
    fn replies_to_valid_requests_only() {
        let req = Request {
            seq: 3,
            client_id: Uuid::new_v4(),
            t1: 42,
        };
        let resp = SyncResponse::decode(&reply(&req.encode(), 100).unwrap()).unwrap();
        assert_eq!((resp.seq, resp.t1, resp.t2), (3, 42, 100));
        assert!(resp.t3 >= 100);
        assert!(reply(&req.encode()[1..], 100).is_none());
    }
}
