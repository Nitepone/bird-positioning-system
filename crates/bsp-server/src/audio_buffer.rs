//! Recently received audio per client, so the locator can look at the same
//! absolute time span on every microphone (calls may straddle chunk boundaries).

use bsp_core::audio::AudioSample;
use bsp_proto::{ClientId, NANOS_PER_SEC, Timestamp};
use std::collections::{HashMap, VecDeque};
use std::sync::{Arc, Mutex};

pub struct AudioBuffers {
    retention_ns: i64,
    clients: Mutex<HashMap<ClientId, VecDeque<AudioSample>>>,
}

impl AudioBuffers {
    pub fn new(retention_s: u64) -> Self {
        Self {
            retention_ns: retention_s as i64 * NANOS_PER_SEC,
            clients: Mutex::default(),
        }
    }

    pub fn push(&self, sample: AudioSample) {
        let mut clients = self.clients.lock().unwrap();
        let q = clients.entry(sample.client_id).or_default();
        let newest = sample.end();
        let pos = q
            .iter()
            .position(|s| s.start > sample.start)
            .unwrap_or(q.len());
        q.insert(pos, sample);
        while q
            .front()
            .is_some_and(|s| s.end() < newest - self.retention_ns)
        {
            q.pop_front();
        }
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
        let chunks: Vec<&AudioSample> = clients
            .get(&client)?
            .iter()
            .filter(|s| s.end() > from && s.start < to)
            .collect();
        let sr = chunks.first()?.sample_rate;
        let len = ((to - from) as i128 * sr as i128 / NANOS_PER_SEC as i128) as usize;
        let mut out = vec![0.0f32; len];
        for c in chunks.iter().filter(|c| c.sample_rate == sr) {
            let offset = ((c.start - from) as i128 * sr as i128 / NANOS_PER_SEC as i128) as i64;
            for (i, &v) in c.pcm.iter().enumerate() {
                let j = offset + i as i64;
                if j >= 0 && (j as usize) < len {
                    out[j as usize] = v;
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
        let buf = AudioBuffers::new(60);
        let id = Uuid::nil();
        for k in 0..3 {
            buf.push(AudioSample {
                client_id: id,
                start: k * NANOS_PER_SEC,
                sample_rate: 10,
                pcm: vec![k as f32 + 1.0; 10].into(),
            });
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
}
