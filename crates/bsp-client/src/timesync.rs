//! Periodic UDP clock measurement against the server.
//!
//! The client clock itself is expected to be disciplined by the OS (chrony,
//! PTP, GPS); this only measures how far off it is so the server can decide
//! whether the client is usable for positioning.

use bsp_proto::timesync::{RESPONSE_LEN, Request, Response};
use bsp_proto::{ClientId, ClockStatus, now_ns};
use std::collections::VecDeque;
use std::net::SocketAddr;
use std::sync::{Arc, Mutex};
use std::time::Duration;
use tokio::net::UdpSocket;

const INTERVAL: Duration = Duration::from_secs(1);
const TIMEOUT: Duration = Duration::from_millis(500);
/// Number of recent samples the best (lowest RTT) one is chosen from.
const HISTORY: usize = 16;

pub type SharedClock = Arc<Mutex<Option<ClockStatus>>>;

pub async fn run(
    server: SocketAddr,
    client_id: ClientId,
    clock: SharedClock,
) -> anyhow::Result<()> {
    let bind: SocketAddr = if server.is_ipv4() {
        "0.0.0.0:0"
    } else {
        "[::]:0"
    }
    .parse()?;
    let socket = UdpSocket::bind(bind).await?;
    socket.connect(server).await?;
    let mut history: VecDeque<ClockStatus> = VecDeque::new();
    let mut tick = tokio::time::interval(INTERVAL);
    let mut buf = [0u8; 64];
    let mut seq = 0u32;
    let mut warned = false;
    loop {
        tick.tick().await;
        seq = seq.wrapping_add(1);
        let req = Request {
            seq,
            client_id,
            t1: now_ns(),
        };
        if let Err(e) = socket.send(&req.encode()).await {
            tracing::debug!("timesync send: {e}");
            continue;
        }
        let deadline = tokio::time::Instant::now() + TIMEOUT;
        let resp = loop {
            match tokio::time::timeout_at(deadline, socket.recv(&mut buf)).await {
                Ok(Ok(n)) => {
                    let t4 = now_ns();
                    // Ignore stale replies to earlier, timed-out requests.
                    if let Some(r) =
                        Response::decode(&buf[..n.min(RESPONSE_LEN)]).filter(|r| r.seq == seq)
                    {
                        break Some((r, t4));
                    }
                }
                _ => break None,
            }
        };
        let Some((resp, t4)) = resp else {
            if !warned {
                tracing::warn!(%server, "no timesync reply");
                warned = true;
            }
            continue;
        };
        warned = false;
        let (offset_ns, rtt_ns) = resp.offset_rtt(t4);
        history.push_back(ClockStatus {
            offset_ns,
            rtt_ns,
            measured_at: t4,
        });
        if history.len() > HISTORY {
            history.pop_front();
        }
        let best = *history.iter().min_by_key(|s| s.rtt_ns).unwrap();
        *clock.lock().unwrap() = Some(ClockStatus {
            measured_at: t4,
            ..best
        });
    }
}
