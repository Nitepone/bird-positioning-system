//! Periodic UDP clock measurement against the server.
//!
//! The measurements feed a [`bsp_proto::timesync::Estimator`], and by default
//! the client adds the estimated offset to its audio timestamps so they are on
//! the server's clock. With `--trust-os-clock` (both ends disciplined by PTP or
//! GPS, which beats a measurement over the network) it only reports how far
//! off its clock is.

use bsp_proto::timesync::{Estimator, RESPONSE_LEN, Request, Response};
use bsp_proto::{ClientId, ClockStatus, NANOS_PER_SEC, SyncDetail, Timestamp, now_ns};
use std::net::SocketAddr;
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};
use tokio::net::UdpSocket;

const INTERVAL: Duration = Duration::from_millis(500);
const TIMEOUT: Duration = Duration::from_millis(400);
/// Measurements older than this are forgotten.
const WINDOW_NS: i64 = 64 * NANOS_PER_SEC;
/// The window is cut into this many bins; each keeps its fastest measurement.
const BINS: usize = 16;

pub type SharedClock = Arc<Clock>;

/// The clock estimate, and what has been done with it.
pub struct Clock {
    corrects: bool,
    inner: Mutex<Inner>,
}

struct Inner {
    est: Estimator,
    /// Client time of the latest measurement.
    last_at: Option<Timestamp>,
    /// Midpoint and correction of the latest corrected chunk.
    applied: Option<(Timestamp, i64)>,
    /// Half the length of that chunk: drift within it is not corrected.
    half_chunk_ns: i64,
    requests: u32,
    rtts: Vec<i64>,
}

impl Clock {
    pub fn new(corrects: bool) -> Self {
        Self {
            corrects,
            inner: Mutex::new(Inner {
                est: Estimator::new(WINDOW_NS, BINS),
                last_at: None,
                applied: None,
                half_chunk_ns: 0,
                requests: 0,
                rtts: Vec::new(),
            }),
        }
    }

    /// The timestamp to send for audio captured from client time `start` for
    /// `len_ns`: on the server's clock, evaluated at the chunk's midpoint.
    pub fn correct(&self, start: Timestamp, len_ns: i64) -> Timestamp {
        if !self.corrects {
            return start;
        }
        let mut g = self.inner.lock().unwrap();
        let mid = start + len_ns / 2;
        let corr = g.est.fit().map_or(0, |f| f.offset_at(mid));
        g.applied = Some((mid, corr));
        g.half_chunk_ns = len_ns / 2;
        start + corr
    }

    /// Status for the next heartbeat; starts a new interval for the counts.
    pub fn status(&self) -> Option<ClockStatus> {
        let mut g = self.inner.lock().unwrap();
        let rtts = std::mem::take(&mut g.rtts);
        let requests = std::mem::take(&mut g.requests);
        let fit = g.est.fit()?;
        let last_at = g.last_at?;
        let now = now_ns();
        // Offset left in the timestamps: for corrected chunks, how far the
        // current estimate has moved from the correction applied.
        let (offset_ns, drift_ns) = match (self.corrects, g.applied) {
            (true, Some((mid, corr))) => (
                fit.offset_at(mid) - corr,
                (fit.rate * g.half_chunk_ns as f64).abs().round() as i64,
            ),
            (true, None) => (0, 0),
            (false, _) => (fit.offset_at(now), 0),
        };
        let mut sorted = rtts;
        sorted.sort_unstable();
        Some(ClockStatus {
            offset_ns,
            rtt_ns: fit.rtt_ns,
            measured_at: last_at + fit.offset_at(last_at),
            error_ns: Some(offset_ns.abs() + fit.error_ns + drift_ns),
            detail: Some(SyncDetail {
                corrects_timestamps: self.corrects,
                clock_offset_ns: fit.offset_at(now),
                drift_ppm: fit.drift_ppm(),
                fit_points: fit.points as u32,
                requests,
                replies: sorted.len() as u32,
                rtt_min_ns: sorted.first().copied(),
                rtt_median_ns: sorted.get(sorted.len() / 2).copied(),
                rtt_max_ns: sorted.last().copied(),
            }),
        })
    }
}

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
    let mut tick = tokio::time::interval(INTERVAL);
    let mut buf = [0u8; 64];
    let mut seq = 0u32;
    let mut warned = false;
    loop {
        tick.tick().await;
        seq = seq.wrapping_add(1);
        let t1 = now_ns();
        let sent = Instant::now();
        let req = Request { seq, client_id, t1 };
        if let Err(e) = socket.send(&req.encode()).await {
            tracing::debug!("timesync send: {e}");
            continue;
        }
        let deadline = tokio::time::Instant::now() + TIMEOUT;
        let resp = loop {
            match tokio::time::timeout_at(deadline, socket.recv(&mut buf)).await {
                Ok(Ok(n)) => {
                    // t4 from the monotonic clock: a wall-clock step during the
                    // exchange must not corrupt the round trip.
                    let t4 = t1 + sent.elapsed().as_nanos() as i64;
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
        // Counted once answered or timed out, so a request and its reply
        // always fall in the same heartbeat interval.
        clock.inner.lock().unwrap().requests += 1;
        let Some((resp, t4)) = resp else {
            if !warned {
                tracing::warn!(%server, "no timesync reply");
                warned = true;
            }
            continue;
        };
        warned = false;
        let Some(sample) = resp.sample(t4) else {
            tracing::debug!(?resp, t4, "implausible timesync reply ignored");
            continue;
        };
        let mut g = clock.inner.lock().unwrap();
        g.rtts.push(sample.rtt_ns);
        g.last_at = Some(sample.at);
        g.est.push(sample);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use bsp_proto::timesync::Sample;

    fn feed(clock: &Clock, offset_ns: i64) {
        let now = now_ns();
        let mut g = clock.inner.lock().unwrap();
        for i in 0..40 {
            let at = now - (40 - i) * NANOS_PER_SEC / 2;
            g.est.push(Sample {
                at,
                offset_ns,
                rtt_ns: 400_000,
            });
            g.last_at = Some(at);
        }
    }

    #[test]
    fn corrects_chunks_and_reports_what_is_left() {
        let clock = Clock::new(true);
        // No estimate yet: the chunk goes out uncorrected and says so.
        assert_eq!(clock.correct(1_000, NANOS_PER_SEC), 1_000);
        feed(&clock, 3_000_000);
        let st = clock.status().unwrap();
        assert_eq!(st.offset_ns, 3_000_000);
        assert!(st.error_ns.unwrap() >= 3_000_000);

        let start = now_ns();
        assert_eq!(clock.correct(start, 9 * NANOS_PER_SEC), start + 3_000_000);
        let st = clock.status().unwrap();
        assert_eq!(st.offset_ns, 0);
        assert_eq!(st.error_ns, Some(200_000));
        assert_eq!(st.detail.unwrap().clock_offset_ns, 3_000_000);
    }

    #[test]
    fn trusting_the_os_clock_reports_the_raw_offset() {
        let clock = Clock::new(false);
        feed(&clock, -1_500_000);
        let start = now_ns();
        assert_eq!(clock.correct(start, NANOS_PER_SEC), start);
        let st = clock.status().unwrap();
        assert_eq!(st.offset_ns, -1_500_000);
        assert_eq!(st.error_ns, Some(1_700_000));
        assert_eq!(st.error_bound_ns(), 1_700_000);
    }
}
