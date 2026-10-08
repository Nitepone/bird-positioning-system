//! UDP responder for client clock measurement (see `bsp_proto::timesync`).

use bsp_proto::now_ns;
use bsp_proto::timesync::{REQUEST_LEN, Request, Response};
use tokio::net::UdpSocket;

pub async fn run(socket: UdpSocket) -> anyhow::Result<()> {
    let mut buf = [0u8; 64];
    loop {
        let (n, peer) = socket.recv_from(&mut buf).await?;
        let t2 = now_ns();
        if n != REQUEST_LEN {
            continue;
        }
        let Some(req) = Request::decode(&buf[..n]) else {
            continue;
        };
        let mut resp = Response {
            seq: req.seq,
            t1: req.t1,
            t2,
            t3: 0,
        };
        resp.t3 = now_ns();
        if let Err(e) = socket.send_to(&resp.encode(), peer).await {
            tracing::debug!(%peer, "timesync send failed: {e}");
        }
    }
}
