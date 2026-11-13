// The System workspace (system.html): running and debugging the devices. Four pages over the
// control API: Devices (a list, and one device in full under tabs), Firmware (the builds on
// offer), Events (the server's log, in words) and Settings (one section saved at a time).
"use strict";

// ---- how a device's reports read ----

function clockLimitNs() { return (App.status?.max_clock_offset_us ?? 2000) * 1000; }
function clockState(c) {
  if (!c.active) return { cls: "muted", text: "offline" };
  if (c.synced && !c.synced_to_server) return { cls: "ok", text: "synced with its access point's devices" };
  return c.synced ? { cls: "ok", text: "synced" } : { cls: "bad", text: "not synced" };
}
/// A device's API version as shown: text, class, and what it means. The server accepts the same
/// major version, and refuses (but lists) any other.
function apiView(c) {
  const ours = App.status ? ` The server implements API ${App.status.api_version}.` : "";
  switch (c.api_compat) {
    case "incompatible": return { text: `API ${c.api_version}: refused`, cls: "bad",
      sub: `The server refused this device's registration: its major version differs.${ours} Update the device or the server.` };
    case "legacy": return { text: "API not reported", cls: "warn",
      sub: `The device does not report an API version, so it is probably running old firmware. It is accepted as compatible.${ours}` };
    case "client_newer": return { text: `API ${c.api_version}: newer than the server`, cls: "warn",
      sub: `The device speaks a newer minor version than the server. It is accepted, but anything new it sends may be ignored. Update the server.${ours}` };
    default: return { text: `API ${c.api_version}`, cls: "muted", sub: `Compatible with the server.${ours}` };
  }
}
const GAUGE_KINDS = ["max17048"];   // parts that report the battery
/// The battery as { text, sub, cls, charging }, or null if the device has no gauge.
function batteryView(c) {
  const h = c.health, b = h?.battery;
  if (b) {
    const r = b.rate_pct_per_h;
    // The gauge has no current sensor: its rate is how its charge estimate (made from the cell's
    // voltage) moves, which goes on settling for a while after a charger is unplugged. So a rise
    // says the estimate is rising, not that the cell is charging.
    const trend = r == null ? "" : r > 0.2 ? `rising ${r.toFixed(1)} %/h` : r < -0.2 ? `falling ${Math.abs(r).toFixed(1)} %/h` : "steady";
    return { text: `${Math.min(100, b.percent).toFixed(0)}%`, cls: b.percent < BATTERY_LOW_PCT && b.charging !== true ? "bad" : "",
             sub: `${b.voltage_v.toFixed(2)} V` + (trend ? `, ${trend}` : ""),
             // Only what the device reports from its charger; never inferred from the trend.
             charging: b.charging === true };
  }
  if (h?.devices?.some(d => GAUGE_KINDS.includes(d.kind))) return { text: "no reading", cls: "muted", sub: "the gauge is not answering: is a battery connected?" };
  return null;
}
const CHARGING = `<span class="badge ok">charging</span>`;
/// The operation mode as { text, sub, cls }: as it runs, and the mode set for it while it has
/// yet to switch.
function modeView(c) {
  const m = c.health?.mode, want = c.mode;
  const name = x => x.mode === "batch" ? `batch, every ${fmtInterval(x.interval_s)}` : x.mode === "low_power" ? "low power" : "live";
  if (!m) return { text: "live", cls: "muted", sub: "reports no mode: cannot be switched" };
  const parts = [];
  if (m.note) parts.push(m.note);
  else if (name(want) !== name(m)) parts.push(`switching to ${name(want)} at its next ${m.mode === "batch" ? "contact" : "heartbeat"}`);
  if (m.mode === "low_power" && m.radio_on_pct != null) parts.push(`radio awake ${m.radio_on_pct.toFixed(1)}% of the time`);
  if (m.mode === "batch") {
    if (m.radio_on_pct != null) parts.push(`radio on ${m.radio_on_pct.toFixed(1)}% of the time`);
    if (m.next_contact_s != null && c.health) {
      const due = c.health.at + m.next_contact_s * 1e9 - Date.now() * 1e6;
      parts.push(due > 0 ? `next contact in about ${fmtInterval(Math.max(30, Math.round(due / 3e10) * 30))}` : "contact due");
    }
  }
  if (m.backlog) parts.push(`${plural(m.backlog, "recording")} (${fmtBytes(m.backlog_bytes)}) to upload`);
  return { text: name(m), sub: parts.join("; "), cls: m.note ? "bad" : "" };
}
/// How far a batch device's clock moved while its radio was last off, or "".
function radioGapText(c) {
  const g = c.health?.mode?.gap;
  if (!g) return "";
  const us = ns => (ns / 1e3).toFixed(0) + " µs";
  return `after ${fmtDuration(g.off_s)} off, its clock was ${us(g.clock_ns)} from where its drift put it` +
    (g.tsf_ns != null ? ` (${us(g.tsf_ns)} against its access point)` : "");
}
/// The storage card as { text, sub, cls }, or null if the device has no slot.
function storageView(c) {
  const s = c.health?.storage;
  if (!s) return null;
  const problems = [s.errors ? plural(s.errors, "write error") : "", s.dropped_bytes ? `${fmtBytes(s.dropped_bytes)} not written` : ""].filter(Boolean).join(", ");
  const saved = `${plural(s.recordings, "recording")} (${fmtBytes(s.recorded_bytes)}) saved since it started`;
  if (s.paused) return { text: "closed", cls: "bad", sub: "the battery is low: the card is closed until it recovers, and recordings are not kept" };
  if (!s.present) return { text: "no card", cls: "muted", sub: "not keeping recordings" + (problems ? `; ${problems}` : "") };
  return { text: `${fmtBytes(s.free_bytes)} free`, cls: s.free_bytes < STORAGE_LOW_BYTES ? "bad" : "",
           sub: `of ${fmtBytes(s.total_bytes)}; ${saved}` + (problems ? `; ${problems}` : "") };
}
function partWhere(d) {
  if (d.bus === "i2c") return `I2C 0x${d.address.toString(16).padStart(2, "0")}`;
  return d.address >= 0 ? `SPI, chip select ${d.address}` : "SPI";
}
function triggerText(t) { return t.trigger_type === "energy" ? `energy, ${t.threshold_db} dB` : t.trigger_type; }
function validationText(v, h) {
  const type = v?.validation_type ?? "none";
  let text = type === "none" ? "no validation"
    : type === "model" ? `model ${v.validation_model}` + (v.validation_threshold != null ? ` at ${v.validation_threshold}` : "") : `validation: ${type}`;
  if (h?.chunks_rejected) text += `, ${h.chunks_rejected} rejected`;
  if (h?.validation_note) text += ` (${h.validation_note})`;
  return text;
}
const REBOOT_REASONS = { requested: "reboot asked from here", power_on: "power on", watchdog: "watchdog", brownout: "brownout",
                         panic: "crash", other: "other reset", unknown: "unknown" };
/// "its next heartbeat" or, for a batch device, "its next contact".
function nextContact(c) { return c.health?.mode?.mode === "batch" ? "its next contact" : "its next heartbeat"; }
/// "10.0.0.7" from "10.0.0.7:51234" or "[fe80::1]:51234".
function addrHost(addr) {
  const m = /^\[(.*)\]:\d+$/.exec(addr) || /^([^:]*):\d+$/.exec(addr);
  return m ? m[1] : addr;
}
/// "v1.3.0", or "dev" for a build that is not a release.
function fwTag(b) {
  const stable = b.tag && !b.dirty;
  return stable ? `<b>${esc(b.tag)}</b>` : `<span class="muted">dev</span>`;
}
/// The build time (UTC, "YYYY-MM-DDTHH:MM:SSZ") in local time.
function fwBuilt(b) { const ms = Date.parse(b.built_at); return isNaN(ms) ? esc(b.built_at) : fmtFull(ms * 1e6); }
function fwCommit(b) { return `<span class="mono">${esc(b.commit)}${b.dirty ? " (dirty)" : ""}</span>`; }
function deviceLink(id, tab) { return href("devices", [id, tab]); }
function deviceLinks(ids) { return ids.map(id => `<a href="${deviceLink(id)}">${esc(label(id))}</a>`).join(", "); }

// ---- Devices ----

const TABS = [["overview", "Overview"], ["clock", "Clock"], ["firmware", "Firmware"], ["events", "Events"], ["setup", "Setup"]];
const LONG_OFFLINE_NS = 24 * 3600e9;
const devPrefs = prefs("bps.devices", { showOld: false });
let devId = null, devTab = "overview";
let firmwareBuilds = [], firmwareAt = 0;

function enterDevices(r) {
  if (r.parts[0] && r.parts[0] !== devId) { devId = r.parts[0]; clockData = null; }
  devTab = TABS.some(t => t[0] === r.parts[1]) ? r.parts[1] : "overview";
  if (devTab === "events" && r.params.get("show")) devEvents.show(r.params.get("show").split(","));
  renderDevices();
}
/// The devices listed: those heard in the last day, or all of them when asked; always the one open.
function listedDevices() {
  const now = Date.now() * 1e6;
  const old = c => !c.active && now - c.last_seen > LONG_OFFLINE_NS;
  const sorted = [...App.clients].sort((a, b) => (b.active - a.active) || (a.active ? label(a.id).localeCompare(label(b.id)) : b.last_seen - a.last_seen));
  const hidden = sorted.filter(c => old(c) && c.id !== devId);
  return { shown: devPrefs.showOld ? sorted : sorted.filter(c => !hidden.includes(c)), hidden };
}
function renderDevices() {
  if (!client(devId)) {
    const { shown } = listedDevices();
    devId = (shown.find(c => c.active) || shown[0] || App.clients[0])?.id ?? null;
  }
  const { shown, hidden } = listedDevices();
  setText("dev-count", App.clients.length ? `${App.clients.filter(c => c.active).length} of ${App.clients.length} online` : "");
  setHtml("dev-list", shown.map(c => {
    const st = deviceState(c), bat = batteryView(c), issues = deviceIssues(c).filter(i => i.level !== "info");
    const worst = issues[0]?.level;
    const second = !c.active ? `offline, last heard ${fmtAgo(c.last_seen)}`
      : issues.length ? esc(issues[0].text) + (issues.length > 1 ? ` (+${issues.length - 1})` : "")
      : esc(st.text) + (c.clock_error_ns != null ? `, clock within ${(c.clock_error_ns / 1e6).toFixed(1)} ms` : "");
    return `<li><a class="item ${c.active ? "" : "off"} ${c.id === devId ? "current" : ""}" href="${deviceLink(c.id, devTab === "overview" ? "" : devTab)}">
      <div class="line"><span class="dot ${worst === "bad" ? "bad" : worst === "warn" && c.active ? "warn" : st.level}" title="${esc(st.text)}"></span>
        <span class="grow"><b>${esc(label(c.id))}</b></span>
        ${bat && c.health?.battery ? `<span class="num ${bat.cls}" title="Battery${bat.charging ? ", charging" : ""}">${esc(bat.text)}${bat.charging ? " +" : ""}</span>` : ""}</div>
      <div class="sub"><span class="grow ${worst && c.active ? worst : ""}">${second}</span></div></a></li>`;
  }).join("") || `<li class="empty">No devices.</li>`);
  $("dev-hidden-box").hidden = !hidden.length;
  setText("dev-hidden-text", devPrefs.showOld ? `${plural(hidden.length, "device")} offline for more than a day.` : `${plural(hidden.length, "device")} offline for more than a day ${hidden.length === 1 ? "is" : "are"} not listed.`);
  setText("dev-hidden-toggle", devPrefs.showOld ? "Hide" : "Show");

  const c = client(devId);
  $("dev-none").hidden = !!c;
  $("dev-body").hidden = !c;
  if (!c) return;
  const st = deviceState(c), h = c.health;
  setText("dev-name", label(c.id));
  setHtml("dev-state", `<span class="dot ${st.level}"></span> ${esc(st.text)}`);
  setHtml("dev-heard", c.active ? "" : `last heard ${ago(c.last_seen)}`);
  setHtml("dev-facts", [
    `<span class="mono" title="Device ID">${esc(c.id)}</span>`,
    c.build ? `<span title="The hardware its firmware was built for: builds are offered only to devices with the same.">${esc(c.build.hardware_id)}</span>` : "",
    c.last_addr ? `<span title="${esc(c.last_addr)}">${esc(addrHost(c.last_addr))}</span>` : "",
    esc(c.version || ""),
  ].filter(Boolean).join(" &middot; "));
  const issues = deviceIssues(c);
  $("dev-issues").hidden = !issues.length;
  setHtml("dev-issues", issues.map(i => `<li class="lvl-${i.level}"><span class="grow">${esc(i.text)}</span>` +
    (i.tab && i.tab !== devTab ? `<a href="${deviceLink(c.id, i.tab)}">${esc(TABS.find(t => t[0] === i.tab)[1])}</a>` : "") + `</li>`).join(""));
  setHtml("dev-tabs", TABS.map(([id, name]) => `<a role="tab" href="${deviceLink(c.id, id === "overview" ? "" : id)}" class="${id === devTab ? "current" : ""}" ` +
    `aria-selected="${id === devTab}">${name}</a>`).join(""));
  for (const [id] of TABS) $("tab-" + id).hidden = id !== devTab;
  if (devTab === "overview") renderOverview(c);
  if (devTab === "clock") renderClock(c);
  if (devTab === "firmware") renderDevFirmware(c);
  if (devTab === "setup") renderSetup(c);
}
async function refreshDevices() {
  renderDevices();
  const id = devId, c = client(id);
  if (!c) return;
  if (devTab === "overview") { await refreshHistory(id); if (id === devId) renderOverview(c); }
  if (devTab === "clock") {
    const data = await api(`/clients/${id}/clock?minutes=${$("clock-range").value}`);
    if (id !== devId) return;
    clockData = data;
    renderClock(c);
  }
  if (devTab === "firmware") { await refreshFirmware(); if (id === devId) renderDevFirmware(c); }
  if (devTab === "events") await devEvents.tick();
}
$("dev-hidden-toggle").onclick = () => { devPrefs.showOld = !devPrefs.showOld; devPrefs.save(); renderDevices(); };

// -- Overview --

const HISTORY_REFRESH_MS = 30000;
let historyData = null, historyAt = 0;   // { key, battery, detections }
function historyKey(id) { return `${id}/${$("battery-range").value}`; }
async function refreshHistory(id) {
  const key = historyKey(id);
  if (historyData?.key === key && Date.now() - historyAt < HISTORY_REFRESH_MS) return;
  const [battery, detections] = await Promise.all([
    api(`/clients/${id}/battery_history?hours=${$("battery-range").value}`), api(`/clients/${id}/detections_hourly`)]);
  if (historyKey(devId) !== key) return;  // another device or range was chosen meanwhile
  historyData = { key, battery, detections };
  historyAt = Date.now();
}
function renderOverview(c) {
  const h = c.health, asOf = h && !c.active ? ` <span class="muted">(${fmtAgo(h.at)})</span>` : "";
  const hist = historyData?.key === historyKey(c.id) ? historyData : null;
  const det = hist?.detections, detTotal = det ? det.counts.reduce((a, b) => a + b, 0) : null;
  const bat = batteryView(c), card = storageView(c), md = modeView(c);
  setText("ov-det-total", detTotal == null ? "" : `${detTotal} in all, at most ${Math.max(...det.counts)} in an hour`);
  setHtml("ov-tiles", [
    !h ? tile("Health", "-", "no heartbeat stored yet") : "",
    h ? tile("Up for" + asOf, fmtDuration(h.uptime_s), c.last_reboot
      ? `started by: ${esc(REBOOT_REASONS[c.last_reboot.reason] || c.last_reboot.reason)}, ${ago(c.last_reboot.at)}` : `since ${esc(fmtStamp(h.at - h.uptime_s * 1e9))}`,
      ["watchdog", "brownout", "panic"].includes(c.last_reboot?.reason) ? "warn" : "") : "",
    bat ? tile("Battery" + asOf, esc(bat.text) + (bat.charging ? " " + CHARGING : ""), esc(bat.sub), bat.cls) : "",
    card ? tile("Storage card" + asOf, esc(card.text), esc(card.sub), card.cls) : "",
    tile("Mode" + asOf, esc(md.text), esc(md.sub), md.cls),
    h ? tile("Audio" + asOf, `${h.chunks_sent} sent`, `${h.chunks_gated} quiet chunks not sent ` +
      info("Chunks of audio the trigger found too quiet to send. Both counts are since the device started.")) : "",
    h?.trigger ? tile("Trigger" + asOf, esc(triggerText(h.trigger)), esc(validationText(h.validation, h))) : "",
    h?.devices?.length ? tile("Parts", `${h.devices.filter(d => d.present).length} of ${h.devices.length} found`,
      h.devices.map(d => `${esc(d.kind)} (${partWhere(d)}): ${d.present ? "found" : `<b class="warn">not found</b>`}`).join("<br>")) : "",
  ].join(""));

  // A battery section only for a device that has, or had, a battery to report.
  $("battery-section").hidden = !bat && !(hist?.battery.points.length);
  const charts = hist ? "" : "Loading...";
  renderDetChart(det, charts || (detTotal ? "" : "Nothing recorded by this device in the last 24 hours."));
  renderBatteryChart(hist?.battery, charts || (hist.battery.points.length ? "" : "No battery readings in this range."));
  fillModeForm(c);
  renderMaintenance(c);
}
function renderBatteryChart(data, empty) {
  const chart = chartFor("battery-chart", empty);
  if (!chart || !data) return;
  const k = chartColors();
  // A break in the line where readings stopped for a while (device off or out of reach).
  const GAP_MS = 30 * 60e3, rows = [];
  for (const p of data.points) {
    const prev = rows.at(-1);
    if (prev && p.at / 1e6 - prev.ms > GAP_MS) rows.push({ ms: (prev.ms + p.at / 1e6) / 2, gap: true });
    rows.push({ ms: p.at / 1e6, p });
  }
  chart.setOption({
    animation: false, textStyle: chartFont(),
    grid: { left: 48, right: 24, top: 12, bottom: 28 },
    xAxis: { type: "time", min: data.from / 1e6, max: data.server_time / 1e6, axisLabel: { color: k.ink2, hideOverlap: true, formatter: { hour: "{HH}:{mm}", day: "{d} {MMM}" } },
             axisLine: { lineStyle: { color: k.axis } }, axisTick: { lineStyle: { color: k.axis } }, splitLine: { show: false } },
    yAxis: { type: "value", min: 0, max: 100, interval: 25, axisLabel: { color: k.ink2, formatter: "{value}%" }, splitLine: { lineStyle: { color: k.grid } } },
    tooltip: {
      trigger: "axis", confine: true, backgroundColor: k.surface, borderColor: k.axis, textStyle: { color: k.ink }, axisPointer: { lineStyle: { color: k.axis } },
      formatter: ps => {
        const r = rows[ps[0]?.dataIndex];
        if (!r || r.gap) return "";
        return `<b>${fmtFull(r.p.at)}</b><br>${Math.min(100, r.p.percent).toFixed(1)}%, ${r.p.voltage_v.toFixed(2)} V` + (r.p.charging ? "<br>charging" : "");
      },
    },
    series: [{ name: "Battery", type: "line", data: rows.map(r => [r.ms, r.gap ? null : Math.min(100, r.p.percent)]),
               showSymbol: data.points.length === 1, symbolSize: 8, connectNulls: false, lineStyle: { width: 2, color: k.s1 }, itemStyle: { color: k.s1 } }],
  }, { replaceMerge: ["series"] });
}
function renderDetChart(data, empty) {
  const chart = chartFor("det-chart", empty);
  if (!chart || !data) return;
  const k = chartColors();
  const hhmm = ns => fmtClock(ns / 1e6);
  const starts = data.counts.map((_, i) => data.from + i * data.bucket_ns);
  chart.setOption({
    animation: false, textStyle: chartFont(),
    grid: { left: 48, right: 24, top: 12, bottom: 28 },
    xAxis: { type: "category", data: starts.map(hhmm), axisLabel: { color: k.ink2, interval: 3 }, axisLine: { lineStyle: { color: k.axis } },
             axisTick: { alignWithLabel: true, lineStyle: { color: k.axis } } },
    yAxis: { type: "value", minInterval: 1, axisLabel: { color: k.ink2 }, splitLine: { lineStyle: { color: k.grid } } },
    tooltip: { trigger: "item", confine: true, backgroundColor: k.surface, borderColor: k.axis, textStyle: { color: k.ink },
               formatter: p => `${hhmm(starts[p.dataIndex])} to ${hhmm(starts[p.dataIndex] + data.bucket_ns)}<br><b>${plural(p.value, "recording")}</b>` },
    series: [{ name: "Recordings", type: "bar", data: data.counts, barCategoryGap: "20%", itemStyle: { color: k.s1 },
               // Pointing anywhere in an hour's column, not just at its bar.
               showBackground: true, backgroundStyle: { color: "transparent" } }],
  }, { replaceMerge: ["series"] });
}
$("battery-range").onchange = () => { historyAt = 0; refresh(); };

// The forms keep what is being typed while the page refreshes; each is filled from the device
// when another is opened, or after saving.
let modeFormFor = null, modeDirty = false;
function fillModeForm(c) {
  if (modeFormFor !== c.id || !modeDirty) {
    if (modeFormFor !== c.id) setText("mode-msg", "");
    modeFormFor = c.id;
    modeDirty = false;
    $("mode-select").value = c.mode.mode;
    $("mode-interval").value = c.mode.interval_s;
  }
  $("mode-interval-label").hidden = $("mode-select").value !== "batch";
  const can = !!c.health?.mode;
  for (const el of $("mode-form").querySelectorAll("select, input, button")) el.disabled = !can;
  setText("mode-hint", can ? "" : "This device reports no mode (older firmware, or a computer or browser client), so it cannot be switched.");
  setHtml("mode-info", info("Live: stays connected and uploads audio as it records.\n" +
    "Low power: stays connected and uploads as it records, but its Wi-Fi radio sleeps between short wakes (its clock is measured " +
    "every 15 s, a heartbeat every 30 s, and uploads), and its processor runs slower. It follows the server's clock only, not its " +
    "access point's. ESP32 boards only.\n" +
    "Batch (needs a storage card): the Wi-Fi radio is off between contacts and audio is kept on the card. At each contact the device " +
    "measures its clock again and uploads what it recorded, so the audio can still be placed in time; what it heard shows up to an interval later."));
}
$("mode-form").addEventListener("input", () => { modeDirty = true; const c = client(modeFormFor); if (c) fillModeForm(c); });
$("mode-form").addEventListener("submit", async ev => {
  ev.preventDefault();
  const id = modeFormFor, mode = $("mode-select").value, interval_s = Number($("mode-interval").value);
  if (!Number.isInteger(interval_s) || interval_s < 30 || interval_s > 3600) { setText("mode-msg", "The interval must be 30 to 3600 s."); return; }
  try {
    await apiSend("PUT", `/clients/${id}/mode`, { mode, interval_s });
    setText("mode-msg", `Set: the device switches at ${nextContact(client(id) || {})}.`);
    modeDirty = false;
    refresh();
  } catch (e) { setText("mode-msg", e.message); }
});

// -- Maintenance: reboot and trace logging (devices on API 1.1.0 or later) --
// Both reach the device in the reply to its next heartbeat (a batch device's: at its next
// contact), and it answers in the heartbeat after.

/// The latest reboot command as { text, cls, title }, or null.
function rebootState(c) {
  const r = c.maintenance?.reboot;
  if (!r) return null;
  if (r.result === "unsupported") return { text: "not supported by this device", cls: "warn" };
  if (r.result === "failed") return { text: "the device could not reboot", cls: "bad" };
  if (r.result === "done") return { text: `rebooted ${fmtAgo(r.acked_at)}`, cls: "ok", title: `acknowledged ${fmtFull(r.acked_at)}` };
  if (r.result) return { text: r.result, cls: "muted" };
  if (r.delivered_at) return { text: "rebooting...", cls: "warn", title: `Sent ${fmtFull(r.delivered_at)}. It is acknowledged in the device's first heartbeat after the restart.` };
  return { text: `waiting for ${nextContact(c)}`, cls: "muted", title: `asked ${fmtFull(r.created_at)}` };
}
/// Trace logging as { text, cls, title }.
function loggingState(c) {
  const m = c.maintenance, l = m?.logging;
  if (l && l.result == null) {
    const what = l.params?.log_level === "off" ? "off" : `on for ${fmtDuration(l.params?.log_duration_s)}`;
    return { text: `waiting for ${nextContact(c)} (${what})`, cls: "muted" };
  }
  if (l?.result === "unsupported") return { text: "not supported by this device", cls: "warn" };
  if (m?.logging_until) return { text: `on until ${fmtClock(m.logging_until / 1e6)}`, cls: "ok", title: `It turns itself off at ${fmtFull(m.logging_until)}.` };
  return { text: "off", cls: "muted" };
}
let logFormFor = null, logDirty = false;
function renderMaintenance(c) {
  const supported = !!c.maintenance?.supported;
  setHtml("maint-info", info("Reboot: the device restarts cleanly (its upload in progress finishes or is given up, its storage card is closed) " +
    "once its next heartbeat reply carries the request; a batch device at its next contact.\n" +
    "Trace logging: the device sends every line it logs with its heartbeats, shown on its Events tab. It turns itself off when the time is up, " +
    "even if it cannot reach the server."));
  const show = (id, v, extra) => setHtml(id, v ? `<span class="${v.cls}" title="${esc(v.title || "")}">${esc(v.text)}</span>${extra || ""}` : "");
  show("reboot-state", supported ? rebootState(c) : null);
  const ls = supported ? loggingState(c) : null;
  show("log-state", ls, c.maintenance?.logging_until || ls?.cls === "muted" && c.maintenance?.logging
    ? ` &middot; <a href="${deviceLink(c.id, "events")}?show=log">see the lines</a>` : "");
  $("reboot-btn").disabled = !supported;
  for (const id of ["log-level", "log-duration", "log-set"]) $(id).disabled = !supported;
  setText("maint-hint", supported ? "" : `Not supported by this device's firmware (API ${c.api_version || "not reported"}): it needs API 1.1.0 or later.`);
  if (logFormFor !== c.id || !logDirty) {
    logFormFor = c.id;
    logDirty = false;
    $("log-level").value = c.maintenance?.logging_until ? "off" : "on";
  }
  $("log-duration-label").hidden = $("log-level").value === "off";
}
$("log-form").addEventListener("input", () => { logDirty = true; $("log-duration-label").hidden = $("log-level").value === "off"; });
$("log-form").addEventListener("submit", async ev => {
  ev.preventDefault();
  const level = $("log-level").value, duration_s = Number($("log-duration").value);
  try {
    await apiSend("PUT", `/clients/${devId}/logging`, level === "off" ? { level } : { level, duration_s });
    logDirty = false;
    refresh();
  } catch (e) { toast("Trace logging was not set: " + e.message, "bad"); }
});
$("reboot-btn").onclick = async () => {
  const c = client(devId);
  if (!c) return;
  const yes = await confirmDialog({
    title: `Reboot ${label(c.id)}?`, action: "Reboot",
    body: `<p>It restarts once ${nextContact(c)} brings the request, and is back within a few seconds.</p><p>Nothing is recorded while it restarts.</p>`,
  });
  if (!yes) return;
  try { await apiSend("POST", `/clients/${c.id}/reboot`); refresh(); } catch (e) { toast("The reboot was not asked for: " + e.message, "bad"); }
};

// -- Clock --
// Placing a call needs every device's audio timestamps on the server's clock. Devices report the
// worst-case error left in their timestamps with every heartbeat; the server keeps the last hour.

let clockData = null;   // GET /clients/{id}/clock for the open device
function percentile(sorted, p) { return sorted.length ? sorted[Math.min(sorted.length - 1, Math.floor(p * sorted.length))] : null; }
function renderClock(c) {
  const data = clockData?.client?.id === c.id ? clockData : null;
  const k = c.clock, d = k?.detail, st = clockState(c), limit = clockLimitNs();
  const reports = data ? data.reports.filter(r => r.clock) : [];
  const errors = reports.map(r => r.error_ns).sort((a, b) => a - b);
  const within = errors.filter(e => e <= limit).length;
  const dets = reports.map(r => r.clock.detail).filter(Boolean);
  const requests = dets.reduce((n, x) => n + x.requests, 0), replies = dets.reduce((n, x) => n + x.replies, 0);
  const medians = dets.map(x => x.rtt_median_ns).filter(v => v != null).sort((a, b) => a - b);
  const minutes = data ? Math.round((data.server_time - data.from) / 60e9) : $("clock-range").value;
  const gap = radioGapText(c);
  setHtml("clock-tiles", [
    tile("Timestamp error now " + info("What is left after the device's correction, plus half the fastest round trip (a measurement cannot tell " +
      "how delay splits between directions) and drift within half a chunk. Over the limit, the device is left out of positioning, unless it " +
      "follows its Wi-Fi access point's beacons: then it lines up with that access point's other devices to microseconds and is used with them."),
      k ? "&plusmn;" + fmtNsMs(c.clock_error_ns) : "-",
      k ? `<span class="${st.cls}">${esc(st.text)}</span> (limit &plusmn;${fmtNsMs(limit, 1)})` : "no measurement"),
    c.ap_error_ns != null ? tile("Within its access point", "&plusmn;" + fmtNsUs(c.ap_error_ns), `against the other devices of ${esc(k?.beacon?.bssid || "")}`) : "",
    tile(`Within the limit, last ${minutes} min`, errors.length ? (100 * within / errors.length).toFixed(1) + "%" : "-",
      errors.length ? `95% of the time within &plusmn;${fmtNsMs(percentile(errors, 0.95))}, worst &plusmn;${fmtNsMs(errors.at(-1))}` : data ? "no reports" : "loading"),
    tile("Clock offset (server minus device)", fmtNsMs(d?.clock_offset_ns),
      d ? (d.corrects_timestamps ? "corrected in the timestamps" : "not corrected (it trusts its system clock)") : "not reported"),
    tile("Drift", d ? d.drift_ppm.toFixed(2) + " ppm" : "-", d ? `${(Math.abs(d.drift_ppm) * 60).toFixed(0)} µs per minute; fitted through ${d.fit_points} measurements` : ""),
    tile("Round trip", k ? "best " + fmtNsMs(k.rtt_ns) : "-", medians.length ? `median ${fmtNsMs(percentile(medians, 0.5))} over the range` : ""),
    tile("Replies", requests ? (100 * replies / requests).toFixed(1) + "%" : "-", requests ? `${replies} of ${requests} measurements answered` : ""),
    k?.beacon ? tile("Access point clock", `<span class="mono">${esc(k.beacon.bssid)}</span>`,
      `follows its beacons to ${fmtNsUs(k.beacon.scatter_ns)} RMS (${k.beacon.beacons} beacons, drift ${k.beacon.drift_ppm.toFixed(2)} ppm against it)` +
      (c.beacon_line ? `; the access point's line: ${plural(c.beacon_line.clients, "device")}, &plusmn;${fmtNsMs(c.beacon_line.error_ns)} to the server clock`
        : "; the server has no line for this access point yet"), "", "grow") : "",
    tile("Clock source", esc(c.capabilities?.clock_source || "unknown"), ""),
    gap ? tile("Last radio-off gap", fmtDuration(c.health.mode.gap.off_s), esc(gap), "", "grow") : "",
  ].join(""));
  setText("clock-foot", data ? `${reports.length} reports, ${fmtClock(data.from / 1e6)} to ${fmtClock(data.server_time / 1e6)}, a heartbeat every ${data.heartbeat_interval_s} s` : "");
  renderClockChart(data);
}
function renderClockChart(data) {
  const chart = chartFor("clock-chart", !data ? "Loading..." : data.reports.length ? "" : "No clock reports in this range.");
  if (!chart) return;
  const c = chartColors();
  // The reports as chart rows, with a gap row wherever heartbeats stopped.
  const gapNs = 3 * data.heartbeat_interval_s * 1e9, rows = [];
  let prev = null;
  for (const r of data.reports) {
    if (prev !== null && r.at - prev > gapNs) rows.push({ at: (prev + r.at) / 2, gap: true });
    rows.push(r);
    prev = r.at;
  }
  const ms = ns => (ns == null ? null : ns / 1e6);
  const det = (r, f) => [r.at / 1e6, r.gap || !r.clock || !r.clock.detail ? null : f(r.clock.detail)];
  const limitMs = clockLimitNs() / 1e6, from = data.from / 1e6, to = data.server_time / 1e6;
  const H = 118, TITLE = 26, GAP = 56;
  const tops = [0, 1, 2].map(i => TITLE + i * (H + TITLE + GAP));
  const titles = ["Timestamp error bound (ms)", "Round trip (ms)", "Clock offset, server minus device (ms)"];
  const axis = i => ({
    gridIndex: i, type: "time", min: from, max: to,
    axisLabel: { show: i === 2, color: c.ink2, formatter: "{HH}:{mm}", hideOverlap: true },
    axisLine: { lineStyle: { color: c.axis } }, axisTick: { show: i === 2, lineStyle: { color: c.axis } }, splitLine: { show: true, lineStyle: { color: c.grid } },
  });
  const yAxis = (i, extra) => ({ gridIndex: i, type: "value", scale: i === 2, axisLabel: { color: c.ink2 }, splitLine: { lineStyle: { color: c.grid } }, ...extra });
  const line = (name, i, values, extra) => ({
    name, type: "line", xAxisIndex: i, yAxisIndex: i, data: values, showSymbol: false, connectNulls: false,
    lineStyle: { width: 2, color: c.s1 }, itemStyle: { color: c.s1 }, ...extra,
  });
  const row = (text, v) => `<br>${text}: <b>${v}</b>`;
  chart.setOption({
    animation: false, textStyle: chartFont(),
    title: titles.map((text, i) => ({ text, top: tops[i] - TITLE + 2, left: 0, textStyle: { fontSize: 13, fontWeight: 600, color: c.ink } })),
    legend: { top: tops[1] - TITLE + 2, right: 24, data: [{ name: "median", icon: "roundRect" }, { name: "lowest to highest", icon: "rect" }],
              textStyle: { color: c.ink2 }, itemStyle: { color: c.s1 } },
    grid: tops.map(top => ({ left: 56, right: 24, top, height: H })),
    xAxis: [0, 1, 2].map(axis),
    yAxis: [yAxis(0, { min: 0, max: v => Math.max(v.max * 1.1, limitMs * 1.2) }), yAxis(1, { min: 0 }), yAxis(2)],
    axisPointer: { link: [{ xAxisIndex: "all" }], lineStyle: { color: c.axis } },
    tooltip: {
      trigger: "axis", confine: true, backgroundColor: c.surface, borderColor: c.axis, textStyle: { color: c.ink },
      formatter: ps => {
        const r = rows[ps[0]?.dataIndex];
        if (!r) return "";
        let html = `<b>${fmtClock(r.at / 1e6, true)}</b>`;
        if (r.gap) return html + "<br>no heartbeats";
        const k = r.clock, d = k?.detail;
        if (!k) return html + "<br>no measurement reported";
        html += row("Timestamp error", "&plusmn;" + fmtNsMs(r.error_ns)) + row("Best round trip (fit)", fmtNsMs(k.rtt_ns));
        if (d) {
          html += row("Round trip, lowest / median / highest", `${fmtNsMs(d.rtt_min_ns, 2)} / ${fmtNsMs(d.rtt_median_ns, 2)} / ${fmtNsMs(d.rtt_max_ns, 2)}`) +
            row("Clock offset", fmtNsMs(d.clock_offset_ns)) + row("Drift", d.drift_ppm.toFixed(2) + " ppm") + row("Replies", `${d.replies} of ${d.requests}`);
        }
        return html;
      },
    },
    series: [
      line("Timestamp error", 0, rows.map(r => [r.at / 1e6, r.gap ? null : ms(r.error_ns)]), {
        markLine: { silent: true, symbol: "none", animation: false, lineStyle: { color: c.ink2, type: "dashed", width: 1 },
                    label: { formatter: `limit ${limitMs} ms`, color: c.ink2, position: "insideEndTop" }, data: [{ yAxis: limitMs }] },
      }),
      // The lowest-to-highest band: an invisible base at the lowest with the range stacked on it.
      line("lowest", 1, rows.map(r => det(r, x => ms(x.rtt_min_ns))), { stack: "rtt", lineStyle: { opacity: 0 }, tooltip: { show: false } }),
      line("lowest to highest", 1, rows.map(r => det(r, x => (x.rtt_max_ns == null ? null : ms(x.rtt_max_ns - x.rtt_min_ns)))), {
        stack: "rtt", lineStyle: { opacity: 0 }, areaStyle: { color: c.s1, opacity: 0.15 }, itemStyle: { color: c.s1, opacity: 0.3 } }),
      line("median", 1, rows.map(r => det(r, x => ms(x.rtt_median_ns)))),
      line("Clock offset", 2, rows.map(r => det(r, x => ms(x.clock_offset_ns)))),
    ],
  }, { replaceMerge: ["series"] });
}
$("clock-range").onchange = () => { clockData = null; refresh(); };

// -- Firmware (of one device) --

const FW_RESULTS = { updated: "updated", deferred: "put off by the device", failed: "failed", rolled_back: "rolled back" };
let fwFormFor = null, fwDirty = false;
function renderDevFirmware(c) {
  const fw = c.firmware, b = c.build, run = fw.running, t = fw.target, api = apiView(c);
  const line = x => `${fwCommit(x)}, built ${fwBuilt(x)}`;
  let target = "";
  if (!fw.supported) target = "";
  else if (!t) target = tile("Should run", "-", fw.policy ? "no build for this choice yet: upload one on the Firmware page" : "what it runs now (no choice made)");
  else if (run && t.id === run.id) target = tile("Should run", `${fwTag(t)} ${esc(t.describe)}`, "up to date: it runs this build", "ok");
  else if (fw.blocked.includes(t.id)) target = tile("Should run", `${fwTag(t)} ${esc(t.describe)}`,
    "failed on this device: not offered again until the choice is set again", "bad");
  else target = tile("Should run", `${fwTag(t)} ${esc(t.describe)}`, `${line(t)}<br>offered at ${esc(nextContact(c))}`, "warn");
  const l = fw.last, lb = l && firmwareBuilds.find(x => x.id === l.build_id);
  setHtml("fw-tiles", [
    tile("Runs", b ? `${fwTag(b)} ${esc(b.describe)}` : esc(c.version || "-"),
      b ? `${line(b)}<br>${esc(b.hardware_id)}` + (run ? "" : `<br><span class="muted">not among the uploaded builds</span>`) : "does not report its build"),
    target,
    l ? tile("Last update", esc(FW_RESULTS[l.result] || l.result),
      [lb ? esc(lb.describe) : `build ${l.build_id}`, l.detail ? esc(l.detail) : "", ago(l.at)].filter(Boolean).join(", "),
      { updated: "ok", deferred: "warn", failed: "bad", rolled_back: "bad" }[l.result] || "") : "",
    tile("API", `<span class="${api.cls === "muted" ? "" : api.cls}">${esc(api.text.replace(/^API /, ""))}</span>`, esc(api.sub), "", "grow"),
  ].join(""));
  setHtml("fw-info", info("Latest dev fw: the newest build for this device's hardware, by build time. Latest stable fw: the newest " +
    "built at a tag from a clean tree. Until a choice is made, the device stays on what it runs.\n" +
    "The device is offered the build in its heartbeat replies while it runs another, and installs it unless its battery is low (it then tries " +
    "again later). A build that fails or is rolled back is not offered again until the choice is set again."));
  const sel = $("fw-policy"), bsel = $("fw-build");
  const options = b ? firmwareBuilds.filter(x => x.hardware_id === b.hardware_id) : [];
  if (fwFormFor !== c.id || !fwDirty) {
    if (fwFormFor !== c.id) setText("fw-msg", "");
    fwFormFor = c.id;
    fwDirty = false;
    sel.value = fw.policy?.policy || "manual";
    setHtml(bsel, options.map(x => `<option value="${x.id}">${x.tag && !x.dirty ? esc(x.tag) : "dev"}, ${esc(x.describe)}, built ${fwBuilt(x)}` +
      `${run && x.id === run.id ? " (running)" : ""}</option>`).join(""));
    const chosen = fw.policy?.build_id ?? run?.id;
    if (chosen != null && options.some(x => x.id === chosen)) bsel.value = String(chosen);
  }
  $("fw-build-label").hidden = sel.value !== "manual";
  $("fw-form").hidden = !fw.supported;
  for (const id of ["fw-policy", "fw-build", "fw-set"]) $(id).disabled = !fw.supported;
  if (fw.supported && sel.value === "manual" && !options.length) $("fw-set").disabled = true;
  setHtml("fw-hint", !fw.supported ? `Updates over the air are not available for this device: ${esc(fw.unsupported)}.`
    : !options.length ? `There is no build for this hardware yet. <a href="#/firmware">Upload one on the Firmware page.</a>` : "");
}
$("fw-form").addEventListener("input", () => { fwDirty = true; const c = client(fwFormFor); if (c) renderDevFirmware(c); });
$("fw-form").addEventListener("submit", async ev => {
  ev.preventDefault();
  const policy = $("fw-policy").value;
  try {
    await apiSend("PUT", `/clients/${fwFormFor}/firmware`, policy === "manual" ? { policy, build_id: Number($("fw-build").value) } : { policy });
    fwDirty = false;
    firmwareAt = 0;
    setHtml("fw-msg", `<span class="ok">Set.</span>`);
    refresh();
  } catch (e) { setHtml("fw-msg", `<span class="bad">${esc(e.message)}</span>`); }
});

// -- Setup: name, position, identity, removal --

let cfgFor = null, cfgDirty = false;
function renderSetup(c) {
  if (cfgFor !== c.id || !cfgDirty) {
    if (cfgFor !== c.id) setText("cfg-msg", "");
    cfgFor = c.id;
    cfgDirty = false;
    $("cfg-name").value = c.name || "";
    for (const k of ["x", "y", "z"]) $("cfg-" + k).value = c.position ? c.position[k] : "";
  }
  $("cfg-name").placeholder = c.suggested_name || cleanHostname(c.hostname) || "";
  setText("cfg-name-hint", c.name ? "" : c.suggested_name ? `Shown as "${c.suggested_name}", the name it suggests for itself, until you set one.`
    : `Shown as "${label(c.id)}" until you set one.`);
  $("setup-revert").disabled = !cfgDirty;
  const cap = c.capabilities || {};
  const fact = (k, v) => v ? `<dt>${k}</dt><dd>${v}</dd>` : "";
  setHtml("setup-facts", [
    fact("Device ID", `<span class="mono">${esc(c.id)}</span>`),
    fact("Hostname", esc(c.hostname)),
    fact("Address", `<span class="mono">${esc(c.last_addr)}</span>`),
    fact("First seen", esc(fmtFull(c.first_seen))),
    fact("Software", esc(c.version)),
    fact("Hardware", esc(c.build?.hardware_id)),
    fact("Microphone", esc(cap.device_name)),
    fact("Audio", cap.sample_rate ? `${cap.sample_rate / 1000} kHz, ${plural(cap.channels, "channel")}, ${esc(cap.sample_format)}` : ""),
    fact("Clock source", esc(cap.clock_source)),
  ].join(""));
  // The map: every positioned device, this one highlighted, and what is being typed.
  const num = k => { const v = $("cfg-" + k).value.trim(); return v === "" ? NaN : Number(v); };
  const [dx, dy, dz] = [num("x"), num("y"), num("z")];
  const mics = App.clients.filter(x => x.position).map(x => ({ id: x.id, label: label(x.id), ...x.position,
    cls: (x.active ? "" : "inactive") + (x.id === c.id ? " selected" : "") }));
  const saved = mics.find(m => m.id === c.id);
  if (Number.isFinite(dx) && Number.isFinite(dy) && !(saved && saved.x === dx && saved.y === dy)) {
    mics.push({ id: c.id, label: `${label(c.id)} (unsaved)`, x: dx, y: dy, z: Number.isFinite(dz) ? dz : 0, cls: "draft" });
  }
  renderMicMap($("setup-map"), mics, "No device has a position yet.");
}
$("setup-form").addEventListener("input", () => { cfgDirty = true; const c = client(cfgFor); if (c) renderSetup(c); });
$("setup-revert").onclick = () => { cfgDirty = false; const c = client(cfgFor); if (c) renderSetup(c); };
$("setup-form").addEventListener("submit", async ev => {
  ev.preventDefault();
  const id = cfgFor, vals = ["x", "y", "z"].map(k => $("cfg-" + k).value.trim());
  let position = null;
  if (vals.some(v => v !== "")) {
    const nums = vals.map(v => Number(v === "" ? "0" : v));
    if (nums.some(n => !Number.isFinite(n))) { setHtml("cfg-msg", `<span class="bad">The position must be numbers.</span>`); return; }
    position = { x: nums[0], y: nums[1], z: nums[2] };
  }
  try {
    await apiSend("PUT", "/clients/" + id, { name: $("cfg-name").value.trim(), position });
    cfgDirty = false;
    setHtml("cfg-msg", `<span class="ok">Saved.</span>`);
    refresh();
  } catch (e) { setHtml("cfg-msg", `<span class="bad">${esc(e.message)}</span>`); }
});
$("dev-delete").onclick = async () => {
  const c = client(devId);
  if (!c) return;
  const name = label(c.id);
  const yes = await confirmDialog({
    title: `Delete ${name}?`, action: "Delete", danger: true,
    body: `<p><span class="mono">${esc(c.id)}</span></p><p>Its name, position and battery history are removed. What it recorded is kept.</p>` +
      (c.active ? `<p class="warn">It is online: it will register again, unnamed, within seconds.</p>` : ""),
  });
  if (!yes) return;
  try {
    await api(`/clients/${c.id}`, { method: "DELETE" });
    toast(`Deleted ${name}.`, "ok");
    devId = null;
    location.hash = "#/devices";
    refresh();
  } catch (e) { toast(`Could not delete ${name}: ${e.message}`, "bad"); }
};

// ---- the event log, as a view used twice: for everything, and for one device ----

const EVENT_PAGE = 200, EVENT_KEEP = 3000;
/// Builds an event view inside `box`. `device` is a function giving the one device to show, or
/// absent for a view with a device filter of its own.
function eventView(box, { key, device }) {
  const saved = prefs(key, { off: EVENT_GROUPS.filter(g => g.quiet).map(g => g.id) });
  const st = { off: new Set(saved.off), device: "", q: "", paused: false, rows: [], loadedFor: null, more: false, open: new Set() };
  box.innerHTML = `<div class="panel">
    <div class="panel-head">
      ${device ? "" : `<label>Device <select data-role="device"><option value="">any</option></select></label>`}
      <div class="checks" data-role="groups">${EVENT_GROUPS.map(g =>
        `<label><input type="checkbox" value="${g.id}"> ${esc(g.label)}</label>`).join("")}</div>
    </div>
    <div class="panel-head">
      <label>Find <input type="search" data-role="q" size="24" placeholder="text in what is listed"></label>
      <button type="button" data-role="pause" title="Stop adding new events while you read"></button>
      <span class="muted" data-role="count"></span>
      <span class="spacer"></span>
      <span class="hint" style="margin: 0">Click an event for everything the server recorded about it.</span>
    </div>
    <div class="panel-body flush scroll-x"><table class="events">
      <thead><tr><th>Time</th>${device ? "" : "<th>Device</th>"}<th>Kind</th><th>What happened</th></tr></thead><tbody></tbody></table></div>
    <div class="panel-body" data-role="more-box" hidden><button type="button" data-role="more">Load older events</button></div></div>`;
  const el = role => box.querySelector(`[data-role="${role}"]`);
  const cols = device ? 3 : 4;
  const dev = () => device ? device() : st.device;
  const queryKey = () => `${dev() || ""}|${[...st.off].sort().join()}`;
  const query = extra => {
    const q = new URLSearchParams(extra);
    if (dev()) q.set("client_id", dev());
    const exclude = EVENT_GROUPS.filter(g => st.off.has(g.id)).flatMap(g => g.kinds);
    if (exclude.length) q.set("exclude_kind", exclude.join(","));
    return "/events?" + q;
  };
  function render() {
    for (const cb of el("groups").querySelectorAll("input")) cb.checked = !st.off.has(cb.value);
    if (!device) {
      const sel = el("device");
      if (document.activeElement !== sel) {
        setHtml(sel, `<option value="">any</option>` + App.clients.map(c => `<option value="${c.id}">${esc(label(c.id))}</option>`).join(""));
        sel.value = st.device;
      }
    }
    setText(el("pause"), st.paused ? "Resume" : "Pause");
    el("pause").classList.toggle("on", st.paused);
    const q = st.q.toLowerCase();
    const rows = st.rows.map(e => ({ e, text: eventSummary(e) }))
      .filter(r => !q || r.text.toLowerCase().includes(q) || r.e.kind.includes(q));
    setText(el("count"), st.loadedFor === null ? "Loading..." : `${plural(rows.length, "event")}` + (q ? ` of ${st.rows.length} loaded` : "") + (st.paused ? ", paused" : ", following"));
    el("more-box").hidden = !st.more;
    let lastDay = "", html = "";
    for (const { e, text } of rows) {
      const dayKey = dateKey(new Date(e.ts / 1e6));
      if (dayKey !== lastDay) { lastDay = dayKey; html += `<tr class="day"><td colspan="${cols}">${fmtDay(e.ts / 1e6)}</td></tr>`; }
      const level = eventLevel(e), group = eventGroup(e.kind);
      html += `<tr class="ev ${level} ${group === "log" ? "log" : ""}" data-id="${e.id}">
        <td class="when" title="${fmtFull(e.ts)}">${fmtClock(e.ts / 1e6, true)}</td>
        ${device ? "" : `<td class="nowrap">${e.client_id ? `<a href="${deviceLink(e.client_id)}">${esc(label(e.client_id))}</a>` : `<span class="muted">server</span>`}</td>`}
        <td class="kind mono">${esc(e.kind)}</td><td class="what">${esc(text)}</td></tr>`;
      if (st.open.has(e.id)) {
        html += `<tr class="raw"><td colspan="${cols}"><pre>#${e.id}  ${fmtFull(e.ts)}  ${esc(e.kind)}\n${esc(JSON.stringify(e.detail, null, 2))}</pre></td></tr>`;
      }
    }
    setHtml(box.querySelector("tbody"), html || `<tr><td colspan="${cols}" class="empty">${st.loadedFor === null ? "Loading..." : "No events to show."}</td></tr>`);
  }
  async function tick() {
    const key = queryKey();
    if (st.loadedFor !== key) {
      const rows = await api(query({ limit: EVENT_PAGE }));
      if (key !== queryKey()) return;
      Object.assign(st, { rows, loadedFor: key, more: rows.length >= EVENT_PAGE });
      st.open.clear();
    } else if (!st.paused) {
      const fresh = await api(query({ limit: 1000, ...(st.rows[0] ? { after_id: st.rows[0].id } : {}) }));
      if (key !== queryKey()) return;
      if (fresh.length) st.rows = fresh.concat(st.rows).slice(0, EVENT_KEEP);
    }
    render();
  }
  el("groups").addEventListener("change", e => {
    if (e.target.checked) st.off.delete(e.target.value); else st.off.add(e.target.value);
    saved.off = [...st.off];
    saved.save();
    refresh();
  });
  if (!device) el("device").addEventListener("change", e => { st.device = e.target.value; refresh(); });
  el("q").addEventListener("input", e => { st.q = e.target.value.trim(); render(); });
  el("pause").onclick = () => { st.paused = !st.paused; render(); if (!st.paused) refresh(); };
  el("more").onclick = async () => {
    const last = st.rows.at(-1);
    if (!last) return;
    const older = await api(query({ limit: EVENT_PAGE, before_id: last.id }));
    st.rows = st.rows.concat(older);
    st.more = older.length >= EVENT_PAGE;
    render();
  };
  box.querySelector("tbody").addEventListener("click", e => {
    if (e.target.closest("a")) return;
    const tr = e.target.closest("tr.ev");
    if (!tr || window.getSelection().toString()) return;
    const id = Number(tr.dataset.id);
    if (st.open.has(id)) st.open.delete(id); else st.open.add(id);
    render();
  });
  render();
  return {
    tick,
    /// Shows these groups (as well as those already shown), e.g. from a link to a device's trace log.
    show(groups) { for (const g of groups) st.off.delete(g); },
    setDevice(id) { if (!device) st.device = id || ""; },
  };
}
const allEvents = eventView($("all-events"), { key: "bps.events" });
const devEvents = eventView($("dev-events"), { key: "bps.deviceEvents", device: () => devId });

// ---- Firmware: the builds kept by the server (updates over the air, API 1.2.0) ----

async function refreshFirmware() {
  if (Date.now() - firmwareAt < 4000) return;
  firmwareBuilds = await api("/firmware");
  firmwareAt = Date.now();
}
function renderFirmwarePage() {
  const groups = new Map();
  for (const b of firmwareBuilds) {  // newest build first, as listed
    if (!groups.has(b.hardware_id)) groups.set(b.hardware_id, []);
    groups.get(b.hardware_id).push(b);
  }
  setText("fw-summary", firmwareBuilds.length ? `${plural(firmwareBuilds.length, "build")} for ${plural(groups.size, "kind", "kinds")} of hardware` : "");
  setHtml("fw-groups", [...groups.keys()].sort().map(hw => `
    <div class="panel">
      <div class="panel-head"><h2>${esc(hw)}</h2><span class="muted">${plural(groups.get(hw).length, "build")}, newest first</span></div>
      <div class="panel-body flush scroll-x"><table class="data">
        <thead><tr><th>Version</th><th>Commit</th><th>Built</th><th class="num">Size</th><th>Running it</th>
          <th title="Devices set to this build that run another: offered it at each heartbeat.">Updating to it</th><th>Uploaded</th><th></th></tr></thead>
        <tbody>${groups.get(hw).map(b => `<tr>
          <td>${fwTag(b)} <span class="muted">${esc(b.describe)}</span></td>
          <td>${fwCommit(b)}</td>
          <td class="nowrap">${fwBuilt(b)}</td>
          <td class="num nowrap" title="MD5 ${esc(b.md5)}">${fmtBytes(b.size)}</td>
          <td>${deviceLinks(b.clients_running) || "-"}</td>
          <td>${deviceLinks(b.clients_targeting) || "-"}</td>
          <td class="nowrap">${ago(b.uploaded_at)}</td>
          <td class="num"><button type="button" class="danger small" data-delete-build="${b.id}"
            ${b.clients_pinned.length ? `disabled title="${esc("Chosen by hand for " + b.clients_pinned.map(label).join(", ") + ": choose another build for them first.")}"` : ""}>Delete</button></td>
        </tr>`).join("")}</tbody>
      </table></div>
    </div>`).join("") || `<div class="panel"><p class="empty">No firmware builds yet. Upload one above.</p></div>`);
}
$("fw-upload-form").addEventListener("submit", async ev => {
  ev.preventDefault();
  const input = $("fw-file"), file = input.files[0], say = (cls, html) => setHtml("fw-upload-msg", `<span class="${cls}">${html}</span>`);
  if (!file) { say("bad", "Choose a firmware.bin first."); return; }
  $("fw-upload").disabled = true;
  say("muted", "Uploading...");
  try {
    const r = await fetch(API + "/firmware", { method: "POST", headers: { "content-type": "application/octet-stream" }, body: file });
    const body = await r.json().catch(() => ({}));
    if (!r.ok) throw new Error(body.error || `${r.status} ${r.statusText}`);
    say("ok", (r.status === 201 ? "Uploaded: " : "Already there: ") + `${esc(body.hardware_id)}, ${fwTag(body)} ${esc(body.describe)}.`);
    input.value = "";
    firmwareAt = 0;
    refresh();
  } catch (e) { say("bad", esc(e.message)); } finally { $("fw-upload").disabled = false; }
});
$("fw-groups").addEventListener("click", async e => {
  const button = e.target.closest("button[data-delete-build]");
  if (!button) return;
  const b = firmwareBuilds.find(b => b.id === Number(button.dataset.deleteBuild));
  if (!b) return;
  const yes = await confirmDialog({
    title: `Delete build ${b.describe}?`, action: "Delete", danger: true,
    body: `<p>${esc(b.hardware_id)}, built ${fwBuilt(b)}.</p><p>Devices set to a latest build move to the newest one left.</p>`,
  });
  if (!yes) return;
  try {
    await api(`/firmware/${b.id}`, { method: "DELETE" });
    toast(`Deleted build ${b.describe}.`, "ok");
    firmwareAt = 0;
    refresh();
  } catch (e2) { toast(`Could not delete the build: ${e2.message}`, "bad"); }
});

// ---- Settings: each section is a form saved on its own ----

let settingsView = null;          // GET /settings as last loaded
const sectionDirty = new Set();   // sections being edited: not overwritten by a reload
const SECTIONS = {
  site: {
    fill(v) {
      const site = v.settings.site;
      $("set-lat").value = site ? site.latitude : "";
      $("set-lon").value = site ? site.longitude : "";
    },
    read() {
      const lat = $("set-lat").value.trim(), lon = $("set-lon").value.trim();
      if (lat === "" && lon === "") return { site: null };
      const site = { latitude: Number(lat), longitude: Number(lon) };
      if (lat === "" || lon === "" || !Number.isFinite(site.latitude) || !Number.isFinite(site.longitude))
        throw new Error("Enter both latitude and longitude as numbers, or leave both empty.");
      return { site };
    },
  },
  confidence: {
    fill(v) {
      const radios = (name, selected) => v.levels.map(l => `<label><input type="radio" name="${name}" value="${l.id}" ${l.id === selected ? "checked" : ""}> ` +
        `${esc(l.label)} (${fmtPct(l.value)} or more)</label>`).join("");
      $("set-conf-expected").innerHTML = radios("conf-expected", v.settings.min_confidence_expected);
      $("set-conf-unexpected").innerHTML = radios("conf-unexpected", v.settings.min_confidence_unexpected);
    },
    read() {
      const checked = name => document.querySelector(`input[name="${name}"]:checked`).value;
      return { min_confidence_expected: checked("conf-expected"), min_confidence_unexpected: checked("conf-unexpected") };
    },
  },
  trigger: {
    fill(v) {
      const t = v.trigger;
      $("set-trig-type").value = t.trigger_type;
      if (t.trigger_type === "energy") {
        $("set-trig-thr").value = t.threshold_db; $("set-trig-lo").value = t.band_low_hz;
        $("set-trig-hi").value = t.band_high_hz; $("set-trig-frame").value = t.frame_ms;
      }
    },
    read() {
      const num = id => Number($(id).value);
      const trigger = { trigger_type: $("set-trig-type").value, threshold_db: num("set-trig-thr"), band_low_hz: num("set-trig-lo"),
                        band_high_hz: num("set-trig-hi"), frame_ms: Math.round(num("set-trig-frame")) };
      if (![trigger.threshold_db, trigger.band_low_hz, trigger.band_high_hz, trigger.frame_ms].every(Number.isFinite) || $("set-trig-thr").value === "")
        throw new Error("Enter the trigger's settings as numbers.");
      return { trigger };
    },
  },
  validation: {
    fill(v) {
      const val = v.settings.validation;
      $("set-validation").value = val.validation_type;
      $("set-val-model").value = val.validation_model ?? "";
      $("set-val-thr").value = val.validation_threshold ?? "";
      showValidationFields();
    },
    read() {
      const validation = { validation_type: $("set-validation").value };
      if (validation.validation_type === "model") {
        validation.validation_model = $("set-val-model").value.trim();
        const thr = $("set-val-thr").value.trim();
        if (thr !== "") validation.validation_threshold = Number(thr);
      }
      return { validation };
    },
  },
};
function showValidationFields() {
  const model = $("set-validation").value === "model";
  for (const el of document.querySelectorAll(".val-model")) el.hidden = !model;
}
function renderSettings() {
  const v = settingsView;
  if (!v) return;
  for (const [name, s] of Object.entries(SECTIONS)) if (!sectionDirty.has(name)) s.fill(v);
  const ls = v.local_species;
  setText("set-species", {
    ready: `${ls.count} species are expected at this location.`,
    no_location: "No location is set: every species is treated as expected.",
    no_geo_model: "The range model is not installed (run scripts/fetch-birdnet.sh): every species is treated as expected.",
    error: "The expected species could not be worked out: " + (ls.error || "unknown error"),
  }[ls.status] || "");
  $("species-list").hidden = ls.status !== "ready";
  const muted = v.settings.muted_species || [];
  setHtml("set-muted", muted.length
    ? `<p>Still recorded and identified, but left out of the Birds pages unless asked for.</p><table class="data"><tbody>` +
      muted.map(s => `<tr><td><i>${esc(s)}</i></td><td class="num"><button type="button" class="small" data-unmute="${esc(s)}">Unmute</button></td></tr>`).join("") + `</tbody></table>`
    : `<p class="muted">None. A species the identifier keeps mistaking something else for can be muted from the Birds pages,
        where it is listed: it is still recorded, but left out of Today and Recordings.</p>`);
  const s = App.status;
  if (s) {
    const fact = (k, val) => `<dt>${k}</dt><dd>${val}</dd>`;
    setHtml("set-server", [
      fact("Version", `bsp-server ${esc(s.version)} <span class="mono">(${esc(s.build)})</span>`),
      fact("Client API", esc(s.api_version)),
      fact("Identifier", esc(s.identifier)),
      fact("Running since", `${esc(fmtFull(s.started_at))} (${fmtDuration((s.server_time - s.started_at) / 1e9)})`),
      fact("Time sync", `UDP port ${s.udp_port}; a device is synced within &plusmn;${(s.max_clock_offset_us / 1000).toFixed(1)} ms`),
      fact("Access points", (s.access_points || []).map(a => `<span class="mono">${esc(a.bssid)}</span>` +
        (a.line ? ` (${plural(a.line.clients, "device")}, &plusmn;${fmtNsMs(a.line.error_ns)})` : "")).join(", ") || "none followed"),
    ].join(""));
  }
}
async function loadSettings() {
  settingsView = await api("/settings");
  renderSettings();
}
for (const form of document.querySelectorAll("#page-settings form[data-section]")) {
  const name = form.dataset.section, msg = form.querySelector(".form-msg");
  form.addEventListener("input", () => { sectionDirty.add(name); msg.innerHTML = `<span class="dirty-note">not saved yet</span>`; });
  form.addEventListener("submit", async ev => {
    ev.preventDefault();
    try {
      if (!settingsView) throw new Error("The settings have not loaded yet.");
      // The server takes the settings whole: send what it has, with this section's fields replaced.
      settingsView = await apiSend("PUT", "/settings", { ...settingsView.settings, ...SECTIONS[name].read() });
      sectionDirty.delete(name);
      renderSettings();
      msg.innerHTML = `<span class="ok">Saved.</span>`;
      if (name === "site" && $("species-list").open) loadSpeciesList();
    } catch (e) { msg.innerHTML = `<span class="bad">${esc(e.message)}</span>`; }
  });
}
$("set-validation").addEventListener("change", showValidationFields);
$("set-detect").onclick = () => {
  const say = text => setText("set-loc-msg", text);
  if (!window.isSecureContext || !navigator.geolocation) {
    say("Browsers only give a location to https:// or http://localhost pages. Open this page that way, or type the coordinates.");
    return;
  }
  say("Asking this device for its location...");
  navigator.geolocation.getCurrentPosition(pos => {
    $("set-lat").value = pos.coords.latitude.toFixed(4);
    $("set-lon").value = pos.coords.longitude.toFixed(4);
    $("set-lat").dispatchEvent(new Event("input", { bubbles: true }));
    say(`Found, to about ${Math.round(pos.coords.accuracy)} m. Save the location to use it.`);
  }, err => say("The location could not be found: " + err.message), { enableHighAccuracy: false, timeout: 15000 });
};
async function loadSpeciesList() {
  const ls = await api("/local-species");
  const rows = ls.status === "ready" ? ls.species : [];
  $("species-list").querySelector("tbody").innerHTML = rows.map(s => `<tr><td>${esc(s.species.common)}</td>
    <td><i>${wikiName(s.species.scientific)}</i></td><td class="num">${fmtPct(s.probability)}</td></tr>`).join("");
}
$("species-list").addEventListener("toggle", () => { if ($("species-list").open) loadSpeciesList().catch(e => toast(e.message, "bad")); });
$("set-muted").addEventListener("click", async e => {
  const b = e.target.closest("button[data-unmute]");
  if (!b) return;
  try { await apiSend("PUT", "/species/muted", { scientific: b.dataset.unmute, muted: false }); await loadSettings(); }
  catch (e2) { toast("Could not unmute: " + e2.message, "bad"); }
});

// ---- a device asking for its position ----

function renderPositioning() {
  const p = App.positioning, box = $("positioning");
  box.hidden = !p;
  if (!p) return;
  const left = Math.max(0, Math.round((p.expires_at - Date.now() * 1e6) / 1e9));
  setText("positioning-text", `${label(p.client_id)} is asking to have its position set (for another ${fmtDuration(left)}).`);
}
$("positioning-set").onclick = () => { if (App.positioning) location.hash = deviceLink(App.positioning.client_id, "setup"); };
$("positioning-dismiss").onclick = async () => {
  try { await api("/positioning-request", { method: "DELETE" }); refresh(); } catch (e) { toast(e.message, "bad"); }
};

// ---- start ----

start({
  pages: ["devices", "firmware", "events", "settings"],
  onRoute(r) {
    if (r.page === "devices") enterDevices(r);
    if (r.page === "events") {
      if (r.params.has("device")) allEvents.setDevice(r.params.get("device"));
      if (r.params.get("show")) allEvents.show(r.params.get("show").split(","));
    }
    if (r.page === "settings") loadSettings().catch(e => toast(e.message, "bad"));
    if (r.page === "firmware") renderFirmwarePage();
  },
  async onTick(r) {
    const s = App.status;
    if (s) setText("foot-server", `bsp-server ${s.version} (${s.build}), client API ${s.api_version}, identifying with ${s.identifier}`);
    renderPositioning();
    if (r.page === "devices") await refreshDevices();
    if (r.page === "firmware") { await refreshFirmware(); renderFirmwarePage(); }
    if (r.page === "events") await allEvents.tick();
    if (r.page === "settings") renderSettings();
  },
});
window.addEventListener("themechange", () => { if (route().page === "devices") renderDevices(); });
