// node --test lib.test.js
"use strict";
const test = require("node:test");
const assert = require("node:assert/strict");
const L = require("./lib.js");

const at = (y, mo, d, h = 0, mi = 0, s = 0, ms = 0) => new Date(y, mo - 1, d, h, mi, s, ms).getTime();
const ns = ms => ms * 1e6;

test("times use one 24-hour format", () => {
  assert.equal(L.fmtClock(at(2026, 11, 13, 8, 5)), "08:05");
  assert.equal(L.fmtClock(at(2026, 11, 13, 20, 5, 9), true), "20:05:09");
  assert.equal(L.fmtClock(at(2026, 11, 13, 0, 14, 40, 650), false, true), "00:14:40.6");
  const now = at(2026, 11, 13, 12);
  assert.equal(L.fmtStamp(ns(at(2026, 11, 13, 0, 14, 40)), now), "00:14:40");
  assert.equal(L.fmtStamp(ns(at(2026, 11, 12, 23, 28, 36)), now), "12 Nov 23:28");
  assert.equal(L.fmtStamp(ns(at(2025, 3, 2, 7, 1)), now), "2 Mar 2025 07:01");
  assert.equal(L.fmtFull(ns(at(2026, 11, 13, 8, 12, 5))), "2026-11-13 08:12:05");
});

test("days are named relative to today", () => {
  const now = at(2026, 11, 13, 0, 30);
  assert.equal(L.fmtDay(at(2026, 11, 13, 0, 1), now), "Today");
  assert.equal(L.fmtDay(at(2026, 11, 12, 23, 59), now), "Yesterday");
  assert.equal(L.fmtDay(at(2026, 11, 9), now), "Mon 9 Nov");
  assert.equal(L.fmtDay(at(2025, 12, 31), now), "Wed 31 Dec 2025");
  assert.equal(L.shiftDayKey("2026-03-01", -1), "2026-02-28");
  assert.equal(L.dayEndMs("2026-11-13") - L.dayStartMs("2026-11-13"), 86400e3);
});

test("ages and durations", () => {
  const now = at(2026, 11, 13, 12);
  const ago = s => L.fmtAgo(ns(now - s * 1000), now);
  assert.deepEqual([1, 12, 300, 7200, 2 * 86400].map(ago), ["now", "12 s ago", "5 min ago", "2 h ago", "2 d ago"]);
  assert.equal(ago(9 * 86400), "4 Nov 12:00");
  assert.equal(L.fmtAgo(0, now), "never");
  assert.deepEqual([65, 3725, 90000].map(L.fmtDuration), ["1m 05s", "1h 02m", "1d 1h"]);
  assert.deepEqual([300, 90].map(L.fmtInterval), ["5 min", "90 s"]);
  assert.deepEqual([512, 2048, 348019].map(L.fmtBytes), ["512 B", "2.0 KB", "340 KB"]);
});

test("short ids grow until they tell the clients apart", () => {
  const a = "44bd8d76-e0dc-8000-8062-70732d6d6375", b = "44bd8d76-ddd8-8000-8062-70732d6d6375", c = "aca7042c-b3f8-8000-8062-70732d6d6375";
  assert.deepEqual([...L.shortIds([c, "d83bda46-cf70-8000-8062-70732d6d6375"]).values()], ["aca7042c", "d83bda46"]);
  const s = L.shortIds([a, b, c]);
  assert.equal(s.get(a), "44bd8d76-e");
  assert.equal(s.get(b), "44bd8d76-d");
  assert.equal(s.get(c), "aca7042c-b");
  assert.equal(L.shortIds([a, a]).get(a), "44bd8d76");
});

test("a client is called by its name, suggestion, hostname, then id", () => {
  const c = (id, extra) => ({ id, name: "", suggested_name: "", hostname: "", ...extra });
  const labels = L.clientLabels([
    c("44bd8d76-e0dc-8000-8062-70732d6d6375", { hostname: "bps-bps-xiao-76e0dc" }),
    c("44bd8d76-ddd8-8000-8062-70732d6d6375"),
    c("aca7042c-b3f8-8000-8062-70732d6d6375", { name: "North fence", hostname: "bps-pico-7042c0" }),
    c("d83bda46-cf70-8000-8062-70732d6d6375", { suggested_name: "Pond" }),
  ]);
  assert.deepEqual([...labels.values()], ["bps-xiao-76e0dc", "44bd8d76-d", "North fence", "Pond"]);
  // Two clients with one name stay distinguishable.
  const twins = L.clientLabels([c("11111111-0000-0000-0000-000000000000", { name: "Gate" }), c("22222222-0000-0000-0000-000000000000", { name: "Gate" })]);
  assert.deepEqual([...twins.values()], ["Gate (11111111)", "Gate (22222222)"]);
  assert.equal(L.cleanHostname("bps-bps-xiao-76e0dc"), "bps-xiao-76e0dc");
  assert.equal(L.cleanHostname("web: Linux"), "web: Linux");
});

test("device state and issues", () => {
  const ok = { active: true, synced: true, api_compat: "compatible", health: { devices: [{ kind: "sd", present: true }], battery: { percent: 80 } } };
  assert.deepEqual(L.deviceState(ok), { level: "ok", text: "listening" });
  assert.deepEqual(L.deviceIssues(ok), []);
  assert.equal(L.deviceState({ ...ok, active: false }).level, "off");
  assert.equal(L.deviceState({ ...ok, synced: false }).level, "bad");
  const sick = {
    active: true, synced: false, api_compat: "legacy", positioning_requested: true,
    health: { devices: [{ kind: "sd", present: false }], battery: { percent: 12, charging: false }, storage: { present: true, free_bytes: 1000, errors: 2 } },
    last_reboot: { reason: "panic" }, firmware: { last: { result: "rolled_back" }, offering: 3 },
  };
  const issues = L.deviceIssues(sick);
  assert.deepEqual(issues.map(i => i.level), ["bad", "bad", "bad", "warn", "warn", "warn", "warn", "warn", "info", "info"]);
  assert.match(issues[0].text, /clock is not synced/);
  assert.ok(issues.some(i => i.text === "Not found: sd" && i.tab === "overview"));
  // A low battery that is charging is not a problem; an offline device's missing parts are not news.
  assert.deepEqual(L.deviceIssues({ ...ok, health: { battery: { percent: 5, charging: true } } }), []);
  assert.deepEqual(L.deviceIssues({ active: false, synced: false, health: { devices: [{ kind: "sd", present: false }] } }), []);
});

test("events are grouped and put into words", () => {
  assert.equal(L.eventGroup("heartbeat"), "heartbeat");
  assert.equal(L.eventGroup("firmware_offered"), "firmware");
  assert.equal(L.eventGroup("something_new"), "other");
  const kinds = L.EVENT_GROUPS.flatMap(g => g.kinds);
  assert.equal(new Set(kinds).size, kinds.length, "a kind is in one group");
  const s = (kind, detail) => L.eventSummary({ kind, detail });
  assert.equal(s("audio", { bytes: 348019, duration_s: 9.0001, latency_ms: 24, sample_rate: 48000 }), "9.0 s of audio, 340 KB, 48 kHz, 24 ms after it ended");
  assert.equal(s("client_log", { ms: 5, text: "upload: sent" }), "upload: sent");
  assert.equal(s("identified", { calls: 1, species: [["Bee Hummingbird", 0.9101]] }), "Heard Bee Hummingbird (0.91)");
  assert.equal(s("command_acked", { command: "logging", result: "done" }), 'The device answered "logging": done');
  assert.equal(s("logging_requested", { log_level: "on", log_duration_s: 600 }), "Trace logging asked on for 10m 00s");
  assert.equal(s("client_rebooted", { reason: "power_on" }), "Started (power on)");
  assert.equal(s("clock_synced", { clock: { error_ns: 1910818 } }), "Clock synced, error within 1.91 ms");
  assert.equal(s("firmware_offered", { build_id: 2, describe: "v9.9", from: "3c48fb8" }), "Offered v9.9 (it runs 3c48fb8)");
  assert.equal(s("capture_dropout", { count: 1 }), "1 gap in the captured audio");
  // An unknown kind shows its plain fields, and never its nested ones.
  assert.equal(s("brand_new", { count: 2, why: "x", nested: { a: 1 } }), "count 2, why x");
  assert.equal(s("brand_new", {}), "brand new");
  assert.equal(L.eventLevel({ kind: "clock_unsynced" }), "bad");
  assert.equal(L.eventLevel({ kind: "client_rebooted", detail: { reason: "panic" } }), "warn");
  assert.equal(L.eventLevel({ kind: "command_acked", detail: { result: "unsupported" } }), "warn");
  assert.equal(L.eventLevel({ kind: "register" }), "");
});

test("a day's detections group into species lanes", () => {
  const d = (scientific, common, time, extra) => ({ scientific, common, time, end: time, confidence: 0.8, clients: ["a"], occurrence: "expected", ...extra });
  const dets = [
    d("Corvus", "Crow", 300), d("Pica", "Magpie", 100, { clients: ["b"] }), d("Corvus", "Crow", 200, { confidence: 0.95, clients: ["b"] }),
    d("Mellisuga", "Bee Hummingbird", 150, { occurrence: "unexpected", muted: true }), d("Engine", "Engine", 50, { occurrence: "unknown" }),
  ];
  const lanes = L.speciesLanes(dets);
  assert.deepEqual(lanes.map(l => l.common), ["Engine", "Magpie", "Bee Hummingbird", "Crow"]);
  const crow = lanes[3];
  assert.deepEqual([crow.items.length, crow.first, crow.last, crow.best, [...crow.clients].sort().join()], [2, 200, 300, 0.95, "a,b"]);
  assert.deepEqual(L.speciesLanes(dets, "count").map(l => l.common)[0], "Crow");
  assert.deepEqual(L.speciesLanes(dets, "name").map(l => l.common), ["Bee Hummingbird", "Crow", "Engine", "Magpie"]);
  const shown = mode => lanes.filter(l => L.laneShown(l, mode)).map(l => l.common);
  assert.deepEqual(shown("expected"), ["Magpie", "Crow"]);
  assert.deepEqual(shown("known"), ["Magpie", "Crow"], "a muted species stays hidden");
  assert.deepEqual(shown("all"), ["Engine", "Magpie", "Crow"]);
});
