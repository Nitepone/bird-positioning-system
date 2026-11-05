// Tests for detection-samples.js: `node --test crates/bsp-server/web/` (also run by `cargo test`).
"use strict";
const test = require("node:test");
const assert = require("node:assert/strict");
const { groupSamples, layoutHighlights, SAMPLE_COLOURS } = require("./detection-samples.js");

const S = 1e9;
const T0 = 1.79e18; // a realistic timestamp, past 2^53 like the server's

/// A detection heard by `calls`: [client, startS, endS] (seconds after T0); every client listed
/// in `clips` has a clip from 0.5 s before its call to 0.5 s after.
function det(id, calls, clips = calls.map(c => c[0]), confidence = 0.8) {
  const cs = calls.map(([client, a, b]) => ({ client_id: client, start: T0 + a * S, end: T0 + b * S, confidence }));
  return {
    id, time: Math.min(...cs.map(c => c.start)), confidence, species: { common: id, scientific: id },
    calls: cs, clips,
    clip_spans: clips.map(client => {
      const c = cs.find(c => c.client_id === client);
      return { client_id: client, start: c.start - 0.5 * S, end: c.end + 0.5 * S };
    }),
  };
}

/// Checks the invariants every sample must hold, and returns its highlights' numbers.
function checkSample(s) {
  const ns = s.highlights.map(h => h.n);
  assert.deepEqual(ns, ns.map((_, i) => i + 1), "numbered 1..N without gaps or repeats");
  for (const h of s.highlights) {
    assert.ok(h.colour >= 1 && h.colour <= SAMPLE_COLOURS);
    assert.ok(h.lane >= 0 && h.lane < s.lanes);
    if (s.audio) assert.ok(h.start >= s.from && h.end <= s.to, "inside the sample");
  }
  // Highlights in one lane never overlap.
  for (const a of s.highlights) {
    for (const b of s.highlights) {
      if (a !== b && a.lane === b.lane) assert.ok(a.end <= b.start || b.end <= a.start, `${a.n} and ${b.n} overlap`);
    }
  }
  return ns;
}

test("no detections: no samples, and a sample without detections has no highlights", () => {
  assert.deepEqual(groupSamples([]), []);
  assert.deepEqual(layoutHighlights([]), { highlights: [], lanes: 1 });
});

test("one chirp: one sample, one highlight covering the call", () => {
  const [s, ...rest] = groupSamples([det("a", [["m1", 2, 5]])]);
  assert.equal(rest.length, 0);
  assert.equal(s.client, "m1");
  assert.ok(s.audio);
  assert.equal(s.from, T0 + 1.5 * S);
  assert.equal(s.to, T0 + 5.5 * S);
  assert.deepEqual(checkSample(s), [1]);
  assert.equal(s.highlights[0].start, T0 + 2 * S);
  assert.equal(s.highlights[0].end, T0 + 5 * S);
  assert.equal(s.highlights[0].detection.id, "a");
});

test("three chirps in one recording: numbered by start, three colours, one lane", () => {
  // Listed newest first, as the API returns them.
  const ds = [det("c", [["m1", 7, 8]]), det("b", [["m1", 4, 6.8]]), det("a", [["m1", 1, 3.6]])];
  const samples = groupSamples(ds);
  assert.equal(samples.length, 1);
  const s = samples[0];
  assert.deepEqual(checkSample(s), [1, 2, 3]);
  assert.deepEqual(s.highlights.map(h => h.detection.id), ["a", "b", "c"]);
  assert.equal(new Set(s.highlights.map(h => h.colour)).size, 3);
  assert.equal(s.lanes, 1);
  assert.equal(s.from, T0 + 0.5 * S);
  assert.equal(s.to, T0 + 8.5 * S);
});

test("overlapping chirps go in separate lanes", () => {
  // BirdNET's 3 s windows: two species in the same window, a third overlapping one of them.
  const ds = [det("x", [["m1", 0, 3]]), det("y", [["m1", 0, 3]]), det("z", [["m1", 1.5, 4.5]]), det("w", [["m1", 3, 6]])];
  const [s] = groupSamples(ds);
  assert.deepEqual(checkSample(s), [1, 2, 3, 4]);
  const lane = Object.fromEntries(s.highlights.map(h => [h.detection.id, h.lane]));
  assert.equal(s.lanes, 3);
  assert.deepEqual(lane, { x: 0, y: 1, z: 2, w: 0 }); // w starts as x ends: x's lane is free again
  assert.equal(new Set(s.highlights.map(h => h.colour)).size, 4);
});

test("colours repeat only past the palette, numbers never", () => {
  const ds = Array.from({ length: 10 }, (_, i) => det("d" + i, [["m1", i, i + 0.5]]));
  const [s] = groupSamples(ds);
  assert.deepEqual(checkSample(s), [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]);
  assert.deepEqual(s.highlights.map(h => h.colour), [1, 2, 3, 4, 5, 6, 7, 8, 1, 2]);
});

test("recordings apart in time, or of other monitors, are separate samples", () => {
  const ds = [det("late", [["m1", 20, 21]]), det("other", [["m2", 0, 1]]), det("early", [["m1", 0, 1]])];
  const samples = groupSamples(ds);
  assert.equal(samples.length, 3);
  assert.deepEqual(samples.map(s => s.highlights.map(h => h.detection.id)).sort(), [["early"], ["late"], ["other"]]);
  assert.equal(samples[0].highlights[0].detection.id, "late"); // newest first
  assert.deepEqual(samples.map(s => s.highlights.length), [1, 1, 1]);
  for (const s of samples) checkSample(s);
  // Clips touching end to start are one recording.
  assert.equal(groupSamples([det("a", [["m1", 0, 1]]), det("b", [["m1", 2, 3]])]).length, 1);
});

test("a detection heard by several monitors is shown once, on the monitor hearing most", () => {
  const ds = [
    det("both1", [["m1", 0, 3], ["m2", 0.01, 3.01]], ["m2", "m1"]),
    det("both2", [["m1", 2, 4], ["m2", 2.01, 4.01]], ["m1", "m2"]),
    det("m2only", [["m2", 3, 5]]),
  ];
  const samples = groupSamples(ds);
  assert.equal(samples.length, 1);
  assert.equal(samples[0].client, "m2");
  assert.deepEqual(checkSample(samples[0]), [1, 2, 3]);
  // With a monitor chosen, its audio only.
  const m1 = groupSamples(ds, { client: "m1" });
  assert.deepEqual(m1.map(s => [s.client, s.audio, s.highlights.length]), [[null, false, 1], ["m1", true, 2]]);
  // The m2-only detection, with no m1 audio, gets a timeline of its own.
  assert.equal(m1[0].highlights[0].detection.id, "m2only");
});

test("detections without audio get a sample of their own", () => {
  const d = det("silent", [["m1", 0, 3]], []);
  const [s, ...rest] = groupSamples([d]);
  assert.equal(rest.length, 0);
  assert.equal(s.audio, false);
  assert.equal(s.client, null);
  assert.deepEqual(checkSample(s), [1]);
  assert.ok(s.from < s.highlights[0].start && s.to > s.highlights[0].end);
});

test("long runs of calls are split into samples of bounded length", () => {
  const ds = Array.from({ length: 30 }, (_, i) => det("d" + i, [["m1", i * 3, i * 3 + 3]]));
  const samples = groupSamples(ds, { maxSpanNs: 20 * S });
  assert.ok(samples.length > 1);
  for (const s of samples) {
    checkSample(s);
    assert.ok(s.to - s.from <= 20 * S + 1);
  }
  const all = samples.flatMap(s => s.highlights.map(h => h.detection.id));
  assert.equal(new Set(all).size, 30);
});
