//! Recently received audio per client, so the locator can look at the same
//! absolute time span on every microphone (calls may straddle chunk boundaries).

use crate::beacon_clock::BeaconClocks;
use bsp_core::audio::AudioSample;
use bsp_proto::{Bssid, ClientId, NANOS_PER_SEC, Timestamp};
use std::collections::{HashMap, VecDeque};
use std::sync::{Arc, Mutex};

/// A chunk's start and end on its access point's clock (TSF, ns), from a
/// client that follows the AP's beacons.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct TsfStamp {
    pub bssid: Bssid,
    pub start: i64,
    pub end: i64,
}

struct Stored {
    sample: AudioSample,
    tsf: Option<TsfStamp>,
}

pub struct AudioBuffers {
    retention_ns: i64,
    clocks: Arc<BeaconClocks>,
    clients: Mutex<HashMap<ClientId, VecDeque<Stored>>>,
}

impl AudioBuffers {
    pub fn new(retention_s: u64, clocks: Arc<BeaconClocks>) -> Self {
        Self {
            retention_ns: retention_s as i64 * NANOS_PER_SEC,
            clocks,
            clients: Mutex::default(),
        }
    }

    pub fn push(&self, sample: AudioSample, tsf: Option<TsfStamp>) {
        let mut clients = self.clients.lock().unwrap();
        let q = clients.entry(sample.client_id).or_default();
        let newest = sample.end();
        let pos = q
            .iter()
            .position(|s| s.sample.start > sample.start)
            .unwrap_or(q.len());
        q.insert(pos, Stored { sample, tsf });
        while q
            .front()
            .is_some_and(|s| s.sample.end() < newest - self.retention_ns)
        {
            q.pop_front();
        }
    }

    /// The chunk on the server clock as of now: chunks stamped on an access
    /// point's clock are converted with its current line, so all of that
    /// AP's clients are converted alike.
    fn current(&self, s: &Stored) -> AudioSample {
        let mut sample = s.sample.clone();
        let Some(t) = s.tsf else { return sample };
        let Some(line) = self.clocks.line(t.bssid) else {
            return sample;
        };
        let (start, end) = (line.server_at(t.start), line.server_at(t.end));
        if let Some(ppb) =
            AudioSample::clock_ppb_from(sample.sample_rate, sample.pcm.len(), start, end)
        {
            sample.start = start;
            sample.clock_ppb = ppb;
        }
        sample
    }

    /// Audio for `client` covering `[from, to)`, stitched from the stored
    /// chunks at the sample rate of the first one; gaps are zero-filled.
    /// Returns `(start, sample_rate, pcm)` or `None` if nothing overlaps.
    pub fn extract(
        &self,
        client: ClientId,
        from: Timestamp,
        to: Timestamp,
    ) -> Option<(Timestamp, u32, Arc<[f32]>)> {
        let clients = self.clients.lock().unwrap();
        let chunks: Vec<AudioSample> = clients
            .get(&client)?
            .iter()
            .map(|s| self.current(s))
            .filter(|s| s.end() > from && s.start < to)
            .collect();
        let sr = chunks.first()?.sample_rate;
        let len = ((to - from) as i128 * sr as i128 / NANOS_PER_SEC as i128) as usize;
        let mut out = vec![0.0f32; len];
        for c in chunks.iter().filter(|c| c.sample_rate == sr) {
            // Each sample goes where its own time falls, so a chunk whose clock
            // ran fast or slow is not smeared out of place along its length.
            let first = c.index_of(from).floor().max(0.0) as usize;
            let last = (c.index_of(to).ceil().max(0.0) as usize).min(c.pcm.len());
            for i in first..last {
                let t = c.time_at(i) - from;
                let j = (t as i128 * sr as i128 + NANOS_PER_SEC as i128 / 2)
                    .div_euclid(NANOS_PER_SEC as i128);
                if j >= 0 && (j as usize) < len {
                    out[j as usize] = c.pcm[i];
                }
            }
        }
        Some((from, sr, out.into()))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use bsp_proto::Uuid;

    #[test]
    fn stitches_chunks() {
        let buf = AudioBuffers::new(60, Default::default());
        let id = Uuid::nil();
        for k in 0..3 {
            buf.push(
                AudioSample {
                    client_id: id,
                    start: k * NANOS_PER_SEC,
                    sample_rate: 10,
                    pcm: vec![k as f32 + 1.0; 10].into(),
                    clock_ppb: 0,
                },
                None,
            );
        }
        let (start, sr, pcm) = buf
            .extract(id, NANOS_PER_SEC / 2, 5 * NANOS_PER_SEC / 2)
            .unwrap();
        assert_eq!((start, sr, pcm.len()), (NANOS_PER_SEC / 2, 10, 20));
        assert_eq!(pcm[0], 1.0);
        assert_eq!(pcm[5], 2.0);
        assert_eq!(pcm[19], 3.0);
        assert!(
            buf.extract(id, 10 * NANOS_PER_SEC, 11 * NANOS_PER_SEC)
                .is_none()
        );
    }

    #[test]
    fn converts_audio_stamped_on_an_access_points_clock() {
        use bsp_proto::BeaconSync;
        let clocks = Arc::new(BeaconClocks::default());
        let buf = AudioBuffers::new(60, clocks.clone());
        let id = Uuid::nil();
        let ap = Bssid([2, 0, 0, 0, 0, 1]);
        let tsf = TsfStamp {
            bssid: ap,
            start: 1_000 * NANOS_PER_SEC,
            end: 1_001 * NANOS_PER_SEC,
        };
        let pcm: Vec<f32> = (0..1000).map(|i| i as f32).collect();
        // Sent with a start of 0 on the client's own clock; no line yet.
        let sample = AudioSample {
            client_id: id,
            start: 0,
            sample_rate: 1000,
            pcm: pcm.into(),
            clock_ppb: 0,
        };
        buf.push(sample, Some(tsf));
        assert_eq!(buf.extract(id, 0, NANOS_PER_SEC).unwrap().2[0], 0.0);
        // Then the AP's TSF turns out to be 5 s behind the server clock.
        let b = BeaconSync {
            bssid: ap,
            tsf_ns: 1_000 * NANOS_PER_SEC,
            server_ns: 1_005 * NANOS_PER_SEC,
            server_error_ns: 1_000_000,
            scatter_ns: 300,
            drift_ppm: 0.0,
            beacons: 100,
        };
        clocks.push(id, &b, 1_005 * NANOS_PER_SEC);
        let (_, _, out) = buf
            .extract(
                id,
                1_005 * NANOS_PER_SEC + 500 * NANOS_PER_SEC / 1000,
                1_006 * NANOS_PER_SEC,
            )
            .unwrap();
        assert_eq!(out[0], 500.0);
    }

    #[test]
    fn places_samples_by_their_clock() {
        let buf = AudioBuffers::new(60, Default::default());
        let id = Uuid::nil();
        // 1000 samples at a nominal 1 kHz whose clock ran 1% slow: they span 1.01 s.
        let pcm: Vec<f32> = (0..1000).map(|i| i as f32).collect();
        buf.push(
            AudioSample {
                client_id: id,
                start: 0,
                sample_rate: 1000,
                pcm: pcm.into(),
                clock_ppb: 10_000_000,
            },
            None,
        );
        let (_, _, out) = buf
            .extract(id, 800 * NANOS_PER_SEC / 1000, NANOS_PER_SEC)
            .unwrap();
        // Sample 800 was captured at 808 ms; at 800 ms is sample 792.
        assert_eq!(out[0], 792.0);
        assert_eq!(out[8], 800.0);
    }
}
