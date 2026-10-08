//! NTP-style UDP clock measurement.
//!
//! Request (client -> server, 36 bytes): magic, version, kind=1, seq, client id, t1.
//! Response (server -> client, 36 bytes): magic, version, kind=2, seq, t1 (echoed), t2, t3.
//! t1 = client send, t2 = server receive, t3 = server send, t4 = client receive.
//!
//! [`Estimator`] turns a stream of these measurements into the client's clock
//! offset and drift relative to the server.

use crate::{ClientId, NANOS_PER_SEC, Timestamp};
use std::collections::VecDeque;
use uuid::Uuid;

const MAGIC: [u8; 3] = *b"BSP";
const VERSION: u8 = 1;
const KIND_REQUEST: u8 = 1;
const KIND_RESPONSE: u8 = 2;

pub const REQUEST_LEN: usize = 3 + 1 + 1 + 3 + 4 + 16 + 8;
pub const RESPONSE_LEN: usize = 3 + 1 + 1 + 3 + 4 + 8 * 3;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Request {
    pub seq: u32,
    pub client_id: ClientId,
    pub t1: Timestamp,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Response {
    pub seq: u32,
    pub t1: Timestamp,
    pub t2: Timestamp,
    pub t3: Timestamp,
}

fn header(kind: u8, seq: u32) -> Vec<u8> {
    let mut b = Vec::with_capacity(RESPONSE_LEN);
    b.extend_from_slice(&MAGIC);
    b.push(VERSION);
    b.push(kind);
    b.extend_from_slice(&[0; 3]);
    b.extend_from_slice(&seq.to_be_bytes());
    b
}

fn check_header(b: &[u8], kind: u8, len: usize) -> Option<u32> {
    if b.len() != len || b[0..3] != MAGIC || b[3] != VERSION || b[4] != kind {
        return None;
    }
    Some(u32::from_be_bytes(b[8..12].try_into().ok()?))
}

fn i64_at(b: &[u8], at: usize) -> i64 {
    i64::from_be_bytes(b[at..at + 8].try_into().unwrap())
}

impl Request {
    pub fn encode(&self) -> Vec<u8> {
        let mut b = header(KIND_REQUEST, self.seq);
        b.extend_from_slice(self.client_id.as_bytes());
        b.extend_from_slice(&self.t1.to_be_bytes());
        b
    }

    pub fn decode(b: &[u8]) -> Option<Self> {
        let seq = check_header(b, KIND_REQUEST, REQUEST_LEN)?;
        let client_id = Uuid::from_slice(&b[12..28]).ok()?;
        Some(Self {
            seq,
            client_id,
            t1: i64_at(b, 28),
        })
    }
}

impl Response {
    pub fn encode(&self) -> Vec<u8> {
        let mut b = header(KIND_RESPONSE, self.seq);
        for t in [self.t1, self.t2, self.t3] {
            b.extend_from_slice(&t.to_be_bytes());
        }
        b
    }

    pub fn decode(b: &[u8]) -> Option<Self> {
        let seq = check_header(b, KIND_RESPONSE, RESPONSE_LEN)?;
        Some(Self {
            seq,
            t1: i64_at(b, 12),
            t2: i64_at(b, 20),
            t3: i64_at(b, 28),
        })
    }

    /// Returns `(offset_ns, rtt_ns)` given the client receive time `t4`.
    /// `offset` is server clock minus client clock.
    pub fn offset_rtt(&self, t4: Timestamp) -> (i64, i64) {
        let offset = ((self.t2 - self.t1) + (self.t3 - t4)) / 2;
        let rtt = (t4 - self.t1) - (self.t3 - self.t2);
        (offset, rtt)
    }

    /// The measurement this reply completes, or `None` if it cannot be right
    /// (a clock stepped during the exchange, making the round trip negative or
    /// the server's hold time absurd).
    pub fn sample(&self, t4: Timestamp) -> Option<Sample> {
        let (offset_ns, rtt_ns) = self.offset_rtt(t4);
        let held = self.t3 - self.t2;
        (rtt_ns >= 0 && (0..NANOS_PER_SEC).contains(&held)).then_some(Sample {
            at: self.t1 + (t4 - self.t1) / 2,
            offset_ns,
            rtt_ns,
        })
    }
}

/// One clock measurement, on the client's clock.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Sample {
    /// Client time halfway through the exchange.
    pub at: Timestamp,
    /// Server clock minus client clock.
    pub offset_ns: i64,
    pub rtt_ns: i64,
}

/// Offset and drift of the client clock, fitted over recent measurements.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Fit {
    /// Client time the fit is anchored at.
    pub at: Timestamp,
    /// Server clock minus client clock at `at`.
    pub offset_ns: f64,
    /// How fast the offset changes, in ns per ns of client time.
    pub rate: f64,
    /// Lowest round trip among the measurements used.
    pub rtt_ns: i64,
    /// How far the true offset may be from the fitted one: half the lowest
    /// round trip (the measurement cannot tell how delay splits between the
    /// two directions) plus the scatter of the measurements around the line.
    pub error_ns: i64,
    /// Number of measurements the line was fitted through.
    pub points: usize,
}

impl Fit {
    /// Server clock minus client clock at client time `t`.
    pub fn offset_at(&self, t: Timestamp) -> i64 {
        (self.offset_ns + self.rate * (t - self.at) as f64).round() as i64
    }

    /// Client clock rate relative to the server's, in parts per million
    /// (positive: the client clock runs slow).
    pub fn drift_ppm(&self) -> f64 {
        self.rate * 1e6
    }
}

/// Estimates a client's clock offset and drift from its measurements.
///
/// Queueing, a busy host or Wi-Fi retries delay packets unevenly in the two
/// directions, which makes a measurement wrong by up to half its round trip;
/// the fastest round trips carry the least of it. So the window is cut into
/// bins, each bin keeps its fastest measurement, and a line fitted through
/// those (weighted towards fast ones) gives offset and drift. A clock step on
/// either side shows up as measurements that keep disagreeing with the line;
/// the estimator then drops what came before the step.
#[derive(Debug, Clone)]
pub struct Estimator {
    window_ns: i64,
    bins: usize,
    samples: VecDeque<Sample>,
    fit: Option<Fit>,
    /// Time of the first of the current run of disagreeing measurements.
    disagreeing_since: Option<Timestamp>,
    disagreements: u32,
}

/// Measurements in a row that must disagree with the fit to count as a step.
const STEP_RUN: u32 = 3;
/// Disagreement beyond the measurement's own uncertainty that counts.
const STEP_MARGIN_NS: i64 = 100_000;
/// Fits spanning less of the window than this assume no drift.
const MIN_DRIFT_SPAN: f64 = 0.25;
const MAX_SAMPLES: usize = 4096;

impl Estimator {
    pub fn new(window_ns: i64, bins: usize) -> Self {
        Self {
            window_ns,
            bins: bins.max(1),
            samples: VecDeque::new(),
            fit: None,
            disagreeing_since: None,
            disagreements: 0,
        }
    }

    pub fn fit(&self) -> Option<Fit> {
        self.fit
    }

    /// Adds a measurement and returns the updated fit.
    pub fn push(&mut self, s: Sample) -> Option<Fit> {
        if let Some(f) = self.fit {
            let miss = (s.offset_ns - f.offset_at(s.at)).abs();
            if miss > s.rtt_ns / 2 + f.error_ns + STEP_MARGIN_NS {
                let since = *self.disagreeing_since.get_or_insert(s.at);
                self.disagreements += 1;
                if self.disagreements >= STEP_RUN {
                    self.samples.retain(|p| p.at >= since);
                    self.disagreeing_since = None;
                    self.disagreements = 0;
                }
            } else {
                self.disagreeing_since = None;
                self.disagreements = 0;
            }
        }
        // Measurements arrive in order; drop anything older (the clock stepped back).
        while self.samples.back().is_some_and(|p| p.at > s.at) {
            self.samples.pop_back();
        }
        self.samples.push_back(s);
        while self.samples.len() > MAX_SAMPLES
            || self
                .samples
                .front()
                .is_some_and(|p| s.at - p.at > self.window_ns)
        {
            self.samples.pop_front();
        }
        // While a run of disagreements may turn out to be a step, keep the old fit.
        if self.disagreements == 0 || self.fit.is_none() {
            self.fit = self.refit();
        }
        self.fit
    }

    fn refit(&self) -> Option<Fit> {
        let newest = self.samples.back()?.at;
        let bin_ns = (self.window_ns / self.bins as i64).max(1);
        let mut best: Vec<Sample> = Vec::new();
        for s in &self.samples {
            let bin = (newest - s.at) / bin_ns;
            match best.last_mut() {
                Some(b) if (newest - b.at) / bin_ns == bin => {
                    if s.rtt_ns < b.rtt_ns {
                        *b = *s;
                    }
                }
                _ => best.push(*s),
            }
        }
        let rtt_ns = best.iter().map(|s| s.rtt_ns).min()?;
        let span = (newest - best[0].at) as f64;
        // Weight 1/rtt^2: a measurement's error bound grows with its round trip.
        let floor = 10_000.0; // 10 us, so near-zero round trips do not dominate
        let w = |s: &Sample| 1.0 / (s.rtt_ns as f64 + floor).powi(2);
        let at = newest;
        let x = |s: &Sample| (s.at - at) as f64;
        let y = |s: &Sample| s.offset_ns as f64;
        let sw: f64 = best.iter().map(w).sum();
        let mx = best.iter().map(|s| w(s) * x(s)).sum::<f64>() / sw;
        let my = best.iter().map(|s| w(s) * y(s)).sum::<f64>() / sw;
        let sxx: f64 = best.iter().map(|s| w(s) * (x(s) - mx).powi(2)).sum();
        let rate = if best.len() >= 3 && span >= MIN_DRIFT_SPAN * self.window_ns as f64 {
            best.iter()
                .map(|s| w(s) * (x(s) - mx) * (y(s) - my))
                .sum::<f64>()
                / sxx
        } else {
            0.0
        };
        let offset_ns = my - rate * mx;
        let scatter = (best
            .iter()
            .map(|s| w(s) * (y(s) - offset_ns - rate * x(s)).powi(2))
            .sum::<f64>()
            / sw)
            .sqrt();
        Some(Fit {
            at,
            offset_ns,
            rate,
            rtt_ns,
            error_ns: rtt_ns / 2 + scatter.round() as i64,
            points: best.len(),
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn roundtrip() {
        let req = Request {
            seq: 7,
            client_id: Uuid::new_v4(),
            t1: 123_456_789,
        };
        assert_eq!(Request::decode(&req.encode()), Some(req));
        let resp = Response {
            seq: 7,
            t1: 1,
            t2: 2,
            t3: 3,
        };
        assert_eq!(Response::decode(&resp.encode()), Some(resp));
        assert_eq!(Response::decode(&req.encode()), None);
    }

    #[test]
    fn offset_math() {
        // Server is 5 ms ahead of client; one-way delay 1 ms each way; 0.2 ms processing.
        let off = 5_000_000;
        let t1 = 1_000_000_000;
        let t2 = t1 + 1_000_000 + off;
        let t3 = t2 + 200_000;
        let t4 = t3 - off + 1_000_000;
        let (o, rtt) = Response { seq: 0, t1, t2, t3 }.offset_rtt(t4);
        assert_eq!(o, off);
        assert_eq!(rtt, 2_000_000);
    }

    #[test]
    fn rejects_impossible_replies() {
        let r = Response {
            seq: 0,
            t1: 1_000,
            t2: 5_000,
            t3: 5_100,
        };
        assert!(r.sample(2_000).is_some());
        // The client clock stepped back during the exchange.
        assert!(r.sample(500).is_none());
        // The server clock stepped back between receive and send.
        assert!(Response { t3: 4_000, ..r }.sample(2_000).is_none());
    }

    /// Measurements of a client clock with the given offset and drift, where
    /// every fourth one has a fast symmetric round trip and the rest are
    /// delayed (asymmetrically) on the way back.
    fn measure(i: i64, offset_ns: i64, drift_ppm: f64) -> Sample {
        let at = 1_000 * NANOS_PER_SEC + i * NANOS_PER_SEC / 4;
        let true_offset = offset_ns + (drift_ppm * 1e-6 * (at as f64 - 1e12)) as i64;
        let (rtt_ns, extra_back) = if i % 4 == 0 {
            (200_000, 0)
        } else {
            (200_000 + (i % 7) * 1_000_000, (i % 7) * 1_000_000)
        };
        Sample {
            at,
            // Delay on the way back makes the client think the server is behind.
            offset_ns: true_offset - extra_back / 2,
            rtt_ns,
        }
    }

    #[test]
    fn fits_offset_and_drift_through_fast_round_trips() {
        let mut est = Estimator::new(64 * NANOS_PER_SEC, 16);
        let mut last = None;
        for i in 0..400 {
            last = est.push(measure(i, 3_000_000, 20.0));
        }
        let f = last.unwrap();
        assert!(
            (f.drift_ppm() - 20.0).abs() < 0.5,
            "drift {}",
            f.drift_ppm()
        );
        let at = 1_000 * NANOS_PER_SEC + 399 * NANOS_PER_SEC / 4;
        let truth = 3_000_000 + (20e-6 * (at as f64 - 1e12)) as i64;
        assert!(
            (f.offset_at(at) - truth).abs() < 20_000,
            "off by {}",
            f.offset_at(at) - truth
        );
        assert_eq!(f.rtt_ns, 200_000);
        assert!(
            f.error_ns >= 100_000 && f.error_ns < 150_000,
            "error {}",
            f.error_ns
        );
    }

    #[test]
    fn starts_afresh_after_a_clock_step() {
        let mut est = Estimator::new(64 * NANOS_PER_SEC, 16);
        for i in 0..200 {
            est.push(measure(i, 0, 0.0));
        }
        // The client clock steps 5 ms back: the server now looks 5 ms ahead.
        let mut f = None;
        for i in 200..260 {
            f = est.push(measure(i, 5_000_000, 0.0));
        }
        let f = f.unwrap();
        assert!(
            (f.offset_ns - 5e6).abs() < 20_000.0,
            "offset {}",
            f.offset_ns
        );
        assert!(f.drift_ppm().abs() < 1.0, "drift {}", f.drift_ppm());
    }

    #[test]
    fn a_single_outlier_is_not_a_step() {
        let mut est = Estimator::new(64 * NANOS_PER_SEC, 16);
        for i in 0..200 {
            est.push(measure(i, 0, 0.0));
        }
        let mut odd = measure(200, 0, 0.0);
        odd.offset_ns += 50_000_000;
        est.push(odd);
        let f = est.push(measure(201, 0, 0.0)).unwrap();
        assert!(f.offset_ns.abs() < 20_000.0, "offset {}", f.offset_ns);
    }
}
