// Shared by both workspaces of the web UI (Birds and System): how things are named, formatted and
// summarised. Pure functions only, so they can be tested with node (lib.test.js); in the browser
// they are globals, in node a module.
"use strict";

// ---- text ----

function esc(s) {
  return String(s ?? "").replace(/[&<>"']/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
}
function plural(n, one, many) { return `${n} ${n === 1 ? one : (many || one + "s")}`; }

// ---- time: one format everywhere (24-hour clock, day before month) ----

const WEEKDAYS = ["Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"];
const MONTHS = ["Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"];
const pad2 = n => String(n).padStart(2, "0");

/// A local calendar day as "YYYY-MM-DD" (what date inputs use).
function dateKey(d) { return `${d.getFullYear()}-${pad2(d.getMonth() + 1)}-${pad2(d.getDate())}`; }
function dayStartMs(key) { const [y, m, d] = key.split("-").map(Number); return new Date(y, m - 1, d).getTime(); }
function dayEndMs(key) { const [y, m, d] = key.split("-").map(Number); return new Date(y, m - 1, d + 1).getTime(); }
function shiftDayKey(key, delta) { const [y, m, d] = key.split("-").map(Number); return dateKey(new Date(y, m - 1, d + delta)); }
const msToNs = ms => Math.round(ms) * 1e6;

/// "08:12", or "08:12:05" with `seconds`, or "08:12:05.3" with `tenths`.
function fmtClock(ms, seconds, tenths) {
  const d = new Date(ms);
  let s = `${pad2(d.getHours())}:${pad2(d.getMinutes())}`;
  if (seconds || tenths) s += `:${pad2(d.getSeconds())}`;
  if (tenths) s += `.${Math.floor(d.getMilliseconds() / 100)}`;
  return s;
}
/// "Today", "Yesterday", or "Fri 13 Nov" (with the year when it is not this one).
function fmtDay(ms, nowMs) {
  const d = new Date(ms), now = new Date(nowMs ?? Date.now());
  const key = dateKey(d);
  if (key === dateKey(now)) return "Today";
  if (key === shiftDayKey(dateKey(now), -1)) return "Yesterday";
  return `${WEEKDAYS[d.getDay()]} ${d.getDate()} ${MONTHS[d.getMonth()]}` + (d.getFullYear() === now.getFullYear() ? "" : ` ${d.getFullYear()}`);
}
/// A moment, as short as stays unambiguous: "08:12:05" today, else "13 Nov 08:12".
function fmtStamp(ns, nowMs) {
  if (!ns) return "";
  const ms = ns / 1e6, d = new Date(ms), now = new Date(nowMs ?? Date.now());
  if (dateKey(d) === dateKey(now)) return fmtClock(ms, true);
  return `${d.getDate()} ${MONTHS[d.getMonth()]}` + (d.getFullYear() === now.getFullYear() ? "" : ` ${d.getFullYear()}`) + ` ${fmtClock(ms)}`;
}
/// The whole moment, for tooltips and logs: "2026-11-13 08:12:05".
function fmtFull(ns) {
  if (!ns) return "";
  const d = new Date(ns / 1e6);
  return `${dateKey(d)} ${fmtClock(ns / 1e6, true)}`;
}
/// How long ago: "now", "12 s ago", "5 min ago", "3 h ago", "2 d ago"; older than a week, the date.
function fmtAgo(ns, nowMs) {
  if (!ns) return "never";
  const now = nowMs ?? Date.now(), s = Math.round((now * 1e6 - ns) / 1e9);
  if (s < 3) return "now";
  if (s < 60) return `${s} s ago`;
  if (s < 3600) return `${Math.round(s / 60)} min ago`;
  if (s < 86400) return `${Math.round(s / 3600)} h ago`;
  if (s < 7 * 86400) return `${Math.round(s / 86400)} d ago`;
  return fmtStamp(ns, now);
}
/// "2d 3h", "3h 12m", "4m 05s"
function fmtDuration(s) {
  if (s == null) return "-";
  s = Math.max(0, Math.floor(s));
  const d = Math.floor(s / 86400), h = Math.floor(s % 86400 / 3600), m = Math.floor(s % 3600 / 60);
  return d ? `${d}d ${h}h` : h ? `${h}h ${pad2(m)}m` : `${m}m ${pad2(s % 60)}s`;
}
/// "5 min", "90 s"
function fmtInterval(s) { return s % 60 === 0 ? `${s / 60} min` : `${s} s`; }

// ---- numbers ----

function fmtBytes(n) {
  if (n == null) return "-";
  const units = ["B", "KB", "MB", "GB", "TB"];
  let i = 0;
  for (; n >= 1024 && i < units.length - 1; i++) n /= 1024;
  return (i === 0 ? n : n.toFixed(n < 10 ? 1 : 0)) + " " + units[i];
}
/// Nanoseconds as milliseconds ("-" when unknown).
function fmtNsMs(ns, digits = 3) { return ns == null ? "-" : (ns / 1e6).toFixed(digits) + " ms"; }
function fmtNsUs(ns) { return ns == null ? "-" : (ns / 1e3).toFixed(1) + " \u00b5s"; }
function fmtPct(x) { return Math.round(x * 100) + "%"; }

// ---- what a client is called ----
// A client is shown by the name set for it, else the one it suggests, else its hostname, else a
// short id. Short ids are as long as it takes to tell this server's clients apart: the boards'
// ids begin with their chip family's bytes, so the first eight characters are often the same.

/// The shortest prefix of each id (at least `min` hex digits) that no other id shares.
function shortIds(ids, min = 8) {
  const hex = id => String(id).replace(/-/g, "");
  const all = [...new Set(ids)];
  let n = min;
  for (; n < 32; n++) {
    if (new Set(all.map(id => hex(id).slice(0, n))).size === all.length) break;
  }
  // Cut the id as written, keeping its dashes, after n hex digits.
  const cut = id => {
    let seen = 0, i = 0;
    for (; i < id.length && seen < n; i++) if (id[i] !== "-") seen++;
    return id.slice(0, i);
  };
  return new Map(all.map(id => [id, cut(String(id))]));
}

/// A hostname as shown: boards named "bps-..." report "bps-bps-...".
function cleanHostname(h) { return String(h || "").trim().replace(/^(bps-)+/, "bps-"); }

/// Map of client id -> label, unique among `clients`.
function clientLabels(clients) {
  const short = shortIds(clients.map(c => c.id));
  const base = c => c.name || c.suggested_name || cleanHostname(c.hostname) || short.get(c.id);
  const count = new Map();
  for (const c of clients) count.set(base(c), (count.get(base(c)) || 0) + 1);
  return new Map(clients.map(c => {
    const b = base(c);
    return [c.id, count.get(b) > 1 && b !== short.get(c.id) ? `${b} (${short.get(c.id)})` : b];
  }));
}

// ---- how a device is doing ----

const BATTERY_LOW_PCT = 20;         // as the server's battery_low event
const STORAGE_LOW_BYTES = 1 << 30;  // as its storage_low event

/// One word on a device, and how to colour it: { level: "ok" | "warn" | "bad" | "off", text }.
function deviceState(c) {
  if (c.api_compat === "incompatible") return { level: "bad", text: "refused" };
  if (!c.active) return { level: "off", text: "offline" };
  if (!c.synced) return { level: "bad", text: "clock not synced" };
  return { level: "ok", text: c.health?.mode?.mode === "batch" ? "listening (batch)" : "listening" };
}

/// What needs attention on a device, worst first: [{ level: "bad" | "warn" | "info", text, tab }].
/// `tab` names the part of the device page that shows more.
function deviceIssues(c) {
  const out = [], h = c.health, add = (level, text, tab) => out.push({ level, text, tab });
  if (c.api_compat === "incompatible") add("bad", `Its API version (${c.api_version}) is refused by this server`, "overview");
  if (c.active && !c.synced) add("bad", "Its clock is not synced: its recordings cannot be placed in time", "clock");
  const b = h?.battery;
  if (c.active && b && b.percent < BATTERY_LOW_PCT && b.charging !== true) add("bad", `Battery at ${Math.round(b.percent)}%`, "overview");
  const fw = c.firmware?.last;
  if (fw && (fw.result === "failed" || fw.result === "rolled_back"))
    add("bad", `Firmware update ${fw.result === "failed" ? "failed" : "was rolled back"}`, "firmware");
  const s = h?.storage;
  if (s && s.present && s.free_bytes != null && s.free_bytes < STORAGE_LOW_BYTES) add("warn", `Storage card nearly full (${fmtBytes(s.free_bytes)} free)`, "overview");
  if (s && s.errors) add("warn", `${plural(s.errors, "storage card write error")}`, "overview");
  if (s && s.paused) add("warn", "Storage card closed while the battery is low: recordings are not kept", "overview");
  const missing = (h?.devices || []).filter(d => !d.present).map(d => d.kind);
  if (c.active && missing.length) add("warn", `Not found: ${missing.join(", ")}`, "overview");
  if (h?.mode?.note) add("warn", h.mode.note, "overview");
  if (["watchdog", "brownout", "panic"].includes(c.last_reboot?.reason))
    add("warn", `Last start was a ${c.last_reboot.reason === "panic" ? "crash" : c.last_reboot.reason}`, "overview");
  if (c.api_compat === "legacy") add("warn", "Old firmware: it does not report an API version", "firmware");
  if (c.api_compat === "client_newer") add("warn", "It speaks a newer API than this server", "firmware");
  if (c.firmware?.offering) add("info", "A firmware update is on offer", "firmware");
  if (c.positioning_requested) add("info", "It is asking to have its position set", "setup");
  const order = { bad: 0, warn: 1, info: 2 };
  return out.sort((a, b) => order[a.level] - order[b.level]);
}

// ---- events ----
// The server's event log, sorted into groups a person filters by. A kind in no group is "other".

const EVENT_GROUPS = [
  { id: "lifecycle", label: "Connections", kinds: ["register", "register_refused", "inactive", "client_rebooted", "deleted", "server_start"] },
  { id: "clock", label: "Clock", kinds: ["clock_synced", "clock_unsynced"] },
  { id: "detect", label: "Identification", kinds: ["identified", "identify_error", "validation"] },
  { id: "command", label: "Commands", kinds: ["reboot_requested", "logging_requested", "command_acked", "mode_set", "config", "settings", "position_request", "species_muted", "species_unmuted"] },
  { id: "firmware", label: "Firmware", kinds: ["firmware_uploaded", "firmware_deleted", "firmware_policy_set", "firmware_offered", "firmware_update_deferred", "firmware_update_failed", "firmware_updated"] },
  { id: "health", label: "Health", kinds: ["battery_low", "storage_low", "storage_paused", "storage_resumed", "capture_dropout", "client_log_dropped"] },
  { id: "audio", label: "Audio uploads", kinds: ["audio"], quiet: true },
  { id: "log", label: "Trace log", kinds: ["client_log"], quiet: true },
  { id: "heartbeat", label: "Heartbeats", kinds: ["heartbeat"], quiet: true },
];
function eventGroup(kind) { return (EVENT_GROUPS.find(g => g.kinds.includes(kind)) || { id: "other" }).id; }
/// How serious an event is, for its colour: "bad", "warn" or "".
function eventLevel(e) {
  const k = e.kind, d = e.detail || {};
  if (["register_refused", "identify_error", "firmware_update_failed", "clock_unsynced", "battery_low"].includes(k)) return "bad";
  if (["inactive", "capture_dropout", "storage_low", "storage_paused", "firmware_update_deferred", "client_log_dropped"].includes(k)) return "warn";
  if (k === "client_rebooted" && ["watchdog", "brownout", "panic"].includes(d.reason)) return "warn";
  if (k === "command_acked" && d.result && d.result !== "done") return "warn";
  return "";
}

/// One line saying what happened, in words; the raw detail stays available beside it.
function eventSummary(e) {
  const d = e.detail || {}, k = e.kind;
  const ms = ns => (ns / 1e6).toFixed(2) + " ms";
  switch (k) {
    case "client_log": return String(d.text ?? "");
    case "audio": return `${Number(d.duration_s || 0).toFixed(1)} s of audio, ${fmtBytes(d.bytes)}` +
      (d.sample_rate ? `, ${d.sample_rate / 1000} kHz` : "") + (d.latency_ms != null ? `, ${d.latency_ms} ms after it ended` : "");
    case "register": return `Registered: ${d.version || "unknown version"}` + (d.api_version ? `, API ${d.api_version}` : ", no API version") +
      (d.addr ? `, from ${d.addr}` : "");
    case "register_refused": return `Registration refused: ${d.reason || d.error || `API ${d.api_version}`}`;
    case "inactive": return "Stopped sending heartbeats" + (d.last_seen ? ` (last heard ${fmtClock(d.last_seen / 1e6, true)})` : "");
    case "client_rebooted": return `Started (${d.reason === "requested" ? "reboot asked from here" : String(d.reason || "unknown").replace(/_/g, " ")})`;
    case "deleted": return "Removed from the server";
    case "server_start": return `Server started` + (d.identifier ? `, identifying with ${d.identifier}` : "");
    case "clock_synced": return "Clock synced" + (d.clock?.error_ns != null ? `, error within ${ms(d.clock.error_ns)}` : "");
    case "clock_unsynced": return "Clock out of sync" + (d.clock?.error_ns != null ? `: error ${ms(d.clock.error_ns)}` : "");
    case "identified": return (d.species || []).length
      ? "Heard " + d.species.map(s => `${s[0]} (${Number(s[1]).toFixed(2)})`).join(", ") : `${plural(d.calls || 0, "call")} identified`;
    case "identify_error": return `Identification failed: ${d.error || ""}`;
    case "reboot_requested": return "Reboot asked for";
    case "logging_requested": return d.log_level === "off" ? "Trace logging asked off" : `Trace logging asked on for ${fmtDuration(d.log_duration_s)}`;
    case "command_acked": return `The device answered "${d.command}": ${d.result}`;
    case "mode_set": return `Mode set to ${d.mode}` + (d.mode === "batch" && d.interval_s ? `, contact every ${fmtInterval(d.interval_s)}` : "");
    case "settings": return "Server settings saved";
    case "species_muted": return `Muted ${d.scientific}`;
    case "species_unmuted": return `Unmuted ${d.scientific}`;
    case "storage_paused": return "Storage card closed: the battery is low";
    case "storage_resumed": return "Storage card opened again: the battery recovered";
    case "capture_dropout": return `${plural(d.count ?? 1, "gap")} in the captured audio`;
    case "firmware_uploaded": return `Build ${d.describe} uploaded for ${d.hardware_id} (${fmtBytes(d.size)})`;
    case "firmware_deleted": return `Build ${d.describe || d.build_id} deleted`;
    case "firmware_policy_set": return "Firmware choice: " + ({ latest_dev: "latest dev", latest_stable: "latest stable", manual: `build ${d.build_id}` }[d.policy] || d.policy);
    case "firmware_offered": return `Offered ${d.describe}` + (d.from ? ` (it runs ${d.from})` : "");
    case "firmware_updated": return `Updated to ${d.describe}` + (d.from ? ` from ${d.from}` : "");
    case "firmware_update_deferred": return `Update put off by the device: ${d.detail || "no reason given"}`;
    case "firmware_update_failed": return `Update failed: ${d.detail || d.state || "no reason given"}`;
  }
  // Anything else: its plain fields, in the order the server wrote them.
  const parts = Object.entries(d).filter(([, v]) => v == null || typeof v !== "object").map(([key, v]) => `${key} ${v}`);
  return parts.length ? parts.join(", ") : k.replace(/_/g, " ");
}

// ---- a day of detections, by species ----

/// Groups timeline entries by species: [{ scientific, common, occurrence, muted, items, clients,
/// first, last, best }], ordered by "first" (first heard), "count" or "name".
function speciesLanes(detections, order = "first") {
  const lanes = new Map();
  for (const d of detections) {
    let l = lanes.get(d.scientific);
    if (!l) {
      l = { scientific: d.scientific, common: d.common, occurrence: d.occurrence, muted: !!d.muted, items: [], clients: new Set(),
            first: Infinity, last: -Infinity, best: 0 };
      lanes.set(d.scientific, l);
    }
    l.items.push(d);
    for (const c of d.clients || []) l.clients.add(c);
    l.first = Math.min(l.first, d.time);
    l.last = Math.max(l.last, d.time);
    l.best = Math.max(l.best, d.confidence || 0);
  }
  const byName = (a, b) => a.common.localeCompare(b.common);
  return [...lanes.values()].sort(order === "count" ? (a, b) => b.items.length - a.items.length || byName(a, b)
    : order === "name" ? byName : (a, b) => a.first - b.first);
}

/// Whether a species lane shows under the "Show" choice: "expected", "known" (expected and
/// unexpected) or "all" (also sounds without range data). Muted species never do.
function laneShown(l, mode) {
  if (l.muted) return false;
  return mode === "all" || l.occurrence === "expected" || (mode === "known" && l.occurrence === "unexpected");
}

if (typeof module !== "undefined") {
  module.exports = {
    esc, plural, dateKey, dayStartMs, dayEndMs, shiftDayKey, fmtClock, fmtDay, fmtStamp, fmtFull, fmtAgo, fmtDuration,
    fmtInterval, fmtBytes, fmtNsMs, fmtNsUs, shortIds, cleanHostname, clientLabels, deviceState, deviceIssues,
    EVENT_GROUPS, eventGroup, eventLevel, eventSummary, speciesLanes, laneShown,
  };
}
