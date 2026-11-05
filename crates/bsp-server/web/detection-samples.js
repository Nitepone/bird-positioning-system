// Groups detections into the audio samples they were heard in, for the web UI's Detections page.
//
// Each stored clip is one monitor's audio around one detection's call. Clips of one monitor that
// overlap in time are one continuous recording (a "sample"), often holding several calls: the
// identifier reports each species it hears in a chunk, and calls in neighbouring windows overlap.
// A sample is shown once, with one numbered, coloured highlight per detection in it.
//
// Pure functions only, so they can be tested with node (detection-samples.test.js); in the
// browser they are globals, in node a module.
"use strict";

/// Highlight colours: categorical slots 1..8, drawn with CSS variables --hl-1..--hl-8 (light and
/// dark steps of one palette). A sample with more detections reuses them; numbers stay unique.
const SAMPLE_COLOURS = 8;
/// Longest sample put together; a monitor heard calls back to back for longer is shown as several.
const MAX_SAMPLE_NS = 60e9;
/// Silence shown around a call that has no stored audio (as clips have around theirs).
const NO_AUDIO_CONTEXT_NS = 0.5e9;

/// Groups `detections` (as /detections returns them: with `calls`, `clips`, best monitor first,
/// and `clip_spans`) into samples, newest first. With `opts.client`, only that monitor's audio is
/// used. Each detection is in at least one sample: monitors are picked greedily, the one whose
/// recording holds the most detections not yet shown first, and a picked recording shows every
/// detection in it. Detections without audio get a sample of their own (`client` null, `audio`
/// false). Every sample has `highlights` and `lanes` (see `layoutHighlights`).
function groupSamples(detections, opts = {}) {
  const maxSpan = opts.maxSpanNs || MAX_SAMPLE_NS;
  const byId = new Map(detections.map(d => [d.id, d]));
  // Clips by monitor, oldest first.
  const spans = new Map();
  for (const d of detections) {
    for (const s of d.clip_spans || []) {
      if (opts.client && s.client_id !== opts.client) continue;
      if (!(d.calls || []).some(c => c.client_id === s.client_id)) continue;
      if (!spans.has(s.client_id)) spans.set(s.client_id, []);
      spans.get(s.client_id).push({ id: d.id, start: s.start, end: Math.max(s.start, s.end) });
    }
  }
  // Each monitor's overlapping clips, joined into recordings.
  const recordings = [];
  for (const [client, list] of [...spans.entries()].sort((a, b) => a[0] < b[0] ? -1 : 1)) {
    list.sort((a, b) => a.start - b.start || a.end - b.end);
    let cur = null;
    for (const s of list) {
      if (!cur || s.start > cur.to || s.end - cur.from > maxSpan) {
        cur = { client, from: s.start, to: s.end, ids: [] };
        recordings.push(cur);
      }
      cur.to = Math.max(cur.to, s.end);
      if (!cur.ids.includes(s.id)) cur.ids.push(s.id);
    }
  }
  // How well a monitor heard a detection: its place in the detection's clip order.
  const rank = (id, client) => {
    const i = (byId.get(id).clips || []).indexOf(client);
    return i < 0 ? 99 : i;
  };
  const shown = new Set();
  const samples = [];
  for (;;) {
    let best = null, bestScore = null;
    for (const r of recordings) {
      const fresh = r.ids.filter(id => !shown.has(id));
      if (!fresh.length) continue;
      const score = [fresh.length, -fresh.reduce((sum, id) => sum + rank(id, r.client), 0)];
      if (!best || score[0] > bestScore[0] || (score[0] === bestScore[0] && score[1] > bestScore[1])) {
        best = r; bestScore = score;
      }
    }
    if (!best) break;
    for (const id of best.ids) shown.add(id);
    samples.push(makeSample(best.client, best.from, best.to, true, best.ids.map(id => {
      const d = byId.get(id);
      return { detection: d, call: d.calls.find(c => c.client_id === best.client) };
    })));
  }
  for (const d of detections) {
    if (shown.has(d.id) || !(d.calls || []).length) continue;
    const call = (opts.client && d.calls.find(c => c.client_id === opts.client)) || d.calls[0];
    samples.push(makeSample(null, call.start - NO_AUDIO_CONTEXT_NS, call.end + NO_AUDIO_CONTEXT_NS,
      false, [{ detection: d, call }]));
  }
  const newest = s => Math.max(...s.highlights.map(h => h.detection.time));
  return samples.sort((a, b) => newest(b) - newest(a) || b.to - a.to);
}

function makeSample(client, from, to, audio, members) {
  const { highlights, lanes } = layoutHighlights(members);
  const ids = highlights.map(h => h.detection.id).sort();
  return { key: (client || "none") + "/" + ids.join(","), client, from, to, audio, highlights, lanes };
}

/// Numbers a sample's detections 1..N in order of their calls' start (then end, then id), gives
/// each a colour slot (1-based, 1..SAMPLE_COLOURS, consecutive numbers differing) and a lane:
/// calls that overlap in time go in different lanes, so each stays visible; a call goes in the
/// lowest lane free at its start. `members` are { detection, call }.
function layoutHighlights(members) {
  const sorted = members.slice().sort((a, b) =>
    a.call.start - b.call.start || a.call.end - b.call.end ||
    (a.detection.id < b.detection.id ? -1 : a.detection.id > b.detection.id ? 1 : 0));
  const laneEnds = [];
  const highlights = sorted.map((m, i) => {
    const start = m.call.start, end = Math.max(m.call.start, m.call.end);
    let lane = laneEnds.findIndex(e => e <= start);
    if (lane < 0) { lane = laneEnds.length; laneEnds.push(end); } else laneEnds[lane] = end;
    return { n: i + 1, colour: i % SAMPLE_COLOURS + 1, lane, start, end, detection: m.detection, call: m.call };
  });
  return { highlights, lanes: Math.max(1, laneEnds.length) };
}

if (typeof module !== "undefined") {
  module.exports = { groupSamples, layoutHighlights, SAMPLE_COLOURS, MAX_SAMPLE_NS, NO_AUDIO_CONTEXT_NS };
}
