//! Groups calls heard by different clients into detections and runs the locator.

use crate::clip_audio;
use crate::db::Clip;
use crate::settings::ConfidenceLevel;
use crate::state::SharedState;
use bsp_core::detection::Detection;
use bsp_core::identifier::Call;
use bsp_core::locator::{Locator, LocatorInput, MIN_MICS};
use bsp_proto::{NANOS_PER_MS, NANOS_PER_SEC, Timestamp, Uuid, now_ns};
use std::collections::HashMap;
use std::sync::Arc;
use std::time::Duration;
use tokio::sync::mpsc;

/// Context included around a group's calls when handing audio to the locator.
const LOCATOR_CONTEXT_NS: i64 = 500 * NANOS_PER_MS;
/// Context kept around each call in the stored playback clips.
const CLIP_CONTEXT_NS: i64 = 500 * NANOS_PER_MS;
/// Upper bound on a stored clip's length.
const MAX_CLIP_NS: i64 = 10 * NANOS_PER_SEC;

pub async fn run(
    state: SharedState,
    mut rx: mpsc::UnboundedReceiver<Vec<Call>>,
    locator: Arc<dyn Locator>,
) {
    let mut pending: Vec<Pending> = Vec::new();
    let mut tick = tokio::time::interval(Duration::from_secs(1));
    let window = state.cfg.group_window_s as i64 * NANOS_PER_SEC;
    let skew = state.cfg.max_call_skew_ms * NANOS_PER_MS;
    loop {
        tokio::select! {
            calls = rx.recv() => match calls {
                Some(calls) => {
                    let received = now_ns();
                    pending.extend(calls.into_iter().map(|call| Pending { call, received }));
                }
                None => return,
            },
            _ = tick.tick() => {
                for group in take_ready_groups(&mut pending, skew, now_ns() - window) {
                    let d = build_detection(&state, group, locator.clone()).await;
                    // The identifier's thresholds normally see to this already.
                    if d.confidence < ConfidenceLevel::MIN {
                        tracing::debug!(species = %d.species.common, confidence = d.confidence,
                                        "dropping low-confidence detection");
                        continue;
                    }
                    tracing::info!(
                        species = %d.species.common, mics = d.calls.len(),
                        bearing = ?d.location.as_ref().map(|l| l.bearing_deg), "detection"
                    );
                    if let Err(e) = state.db.insert_detection(&d) {
                        tracing::error!("failed to store detection: {e}");
                        continue;
                    }
                    let state = state.clone();
                    tokio::task::spawn_blocking(move || store_clips(&state, &d));
                }
            }
        }
    }
}

/// A call waiting for matching calls from other clients.
pub struct Pending {
    pub call: Call,
    /// When the server received it. Clients upload whole chunks, so a call
    /// can arrive long after it started; waiting is measured from arrival.
    pub received: Timestamp,
}

/// Removes and returns groups whose earliest call was received before `cutoff`.
/// A group holds calls of one species, at most one per client, all starting
/// within `skew` of the group's earliest call.
pub fn take_ready_groups(
    pending: &mut Vec<Pending>,
    skew: i64,
    cutoff: Timestamp,
) -> Vec<Vec<Call>> {
    pending.sort_by_key(|p| p.call.start);
    let mut groups = Vec::new();
    while pending.first().is_some_and(|p| p.received < cutoff) {
        let anchor = pending.remove(0).call;
        let mut group = vec![anchor];
        let mut i = 0;
        while i < pending.len() && pending[i].call.start - group[0].start <= skew {
            if pending[i].call.species != group[0].species {
                i += 1;
                continue;
            }
            let c = pending.remove(i).call;
            match group.iter_mut().find(|g| g.client_id == c.client_id) {
                // Same client twice: one call reported by overlapping windows.
                Some(g) => {
                    if c.confidence > g.confidence {
                        g.confidence = c.confidence;
                    }
                    g.end = g.end.max(c.end);
                }
                None => group.push(c),
            }
        }
        groups.push(group);
    }
    groups
}

/// Saves each client's audio around its call, for playback in the web UI.
fn store_clips(state: &SharedState, d: &Detection) {
    for call in &d.calls {
        let from = call.start - CLIP_CONTEXT_NS;
        let to = (call.end + CLIP_CONTEXT_NS).min(from + MAX_CLIP_NS);
        let Some((start, sample_rate, pcm)) = state.audio.extract(call.client_id, from, to) else {
            continue;
        };
        let clip = Clip {
            start,
            audio: clip_audio::encode(sample_rate, &pcm),
        };
        if let Err(e) = state.db.insert_clip(d.id, call.client_id, &clip) {
            tracing::error!("failed to store clip: {e}");
        }
    }
}

async fn build_detection(
    state: &SharedState,
    calls: Vec<Call>,
    locator: Arc<dyn Locator>,
) -> Detection {
    let now = now_ns();
    let time = calls.iter().map(|c| c.start).min().unwrap_or(now);
    let end = calls.iter().map(|c| c.end).max().unwrap_or(now);
    let clients: HashMap<_, _> = match state.db.list_clients() {
        Ok(list) => list.into_iter().map(|c| (c.id, c)).collect(),
        Err(e) => {
            tracing::error!("listing clients: {e}");
            HashMap::new()
        }
    };

    let inputs: Vec<LocatorInput> = calls
        .iter()
        .filter_map(|call| {
            let client = clients.get(&call.client_id)?;
            let position = client.position?;
            if !state.is_synced(client, now) {
                return None;
            }
            let (start, sample_rate, pcm) = state.audio.extract(
                call.client_id,
                time - LOCATOR_CONTEXT_NS,
                end + LOCATOR_CONTEXT_NS,
            )?;
            Some(LocatorInput {
                client_id: call.client_id,
                position,
                call: call.clone(),
                start,
                sample_rate,
                pcm,
            })
        })
        .collect();

    let location = if inputs.len() >= MIN_MICS {
        tokio::task::spawn_blocking(move || locator.locate(&inputs))
            .await
            .unwrap_or_else(|e| {
                tracing::error!("locator panicked: {e}");
                None
            })
    } else {
        tracing::debug!(
            usable = inputs.len(),
            heard = calls.len(),
            "not enough positioned, synced clients to locate"
        );
        None
    };

    Detection {
        id: Uuid::new_v4(),
        time,
        species: calls[0].species.clone(),
        confidence: calls.iter().map(|c| c.confidence).fold(0.0, f32::max),
        unexpected: calls.iter().any(|c| c.unexpected),
        calls,
        location,
        created_at: now,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use bsp_core::identifier::Species;

    /// A call that started and was received at `start_ms`.
    fn call(client: u128, species: &str, start_ms: i64) -> Pending {
        let call = Call {
            client_id: Uuid::from_u128(client),
            species: Species {
                scientific: species.into(),
                common: species.into(),
            },
            confidence: 0.8,
            start: start_ms * NANOS_PER_MS,
            end: (start_ms + 300) * NANOS_PER_MS,
            unexpected: false,
        };
        Pending {
            call,
            received: start_ms * NANOS_PER_MS,
        }
    }

    #[test]
    fn groups_within_skew() {
        let skew = 200 * NANOS_PER_MS;
        let mut pending = vec![
            call(1, "a", 1000),
            call(2, "a", 1150),
            call(3, "a", 1200), // exactly at the limit
            call(4, "a", 1201), // just outside
            call(2, "b", 1050), // other species
            call(1, "a", 1100), // same client again -> merged
            call(5, "a", 9000), // not ready yet
        ];
        let groups = take_ready_groups(&mut pending, skew, 5000 * NANOS_PER_MS);
        let summary: Vec<Vec<u128>> = groups
            .iter()
            .map(|g| g.iter().map(|c| c.client_id.as_u128()).collect())
            .collect();
        assert_eq!(summary, vec![vec![1, 2, 3], vec![2], vec![4]]);
        assert_eq!(pending.len(), 1);
    }

    #[test]
    fn waits_from_arrival_not_call_start() {
        let skew = 200 * NANOS_PER_MS;
        // An old call that only just arrived must wait for the other clients' uploads.
        let mut late = call(1, "a", 1000);
        late.received = 9000 * NANOS_PER_MS;
        let mut pending = vec![late];
        assert!(take_ready_groups(&mut pending, skew, 5000 * NANOS_PER_MS).is_empty());
        pending.push(call(2, "a", 1100));
        let groups = take_ready_groups(&mut pending, skew, 10_000 * NANOS_PER_MS);
        assert_eq!(groups.len(), 1);
        assert_eq!(groups[0].len(), 2);
    }
}
