// The Birds workspace (index.html): what was heard. Four pages over the control API:
// Today (one day at a glance), Recordings (every identified call, one at a time), Species
// (everything heard here) and Map (where the microphones are; positioning is still a stub).
"use strict";

// ---- shared: recordings as pictures, muting ----

/// A recording's picture with a numbered band per identified call (see groupSamples).
function clipHtml(s, big) {
  const len = Math.max(1, s.to - s.from);
  const pct = ns => Math.max(0, Math.min(1, ns / len)) * 100;
  const bands = s.highlights.map(h => {
    const left = pct(h.start - s.from), width = Math.max(0.6, pct(h.end - s.from) - left);
    return `<div class="hl" data-n="${h.n}" style="--c:var(--hl-${h.colour});left:${left.toFixed(2)}%;width:${width.toFixed(2)}%;` +
      `top:${(h.lane / s.lanes * 100).toFixed(2)}%;height:${(100 / s.lanes).toFixed(2)}%">${big ? `<span class="hl-num">${h.n}</span>` : ""}</div>`;
  }).join("");
  if (!s.audio) return `<div class="clip no-audio">no audio was kept for this call${bands}</div>`;
  return `<div class="clip" data-url="${esc(sampleUrl(s.client, s.from, s.to))}" data-wave="${esc(sampleUrl(s.client, s.from, s.to, "waveform"))}" ` +
    `title="Click to play or stop">${bands}<div class="playhead"></div></div>`;
}
/// The species in a recording, each once, most confident first.
function sampleSpecies(s) {
  const by = new Map();
  for (const h of s.highlights) {
    const d = h.detection, cur = by.get(d.species.scientific);
    if (!cur || d.confidence > cur.confidence) by.set(d.species.scientific, { ...d.species, confidence: d.confidence, unexpected: d.unexpected });
  }
  return [...by.values()].sort((a, b) => b.confidence - a.confidence);
}

/// Mutes or unmutes a species everywhere, and redraws what that changes.
async function setMuted(scientific, common, muted) {
  try {
    await apiSend("PUT", "/species/muted", { scientific, muted });
  } catch (e) { toast(`Could not ${muted ? "mute" : "unmute"} ${common}: ${e.message}`, "bad"); return; }
  toast(muted ? `${common} is muted: it is left out of Today and Recordings. Unmute it on the Species page.` : `${common} is shown again.`, "ok");
  timelineAt = 0; latestAt = 0; speciesAt = 0; recLoadedAt = 0; recRows = []; recKey = null;
  refresh();
}
document.addEventListener("click", e => {
  const b = e.target.closest?.("button[data-mute]");
  if (b) setMuted(b.dataset.mute, b.dataset.common, b.dataset.muted !== "true");
});
function muteButton(scientific, common, muted) {
  return `<button type="button" class="small" data-mute="${esc(scientific)}" data-common="${esc(common)}" data-muted="${!!muted}" ` +
    `title="${muted ? "Show this species again" : "Leave this species out of Today and Recordings: for one the identifier keeps mistaking something else for"}">` +
    `${muted ? "Unmute" : "Mute"}</button>`;
}
function recordingsLink(scientific, extra) { return href("recordings", [], { species: scientific, ...extra }); }
const OCCURRENCE = { expected: "expected", unexpected: `<span class="tag">unexpected</span>`, unknown: `<span class="muted">no range data</span>` };

// ---- Today ----

const dash = prefs("bsp.dash", { filter: "expected", order: "first" });
const today = () => dateKey(new Date());
let day = today();
let followToday = true;   // move on at midnight, until another day is picked
let timeline = { detections: [], truncated: false, loaded: false };
let timelineAt = 0;
let latest = null, latestAt = 0;
const TIMELINE_REFRESH_MS = 10000, LATEST_REFRESH_MS = 6000, LANE_HEIGHT = 26;

/// Shows another day. The day is kept in the address (#/today?day=...), so it can be linked to.
function setDay(key) {
  if (!key || !/^\d{4}-\d{2}-\d{2}$/.test(key)) return;
  const next = href("today", [], { day: key === today() ? "" : key });
  if (location.hash !== next && !(location.hash === "" && key === today())) { location.hash = next; return; }
  showDay(key);
}
function showDay(key) {
  if (key === day && timeline.loaded) return;
  day = key;
  followToday = key === today();
  timeline = { detections: [], truncated: false, loaded: false };
  timelineAt = 0;
  renderToday();
  refresh();
}
async function refreshToday() {
  if (followToday && day !== today()) { showDay(today()); return; }
  const isToday = day === today();
  // Past days don't change; today is refetched every few seconds.
  if (!timelineAt || (isToday && Date.now() - timelineAt >= TIMELINE_REFRESH_MS)) {
    const asked = day;
    const data = await api(`/timeline?from=${msToNs(dayStartMs(asked))}&to=${msToNs(dayEndMs(asked))}`);
    if (asked !== day) return;
    timeline = { ...data, loaded: true };
    timelineAt = Date.now();
  }
  if (isToday && Date.now() - latestAt >= LATEST_REFRESH_MS) {
    const rows = await api("/detections?limit=1&hide_muted=true");
    latest = rows[0] ? groupSamples(rows)[0] : null;
    latestAt = Date.now();
  }
  renderToday();
}
const FILTER_LABELS = { expected: "expected species", known: "expected and unexpected species", all: "species and sounds" };
/// The "Show" choice; without a site location nothing can be classified, so everything shows.
function laneMode() { return timeline.site_set === false ? "all" : dash.filter; }

function renderToday() {
  const isToday = day === today();
  setText("day-title", fmtDay(dayStartMs(day)));
  if ($("day").value !== day) $("day").value = day;
  $("day-next").disabled = isToday;
  $("day-today").disabled = isToday;
  $("lane-filter").value = dash.filter;
  $("lane-order").value = dash.order;
  $("lane-filter").disabled = timeline.site_set === false;

  const mode = laneMode();
  const all = speciesLanes(timeline.detections, dash.order);
  const lanes = all.filter(l => laneShown(l, mode));
  const shown = lanes.flatMap(l => l.items);
  const hiddenUnexpected = all.filter(l => !l.muted && l.occurrence === "unexpected" && !laneShown(l, mode));
  const hiddenUnknown = all.filter(l => !l.muted && l.occurrence === "unknown" && !laneShown(l, mode));
  const muted = all.filter(l => l.muted);

  const top = [...lanes].sort((a, b) => b.items.length - a.items.length)[0];
  const last = shown.length ? shown.reduce((a, b) => (a.time > b.time ? a : b)) : null;
  setHtml("day-tiles", [
    tile("Species heard", timeline.loaded ? lanes.length : "-", mode === "expected" && hiddenUnexpected.length
      ? `plus ${hiddenUnexpected.length} unexpected` : esc(FILTER_LABELS[mode])),
    tile("Recordings", timeline.loaded ? shown.length : "-", timeline.truncated ? "the most recent of a very busy day" : "identified calls"),
    tile("Most active", top ? esc(top.common) : "-", top ? plural(top.items.length, "recording") : ""),
    tile("Last heard", last ? esc(last.common) : "-", last ? (isToday ? ago(last.time) : "at " + fmtClock(last.time / 1e6)) : ""),
  ].join(""));

  renderLatest(isToday);

  // What the filter leaves out, and why everything shows without a site location.
  const notes = [];
  if (timeline.site_set === false) {
    notes.push(`The site's location is not set, so species cannot be sorted into expected and unexpected: everything is shown. ` +
      `<a href="/system#/settings">Set it in System settings.</a>`);
  } else {
    const parts = [];
    if (hiddenUnexpected.length) parts.push(plural(hiddenUnexpected.length, "unexpected species", "unexpected species"));
    if (hiddenUnknown.length) {
      const eg = hiddenUnknown.slice(0, 3).map(l => esc(l.common)).join(", ");
      parts.push(`${plural(hiddenUnknown.length, "other sound")} without range data (${eg}${hiddenUnknown.length > 3 ? ", and more" : ""})`);
    }
    if (parts.length) notes.push(`Not shown: ${parts.join(" and ")}. Change "Show" above to include them.`);
  }
  if (muted.length) {
    notes.push(`${plural(muted.length, "muted species", "muted species")} left out (${muted.map(l => esc(l.common)).join(", ")}). ` +
      `<a href="#/species">Manage on the Species page.</a>`);
  }
  setHtml("timeline-note", notes.join(" "));

  // Legend: only the kinds of mark on screen.
  const kinds = new Set(lanes.map(l => l.occurrence));
  const key = (v, text) => `<span><span class="key" style="background: var(${v})"></span>${text}</span>`;
  setHtml("timeline-legend",
    (kinds.has("expected") || kinds.has("unknown") ? key("--tick", kinds.has("unknown") ? "expected or unclassified" : "expected") : "") +
    (kinds.has("unexpected") ? key("--tick-unexpected", "unexpected at this site") : ""));

  setText("day-species-title", `Species ${isToday ? "today" : "that day"}` + (mode === "expected" ? "" : ` (${FILTER_LABELS[mode]})`));
  setHtml(document.querySelector("#day-species tbody"), lanes.map(l => `<tr class="${l.occurrence === "unexpected" ? "unexpected" : ""}">
      <td><a href="${recordingsLink(l.scientific, { from: day, to: day })}"><b>${esc(l.common)}</b></a>
        ${l.occurrence === "unexpected" ? ` <span class="tag" title="Not expected at this site">unexpected</span>` : ""}<br><i class="muted">${wikiName(l.scientific)}</i></td>
      <td class="num">${l.items.length}</td>
      <td>${fmtClock(l.first / 1e6)}</td>
      <td>${fmtClock(l.last / 1e6)}</td>
      <td class="num">${fmtPct(l.best)}</td>
      <td>${[...l.clients].map(c => esc(label(c))).join(", ")}</td>
      <td class="num">${muteButton(l.scientific, l.common, false)}</td>
    </tr>`).join("") || `<tr><td colspan="7" class="empty">${timeline.loaded ? "Nothing to list." : "Loading..."}</td></tr>`);

  const empty = !timeline.loaded ? "Loading..."
    : lanes.length ? ""
    : all.length ? "Nothing to show with this choice of species."
    : isToday ? "Nothing has been heard yet today." : "Nothing was heard on this day.";
  $("timeline-help").hidden = !!empty;
  $("timeline-zoom").hidden = !!empty;
  renderTimeline(lanes, empty);
}

function renderLatest(isToday) {
  const box = $("latest"), s = latest;
  box.hidden = !isToday || !s;
  if (box.hidden) return;
  const species = sampleSpecies(s);
  const newest = Math.max(...s.highlights.map(h => h.detection.time));
  setHtml("latest-when", ago(newest));
  const changed = setHtml("latest-body", `<div style="display:flex; flex-wrap:wrap; gap:10px 20px; align-items:center">
      <div style="flex: 1 1 320px; max-width: 560px">${clipHtml(s, false)}</div>
      <div style="flex: 1 1 14em">
        <div class="viewer-title">${species.map(x => `<span class="name">${esc(x.common)}</span>` +
          (x.unexpected ? ` <span class="tag">unexpected</span>` : "")).join(`<span class="muted">and</span>`)}</div>
        <p class="muted">${fmtClock(newest / 1e6, true)}${s.client ? `, heard by ${esc(label(s.client))}` : ""},
          confidence ${fmtPct(species[0].confidence)}</p>
        <div class="toolbar">
          ${s.audio ? `<button type="button" class="play" data-url="${esc(sampleUrl(s.client, s.from, s.to))}" data-label="Play">Play</button>` : ""}
          <a class="button" href="${href("recordings", [], { rec: s.highlights[0].detection.id })}">Open in Recordings</a>
        </div>
      </div></div>`);
  if (changed) { for (const v of $("latest-body").querySelectorAll(".clip[data-url]")) mountClip(v); markPlaying(); }
}

function renderTimeline(lanes, empty) {
  const el = $("timeline");
  if (!empty) el.style.height = (lanes.length * LANE_HEIGHT + 104) + "px";
  const chart = chartFor(el, empty);
  if (!chart) return;
  if (!chart.__wired) {
    chart.__wired = true;
    chart.on("click", "series", p => { location.hash = recordingsLink(p.data.entry.scientific, { from: day, to: day, rec: p.data.entry.id }); });
    chart.on("click", "yAxis", p => { location.hash = recordingsLink(p.value, { from: day, to: day }); });
  }
  const c = chartColors();
  const laneIndex = new Map(lanes.map((l, i) => [l.scientific, i]));
  const data = lanes.flatMap(l => l.items).map(d => ({
    value: [d.time / 1e6, Math.max(d.end, d.time) / 1e6, laneIndex.get(d.scientific)], entry: d,
  }));
  const dayStart = dayStartMs(day), dayEnd = dayEndMs(day), now = Date.now();
  const longest = Math.max(...lanes.map(l => l.common.length + (l.occurrence === "unexpected" ? 12 : 0)));
  const shorten = s => (s.length > 28 ? s.slice(0, 27) + "..." : s);
  chart.setOption({
    animation: false,
    textStyle: chartFont(),
    grid: { left: Math.min(280, 16 + Math.min(longest, 40) * 7), right: 24, top: 16, bottom: 64 },
    tooltip: {
      trigger: "item", confine: true, backgroundColor: c.surface, borderColor: c.axis, textStyle: { color: c.ink },
      formatter: p => {
        const d = p.data.entry;
        return `<b>${fmtClock(d.time / 1e6, true)}</b> &middot; confidence <b>${fmtPct(d.confidence)}</b><br>` +
          `${esc(d.common)} <i>${esc(d.scientific)}</i>` +
          (d.occurrence === "unexpected" ? ` <span style="color:${c.tag};font-weight:600">unexpected</span>` : "") +
          `<br>Heard by ${d.clients.map(label).map(esc).join(", ")}` + (d.cardinal ? `<br>Direction: ${esc(d.cardinal)}` : "") +
          `<br><span style="color:${c.ink2};font-size:12px">Click to open the recording</span>`;
      },
    },
    xAxis: {
      type: "time", min: dayStart, max: dayEnd,
      axisLabel: { color: c.ink2, formatter: "{HH}:{mm}", hideOverlap: true },
      axisLine: { lineStyle: { color: c.axis } }, axisTick: { lineStyle: { color: c.axis } },
      splitLine: { show: true, lineStyle: { color: c.grid } },
    },
    yAxis: {
      type: "category", inverse: true, data: lanes.map(l => l.scientific), triggerEvent: true,
      axisTick: { show: false }, axisLine: { lineStyle: { color: c.axis } },
      splitLine: { show: true, interval: 0, lineStyle: { color: c.grid } },
      axisLabel: {
        color: c.ink,
        // Plain text (not HTML): only ECharts' rich-text markers need removing.
        formatter: sci => {
          const l = lanes[laneIndex.get(sci)];
          return l ? shorten(l.common).replace(/[{}|]/g, "") + (l.occurrence === "unexpected" ? " {u|unexpected}" : "") : "";
        },
        rich: { u: { color: c.tag, fontWeight: "bold", fontSize: 11 } },
      },
    },
    dataZoom: [
      // The wheel scrolls the page; with Ctrl (or a pinch) it zooms the day.
      { type: "inside", xAxisIndex: 0, filterMode: "none", minValueSpan: 5 * 60 * 1000, zoomOnMouseWheel: "ctrl", moveOnMouseWheel: false },
      { type: "slider", xAxisIndex: 0, filterMode: "none", height: 18, bottom: 10, borderColor: c.axis, textStyle: { color: c.ink2 },
        labelFormatter: v => fmtClock(v) },
    ],
    series: [{
      type: "custom", encode: { x: [0, 1], y: 2 }, data, clip: true,
      renderItem: (params, api) => {
        const start = api.coord([api.value(0), api.value(2)]), end = api.coord([api.value(1), api.value(2)]);
        const laneH = api.size([0, 1])[1], h = laneH * 0.6, w = Math.max(3, end[0] - start[0]);
        const color = data[params.dataIndex].entry.occurrence === "unexpected" ? c.tickU : c.tick;
        return { type: "group", children: [
          // A generous invisible hit area, so a 3px mark is easy to point at and click.
          { type: "rect", shape: { x: start[0] - 6, y: start[1] - laneH / 2, width: w + 12, height: laneH }, style: { fill: "rgba(0,0,0,0)" } },
          { type: "rect", shape: { x: start[0], y: start[1] - h / 2, width: w, height: h, r: 1 }, style: { fill: color },
            emphasis: { style: { fill: color, stroke: c.ink, lineWidth: 1 } } },
        ] };
      },
      markLine: day === today() && now >= dayStart && now < dayEnd ? {
        silent: true, symbol: "none", animation: false, lineStyle: { color: c.ink2, type: "dashed", width: 1 },
        label: { formatter: "now", color: c.ink2, position: "start" }, data: [{ xAxis: now }],
      } : { data: [] },
    }],
  }, { replaceMerge: ["series", "yAxis"] });
}

/// Zooms the timeline to the part of the day with recordings (or back out to the whole day).
function fitTimeline(whole) {
  const chart = charts.get($("timeline")), times = timeline.detections.filter(d => !d.muted).map(d => d.time / 1e6);
  if (!chart || !times.length) return;
  const pad = Math.max(10 * 60e3, (Math.max(...times) - Math.min(...times)) * 0.1);
  chart.dispatchAction(whole ? { type: "dataZoom", start: 0, end: 100 }
    : { type: "dataZoom", startValue: Math.max(dayStartMs(day), Math.min(...times) - pad), endValue: Math.min(dayEndMs(day), Math.max(...times) + pad) });
}
$("timeline-fit").onclick = () => fitTimeline(false);
$("timeline-whole").onclick = () => fitTimeline(true);
$("day-prev").onclick = () => setDay(shiftDayKey(day, -1));
$("day-next").onclick = () => setDay(shiftDayKey(day, 1));
$("day-today").onclick = () => setDay(today());
$("day").onchange = e => setDay(e.target.value);
$("lane-filter").onchange = e => { dash.filter = e.target.value; dash.save(); renderToday(); };
$("lane-order").onchange = e => { dash.order = e.target.value; dash.save(); renderToday(); };

// ---- Recordings: a list to step through, and one recording in full ----

const REC_PAGE = 60;
const REC_FILTERS = ["q", "species", "client", "from", "to", "unexpected", "min_confidence", "muted"];
let recParams = new URLSearchParams();
let recKey = null;        // the filters the loaded rows answer
let recRows = [], recMore = false, recLoadedAt = 0;
let recSamples = [];
let recSel = null;        // key of the recording shown
let recWant = null;       // detection id asked for in the address (rec=)
let speciesSeen = [], speciesAt = 0;

function recFilterKey(params) { return REC_FILTERS.map(k => params.get(k) || "").join("|"); }
/// Turns the page's address parameters into the API's.
function recApiQuery(limit, before) {
  const q = new URLSearchParams();
  for (const k of ["q", "species", "client", "unexpected", "min_confidence"]) if (recParams.get(k)) q.set(k, recParams.get(k));
  if (recParams.get("from")) q.set("from", msToNs(dayStartMs(recParams.get("from"))));
  if (recParams.get("to")) q.set("to", msToNs(dayEndMs(recParams.get("to"))));
  // A species asked for by name is shown even if it is muted.
  if (!recParams.get("muted") && !recParams.get("species")) q.set("hide_muted", "true");
  if (before) q.set("before", before);
  q.set("limit", limit);
  return q;
}
function enterRecordings(params) {
  recParams = params;
  recWant = params.get("rec");
  $("f-q").value = params.get("q") || "";
  $("f-from").value = params.get("from") || "";
  $("f-to").value = params.get("to") || "";
  $("f-unexpected").value = params.get("unexpected") || "";
  $("f-conf").value = params.get("min_confidence") || "";
  $("f-muted").checked = !!params.get("muted");
  renderRecFilters();
  if (recFilterKey(params) !== recKey) { recRows = []; recSamples = []; recLoadedAt = 0; recSel = null; renderRecordings(); }
  else if (recWant) selectWanted();
}
function renderRecFilters() {
  const sel = $("f-client"), want = recParams.get("client") || "";
  setHtml(sel, `<option value="">any</option>` + App.clients.map(c => `<option value="${c.id}">${esc(label(c.id))}</option>`).join(""));
  sel.value = want;
  const sci = recParams.get("species"), chip = $("f-species");
  chip.hidden = !sci;
  if (sci) {
    const known = speciesSeen.find(s => s.scientific.toLowerCase() === sci.toLowerCase());
    setText(chip.querySelector("span"), "Species: " + (known ? known.common : sci));
  }
}
/// Writes the filter form into the address; the page reloads its list from there.
function applyRecFilters(dropSpecies) {
  const v = id => $(id).value.trim();
  const next = href("recordings", [], {
    q: v("f-q"), species: dropSpecies ? "" : recParams.get("species"), client: v("f-client"), from: v("f-from"), to: v("f-to"),
    unexpected: v("f-unexpected"), min_confidence: v("f-conf"), muted: $("f-muted").checked ? "1" : "",
  });
  if (location.hash !== next) location.hash = next;
}
async function refreshRecordings() {
  if (Date.now() - speciesAt > 60000) {
    speciesAt = Date.now();
    api("/species").then(s => {
      speciesSeen = s;
      setHtml("species-names", s.map(x => `<option value="${esc(x.common)}">${esc(x.scientific)}</option>`).join(""));
      renderRecFilters();
    }).catch(() => {});
  }
  const key = recFilterKey(recParams);
  if (key !== recKey || !recLoadedAt) {
    const rows = await api("/detections?" + recApiQuery(Math.min(1000, Math.max(REC_PAGE, recRows.length))));
    if (key !== recFilterKey(recParams)) return;
    recKey = key;
    recRows = rows;
    recMore = rows.length >= REC_PAGE;
    recLoadedAt = Date.now();
  } else if (Date.now() - recLoadedAt >= 5000) {
    // Pick up new recordings at the top while keeping what is loaded.
    const top = await api("/detections?" + recApiQuery(1));
    if (key !== recFilterKey(recParams)) return;
    recLoadedAt = Date.now();
    if ((top[0] && top[0].id) !== (recRows[0] && recRows[0].id)) { recLoadedAt = 0; return refreshRecordings(); }
  }
  renderRecordings();
}
async function loadOlderRecordings() {
  if (!recRows.length) return;
  const key = recKey;
  // Times above 2^53 lose precision in JavaScript; step back slightly and drop repeats by id.
  const rows = await api("/detections?" + recApiQuery(REC_PAGE, recRows[recRows.length - 1].time + 1000));
  if (key !== recKey) return;
  const seen = new Set(recRows.map(d => d.id));
  recRows = recRows.concat(rows.filter(d => !seen.has(d.id)));
  recMore = rows.length >= REC_PAGE;
  renderRecordings();
}
function selectWanted() {
  const s = recSamples.find(s => s.highlights.some(h => h.detection.id === recWant));
  if (s) { recSel = s.key; recWant = null; renderRecordings(true); }
}
function renderRecordings(scroll) {
  const loaded = recKey === recFilterKey(recParams) && recLoadedAt > 0 || recRows.length > 0;
  recSamples = groupSamples(recRows, { client: recParams.get("client") || null });
  if (recWant) {
    const s = recSamples.find(s => s.highlights.some(h => h.detection.id === recWant));
    if (s) { recSel = s.key; recWant = null; scroll = true; }
  }
  if (!recSamples.some(s => s.key === recSel)) recSel = recSamples[0]?.key ?? null;
  const filtered = REC_FILTERS.some(k => recParams.get(k));
  setText("rec-summary", !loaded ? "Loading..."
    : recRows.length ? `${plural(recRows.length, filtered ? "matching call" : "call")} in ${plural(recSamples.length, "recording")}` + (recMore ? ", and older ones" : "")
    : filtered ? "Nothing matches these filters." : "Nothing has been recorded yet.");
  $("rec-more-box").hidden = !recMore;

  // The list: one line per recording, under a heading per day.
  const days = new Map();
  for (const s of recSamples) {
    const k = dateKey(new Date(s.from / 1e6));
    if (!days.has(k)) days.set(k, []);
    days.get(k).push(s);
  }
  const html = [...days.entries()].map(([k, list]) => `<li class="group"><span>${fmtDay(dayStartMs(k))}</span><span>${list.length}</span></li>` +
    list.map(s => {
      const species = sampleSpecies(s), unexpected = species.some(x => x.unexpected);
      return `<li class="item${unexpected ? " unexpected" : ""}" data-key="${esc(s.key)}" role="option">
        <div class="line"><span class="mono">${fmtClock(s.from / 1e6, true)}</span>
          <span class="grow"><b>${species.map(x => esc(x.common)).join(", ")}</b></span>
          <span class="num">${fmtPct(species[0].confidence)}</span></div>
        <div class="sub"><span class="grow">${s.client ? esc(label(s.client)) : "no audio kept"}</span>
          ${unexpected ? `<span class="tag">unexpected</span>` : ""}<span>${((s.to - s.from) / 1e9).toFixed(1)} s</span></div></li>`;
    }).join("")).join("");
  setHtml("rec-list", html || `<li class="empty">${loaded ? "No recordings." : "Loading..."}</li>`);
  for (const li of $("rec-list").querySelectorAll(".item")) {
    const current = li.dataset.key === recSel;
    li.classList.toggle("current", current);
    li.setAttribute("aria-selected", current);
    if (current && scroll) li.scrollIntoView({ block: "nearest" });
  }
  renderViewer();
}

function renderViewer() {
  const s = recSamples.find(s => s.key === recSel);
  if (!s) { setHtml("viewer", `<p class="empty">${recRows.length ? "Choose a recording from the list." : "No recording to show."}</p>`); return; }
  const len = Math.max(1, s.to - s.from), species = sampleSpecies(s), i = recSamples.indexOf(s);
  const located = s.highlights.some(h => h.detection.location);
  const url = s.audio ? sampleUrl(s.client, s.from, s.to) : "";
  const rows = s.highlights.map(h => {
    const d = h.detection, loc = d.location;
    const heard = d.calls.map(c => {
      const who = esc(label(c.client_id));
      if (!d.clips.includes(c.client_id)) return who;
      return `<button type="button" class="play small" data-url="${clipUrl(d.id, c.client_id)}" data-label="${who}" title="Play this call as ${who} heard it">${who}</button>`;
    }).join(" ");
    return `<tr data-n="${h.n}" class="${d.unexpected ? "unexpected" : ""}">
      <td><span class="hl-badge" style="--c:var(--hl-${h.colour})">${h.n}</span></td>
      <td><b>${esc(d.species.common)}</b>${d.unexpected ? ` <span class="tag" title="Not expected at this site">unexpected</span>` : ""}
        <br><i class="muted">${wikiName(d.species.scientific)}</i>
        <div class="toolbar" style="margin-top: 4px; gap: 6px"><a class="button small" href="${recordingsLink(d.species.scientific)}"
          title="Every recording of this species">All its recordings</a>${muteButton(d.species.scientific, d.species.common, false)}</div></td>
      <td class="num">${fmtPct(d.confidence)}</td>
      <td class="num nowrap" title="${fmtFull(h.start)}">${((h.start - s.from) / 1e9).toFixed(2)} to ${((h.end - s.from) / 1e9).toFixed(2)} s
        <br><span class="muted">${fmtClock(h.start / 1e6, false, true)}</span></td>
      <td>${heard}</td>
      ${located ? `<td class="nowrap">${loc ? `${esc(loc.cardinal)} (${loc.bearing_deg.toFixed(0)}&deg;)` : "-"}</td>` : ""}
    </tr>`;
  }).join("");
  const html = `
    <div class="viewer-title">${species.map(x => `<span><span class="name">${esc(x.common)}</span> <i>${wikiName(x.scientific)}</i>` +
      (x.unexpected ? ` <span class="tag">unexpected</span>` : "") + `</span>`).join("")}</div>
    <p class="muted">${fmtDay(s.from / 1e6)}, ${fmtClock(s.from / 1e6, true)} &middot; ${s.client ? "heard by " + esc(label(s.client)) : "no audio kept"}
      &middot; ${(len / 1e9).toFixed(2)} s &middot; ${plural(s.highlights.length, "identified call")}</p>
    <div class="toolbar" style="margin: 8px 0">
      ${url ? `<button type="button" class="play primary" data-url="${esc(url)}" data-label="Play">Play</button>` : ""}
      <span class="segmented">
        <button type="button" data-step="-1" ${i <= 0 ? "disabled" : ""} title="The newer recording (Up)">Newer</button>
        <button type="button" data-step="1" ${i >= recSamples.length - 1 ? "disabled" : ""} title="The older recording (Down)">Older</button>
      </span>
      <span class="spacer"></span>
      <span class="segmented" role="group" aria-label="Picture">
        <button type="button" data-view="spectrogram" class="${clipPrefs.view === "spectrogram" ? "on" : ""}">Spectrogram</button>
        <button type="button" data-view="waveform" class="${clipPrefs.view === "waveform" ? "on" : ""}">Waveform</button>
      </span>
      <label title="Play every recording at the same peak volume, so quiet calls are easier to hear">
        <input type="checkbox" id="rec-normalize" ${clipPrefs.normalize ? "checked" : ""}> Even out volume</label>
    </div>
    <div class="clip-frame">
      <div class="freq-axis">${clipPrefs.view === "spectrogram" && s.audio ? "<span>12 kHz</span><span>8</span><span>4</span><span>0</span>" : ""}</div>
      ${clipHtml(s, true)}
      <div class="time-axis">${timeAxis(len)}</div>
    </div>
    <p class="hint">Each numbered band is one identified call; the table has a row per band. Click the picture to play the whole
      recording, or a microphone's name to hear just that call as it heard it.</p>
    <div class="scroll-x" style="margin-top: 10px"><table class="data" id="viewer-calls">
      <thead><tr><th></th><th>Species</th><th class="num">Confidence</th><th class="num" title="Where the call is in the recording, and the clock time it started">When</th><th>Heard by</th>
        ${located ? `<th title="A first estimate: positioning is still being built">Direction</th>` : ""}</tr></thead>
      <tbody>${rows}</tbody></table></div>`;
  if (setHtml("viewer", html)) {
    for (const v of $("viewer").querySelectorAll(".clip[data-url]")) mountClip(v);
    markPlaying();
  }
}
/// Shows the recording `step` places down the list (negative: up).
function stepRecording(step) {
  const i = recSamples.findIndex(s => s.key === recSel), next = recSamples[i + step];
  if (!next) return;
  chooseRecording(next.key);
}
function chooseRecording(key) {
  recSel = key;
  const s = recSamples.find(s => s.key === key);
  // Keep the address pointing at what is shown, without adding to the history.
  if (s) history.replaceState(null, "", href("recordings", [], { ...Object.fromEntries(REC_FILTERS.map(k => [k, recParams.get(k)])), rec: s.highlights[0].detection.id }));
  renderRecordings(true);
}

$("rec-list").addEventListener("click", e => { const li = e.target.closest(".item"); if (li) chooseRecording(li.dataset.key); });
$("rec-more").onclick = loadOlderRecordings;
$("viewer").addEventListener("click", e => {
  const b = e.target.closest("button");
  if (!b) return;
  if (b.dataset.step) stepRecording(Number(b.dataset.step));
  if (b.dataset.view) { clipPrefs.view = b.dataset.view; clipPrefs.save(); renderViewer(); }
});
$("viewer").addEventListener("change", e => { if (e.target.id === "rec-normalize") setNormalize(e.target.checked); });
// Pointing at a row of the table picks out its band on the picture.
for (const type of ["mouseover", "mouseout"]) {
  $("viewer").addEventListener(type, e => {
    const row = e.target.closest?.("#viewer-calls tbody tr");
    if (row) for (const x of $("viewer").querySelectorAll(`[data-n="${row.dataset.n}"]`)) x.classList.toggle("active", type === "mouseover");
  });
}
document.addEventListener("keydown", e => {
  if (route().page !== "recordings" || e.ctrlKey || e.metaKey || e.altKey) return;
  if (e.target.closest("input, select, textarea, button, a, dialog")) return;
  if (e.key === "ArrowDown" || e.key === "j") { stepRecording(1); e.preventDefault(); }
  else if (e.key === "ArrowUp" || e.key === "k") { stepRecording(-1); e.preventDefault(); }
  else if (e.key === " ") {
    const s = recSamples.find(s => s.key === recSel);
    if (s && s.audio) playClip(sampleUrl(s.client, s.from, s.to));
    e.preventDefault();
  }
});
// Filters apply as soon as they change; typing in the search box waits for a pause.
$("rec-filters").addEventListener("change", e => { if (e.target.id !== "f-q") applyRecFilters(); });
let searchTimer = null;
$("f-q").addEventListener("input", () => { clearTimeout(searchTimer); searchTimer = setTimeout(() => applyRecFilters(), 400); });
$("f-species-clear").onclick = () => applyRecFilters(true);
$("f-clear").onclick = () => { location.hash = "#/recordings"; };

// ---- Species: everything heard here ----

const spPrefs = prefs("bsp.species", { order: "recent" });
let speciesList = null, speciesListAt = 0;

async function refreshSpecies() {
  if (!speciesList || Date.now() - speciesListAt > 10000 || speciesAt === 0) {
    speciesList = await api("/species");
    speciesListAt = Date.now();
    speciesAt = speciesAt || 1;
  }
  renderSpecies();
}
function renderSpecies() {
  $("sp-order").value = spPrefs.order;
  const all = speciesList || [], q = $("sp-q").value.trim().toLowerCase();
  const sort = { recent: (a, b) => b.last_seen - a.last_seen, count: (a, b) => b.count - a.count, name: (a, b) => a.common.localeCompare(b.common) }[spPrefs.order];
  const match = s => !q || s.common.toLowerCase().includes(q) || s.scientific.toLowerCase().includes(q);
  const row = s => `<tr class="${s.occurrence === "unexpected" && !s.muted ? "unexpected" : ""}">
      <td><a href="${recordingsLink(s.scientific)}"><b>${esc(s.common)}</b></a><br><i class="muted">${wikiName(s.scientific)}</i></td>
      <td>${OCCURRENCE[s.occurrence] || ""}</td>
      <td class="num">${s.count}</td>
      <td class="nowrap">${ago(s.last_seen)}</td>
      <td class="num">${muteButton(s.scientific, s.common, s.muted)}</td></tr>`;
  const shown = all.filter(s => !s.muted), muted = all.filter(s => s.muted);
  const birds = shown.filter(s => s.occurrence !== "unknown").length;
  setText("species-summary", speciesList ? `${plural(shown.length, "species and sounds", "species and sounds")} heard here` +
    (shown.length !== birds ? `, ${birds} with range data` : "") + (muted.length ? `; ${muted.length} muted` : "") : "Loading...");
  setHtml(document.querySelector("#species-table tbody"), shown.filter(match).sort(sort).map(row).join("") ||
    `<tr><td colspan="5" class="empty">${speciesList ? (q ? "No species matches." : "Nothing has been heard yet.") : "Loading..."}</td></tr>`);
  $("muted-panel").hidden = !muted.length;
  setHtml(document.querySelector("#muted-table tbody"), muted.sort(sort).map(row).join(""));
}
$("sp-q").addEventListener("input", renderSpecies);
$("sp-order").onchange = e => { spPrefs.order = e.target.value; spPrefs.save(); renderSpecies(); };

// ---- Map: the microphones' positions (positioning itself is still to come) ----

function renderMap() {
  const mics = App.clients.filter(c => c.position).map(c => ({ label: label(c.id), x: c.position.x, y: c.position.y, z: c.position.z, cls: c.active ? "" : "inactive" }));
  renderMicMap($("mic-map"), mics);
  const n = v => v == null ? "-" : v.toFixed(1);
  setHtml(document.querySelector("#map-mics tbody"), App.clients.map(c => {
    const st = deviceState(c), p = c.position;
    return `<tr class="${c.active ? "" : "off"}"><td class="nowrap">${esc(label(c.id))}</td>
      <td class="nowrap"><span class="dot ${st.level}"></span> ${esc(st.text)}</td>
      <td class="num">${n(p?.x)}</td><td class="num">${n(p?.y)}</td><td class="num">${n(p?.z)}</td>
      <td class="num nowrap"><a href="/system#/devices/${c.id}/setup">${p ? "Change" : "Set position"}</a></td></tr>`;
  }).join("") || `<tr><td colspan="6" class="empty">No microphone has connected yet.</td></tr>`);
}

// ---- start ----

function renderFoot() {
  const s = App.status;
  if (s) setText("foot-server", `bsp-server ${s.version} (${s.build}), identifying with ${s.identifier}`);
}
start({
  pages: ["today", "recordings", "species", "map"],
  onRoute(r) {
    if (r.page === "today") {
      const asked = r.params.get("day");
      if (asked && /^\d{4}-\d{2}-\d{2}$/.test(asked) ? asked !== day : !followToday) showDay(asked || today());
      else { if (day === today()) timelineAt = 0; renderToday(); }
    }
    if (r.page === "recordings") enterRecordings(r.params);
    if (r.page === "species") renderSpecies();
    if (r.page !== "recordings") stopPlayback();
  },
  async onTick(r) {
    renderFoot();
    if (r.page === "today") await refreshToday();
    if (r.page === "recordings") await refreshRecordings();
    if (r.page === "species") await refreshSpecies();
    if (r.page === "map") renderMap();
  },
  onClients() {
    const r = route();
    if (r.page === "recordings") { renderRecFilters(); renderRecordings(); }
    if (r.page === "today") renderToday();
  },
});
window.addEventListener("themechange", () => { if (route().page === "today") renderToday(); });
