//! Converts Wi-Fi access points' clocks (TSF) to the server's.
//!
//! Clients that follow their access point's beacons timestamp audio in its
//! TSF, which all of that AP's clients share to microseconds. Each heartbeat
//! of such a client carries a reading of the TSF and of the server clock at
//! the same instant, the latter only as good as the client's own clock
//! measurement. A line through the readings of all the AP's clients, each
//! weighted by its error, converts the TSF to the server clock.
//!
//! The line is refitted once a minute rather than with every reading, and
//! audio is converted again with the current line whenever it is looked at
//! (see `AudioBuffers::extract`), so the clients of one AP always line up
//! with each other however the line moves.

use bsp_proto::{BeaconSync, Bssid, ClientId, NANOS_PER_SEC, Timestamp};
use serde::Serialize;
use std::collections::{HashMap, HashSet, VecDeque};
use std::sync::Mutex;

/// Readings older than this are forgotten.
const WINDOW_NS: i64 = 30 * 60 * NANOS_PER_SEC;
/// How often the line is refitted.
const REFIT_NS: i64 = 60 * NANOS_PER_SEC;
/// The line's error counts only readings this recent.
const RECENT_NS: i64 = 5 * 60 * NANOS_PER_SEC;
/// The drift needs readings over at least this long.
const MIN_DRIFT_SPAN_NS: i64 = 5 * 60 * NANOS_PER_SEC;
/// A reading this much further off the line than its error means the AP's
/// clock restarted (the AP rebooted): its readings start afresh.
const RESTART_MARGIN_NS: i64 = 100_000_000;

#[derive(Debug, Clone, Copy)]
struct Reading {
    client: ClientId,
    /// Server clock when it arrived.
    at: Timestamp,
    tsf_ns: i64,
    /// Server clock minus TSF.
    offset_ns: i64,
    error_ns: i64,
}

/// Server clock = TSF + offset, the offset drifting linearly with the TSF.
#[derive(Debug, Clone, Copy, PartialEq, Serialize)]
pub struct TsfLine {
    pub tsf_at: i64,
    pub offset_ns: f64,
    /// Offset change per ns of TSF: the AP's clock rate against the server's.
    pub rate: f64,
    /// Worst-case error of converted times against the server clock (ns).
    pub error_ns: i64,
    pub readings: usize,
    pub clients: usize,
    pub fitted_at: Timestamp,
}

impl TsfLine {
    pub fn server_at(&self, tsf_ns: i64) -> Timestamp {
        tsf_ns + (self.offset_ns + self.rate * (tsf_ns - self.tsf_at) as f64).round() as i64
    }

    pub fn drift_ppm(&self) -> f64 {
        self.rate * 1e6
    }
}

#[derive(Default)]
struct Ap {
    readings: VecDeque<Reading>,
    line: Option<TsfLine>,
}

#[derive(Default)]
pub struct BeaconClocks {
    aps: Mutex<HashMap<Bssid, Ap>>,
}

impl BeaconClocks {
    /// Adds a client's reading, received at server time `now`.
    pub fn push(&self, client: ClientId, b: &BeaconSync, now: Timestamp) {
        let r = Reading {
            client,
            at: now,
            tsf_ns: b.tsf_ns,
            offset_ns: b.server_ns - b.tsf_ns,
            error_ns: b.server_error_ns.max(0),
        };
        let mut aps = self.aps.lock().unwrap();
        let ap = aps.entry(b.bssid).or_default();
        if let Some(line) = &ap.line {
            let miss = (line.server_at(r.tsf_ns) - b.server_ns).abs();
            if miss > r.error_ns + line.error_ns + RESTART_MARGIN_NS {
                tracing::info!(bssid = %b.bssid, miss_ms = miss / 1_000_000,
                               "access point clock restarted");
                *ap = Ap::default();
            }
        }
        ap.readings.push_back(r);
        while ap.readings.front().is_some_and(|r| r.at < now - WINDOW_NS) {
            ap.readings.pop_front();
        }
        if ap.line.is_none_or(|l| now - l.fitted_at >= REFIT_NS) {
            ap.line = fit(&ap.readings, now);
        }
    }

    /// The current line for `bssid`, if any of its clients reported in.
    pub fn line(&self, bssid: Bssid) -> Option<TsfLine> {
        self.aps.lock().unwrap().get(&bssid)?.line
    }

    /// Every access point's line, for monitoring.
    pub fn lines(&self) -> Vec<(Bssid, TsfLine)> {
        let aps = self.aps.lock().unwrap();
        let mut v: Vec<_> = aps
            .iter()
            .filter_map(|(b, ap)| Some((*b, ap.line?)))
            .collect();
        v.sort_by_key(|(b, _)| *b);
        v
    }
}

/// Weighted least squares of offset against TSF, weights 1/error².
fn fit(readings: &VecDeque<Reading>, now: Timestamp) -> Option<TsfLine> {
    let first = readings.front()?;
    let last = readings.back()?;
    let w = |r: &Reading| 1.0 / ((r.error_ns + 10_000) as f64).powi(2);
    let (mut sw, mut sx, mut sy) = (0.0, 0.0, 0.0);
    for r in readings {
        let x = (r.tsf_ns - first.tsf_ns) as f64;
        let y = (r.offset_ns - first.offset_ns) as f64;
        sw += w(r);
        sx += w(r) * x;
        sy += w(r) * y;
    }
    let (mx, my) = (sx / sw, sy / sw);
    let (mut sxx, mut sxy) = (0.0, 0.0);
    for r in readings {
        let x = (r.tsf_ns - first.tsf_ns) as f64 - mx;
        let y = (r.offset_ns - first.offset_ns) as f64 - my;
        sxx += w(r) * x * x;
        sxy += w(r) * x * y;
    }
    let rate = if last.tsf_ns - first.tsf_ns >= MIN_DRIFT_SPAN_NS && sxx > 0.0 {
        sxy / sxx
    } else {
        0.0
    };
    let tsf_at = last.tsf_ns;
    let offset_ns = first.offset_ns as f64 + my + rate * ((tsf_at - first.tsf_ns) as f64 - mx);
    let mut line = TsfLine {
        tsf_at,
        offset_ns,
        rate,
        error_ns: 0,
        readings: readings.len(),
        clients: readings
            .iter()
            .map(|r| r.client)
            .collect::<HashSet<_>>()
            .len(),
        fitted_at: now,
    };
    // No better than its best recent reading, plus how far readings stray.
    let (mut swr, mut swrr) = (0.0, 0.0);
    for r in readings {
        let miss = (line.server_at(r.tsf_ns) - (r.tsf_ns + r.offset_ns)) as f64;
        swr += w(r);
        swrr += w(r) * miss * miss;
    }
    let best = readings
        .iter()
        .filter(|r| r.at >= now - RECENT_NS)
        .map(|r| r.error_ns)
        .min()
        .unwrap_or(last.error_ns);
    line.error_ns = best + (swrr / swr).sqrt().round() as i64;
    Some(line)
}

#[cfg(test)]
mod tests {
    use super::*;
    use bsp_proto::Uuid;

    const AP: Bssid = Bssid([4, 0xf4, 0x1c, 7, 0x93, 0x52]);

    /// What a client reports at server time `now` of an AP whose TSF is
    /// `server - 1000 s`, running 20 ppm fast, measured `off` ns wrong.
    fn reading(now: Timestamp, off: i64, error: i64) -> BeaconSync {
        let since = now - 5_000 * NANOS_PER_SEC;
        BeaconSync {
            bssid: AP,
            tsf_ns: now - 1_000 * NANOS_PER_SEC + since * 20 / 1_000_000,
            server_ns: now + off,
            server_error_ns: error,
            scatter_ns: 300,
            drift_ppm: -20.0,
            beacons: 120,
        }
    }

    fn truth(tsf: i64) -> Timestamp {
        // Inverse of the above, to well under a microsecond for these spans.
        let since = (tsf + 1_000 * NANOS_PER_SEC - 5_000 * NANOS_PER_SEC) as f64 / (1.0 + 20e-6);
        5_000 * NANOS_PER_SEC + since.round() as i64
    }

    #[test]
    fn pools_clients_into_one_line() {
        let clocks = BeaconClocks::default();
        let (a, b) = (Uuid::from_u128(1), Uuid::from_u128(2));
        let t0 = 5_000 * NANOS_PER_SEC;
        // Twenty minutes of heartbeats: client a measured 0.4 ms late (error
        // 1.5 ms), client b 0.1 ms early (error 0.5 ms).
        let mut now = t0;
        while now < t0 + 20 * 60 * NANOS_PER_SEC {
            clocks.push(a, &reading(now, 400_000, 1_500_000), now);
            clocks.push(b, &reading(now + 1, -100_000, 500_000), now + 1);
            now += 5 * NANOS_PER_SEC;
        }
        let line = clocks.line(AP).unwrap();
        // Fitted at the last whole minute: through all but the latest readings.
        assert_eq!(line.clients, 2);
        assert!((450..480).contains(&line.readings), "{}", line.readings);
        // Weighted 1:9 towards b: about 50 us early; the drift is the AP's.
        let tsf = reading(now, 0, 0).tsf_ns;
        let err = line.server_at(tsf) - truth(tsf);
        assert!((-80_000..=-20_000).contains(&err), "{err}");
        assert!(
            (line.drift_ppm() + 20.0).abs() < 0.01,
            "{}",
            line.drift_ppm()
        );
        assert!(
            line.error_ns >= 500_000 && line.error_ns < 800_000,
            "{}",
            line.error_ns
        );
        assert!(clocks.line(Bssid([0; 6])).is_none());
    }

    #[test]
    fn refits_once_a_minute_and_restarts_with_the_ap() {
        let clocks = BeaconClocks::default();
        let id = Uuid::from_u128(1);
        let t0 = 5_000 * NANOS_PER_SEC;
        clocks.push(id, &reading(t0, 0, 1_000_000), t0);
        let first = clocks.line(AP).unwrap();
        // A later, different reading does not move the line until a minute is up.
        clocks.push(
            id,
            &reading(t0 + NANOS_PER_SEC, 900_000, 1_000_000),
            t0 + NANOS_PER_SEC,
        );
        assert_eq!(clocks.line(AP).unwrap(), first);
        let t1 = t0 + 61 * NANOS_PER_SEC;
        clocks.push(id, &reading(t1, 0, 1_000_000), t1);
        assert_eq!(clocks.line(AP).unwrap().readings, 3);
        // The AP rebooted: its TSF is back near zero.
        let mut r = reading(t1 + NANOS_PER_SEC, 0, 1_000_000);
        r.tsf_ns = 3 * NANOS_PER_SEC;
        clocks.push(id, &r, t1 + NANOS_PER_SEC);
        let line = clocks.line(AP).unwrap();
        assert_eq!(line.readings, 1);
        assert_eq!(line.server_at(r.tsf_ns), r.server_ns);
    }
}
