// What both workspaces of the web UI (Birds: index.html, System: system.html) are built on:
// talking to the server, the page frame (top bar, pages by #/hash, polling), small widgets
// (messages, questions, relative times), recordings (spectrogram, waveform, playback), the
// microphone map and chart colours. Formatting and naming are in lib.js.
"use strict";

const API = "/api/v1/control";
const POLL_MS = 2000;
const $ = id => document.getElementById(id);

/// What every page may read: the server's clients and status as last fetched.
const App = {
  clients: [],
  labels: new Map(),    // client id -> name shown (see clientLabels in lib.js)
  status: null,         // GET /status
  positioning: null,    // GET /positioning-request
  online: true,         // false while the server cannot be reached
  pages: [],            // this workspace's pages; the first is the default
  onRoute: null,        // (route) => void: a page was opened
  onTick: null,         // async (route) => void: time to refresh what is on screen
  onClients: null,      // () => void: the client list or its names changed
};

// ---- the server ----

class ApiError extends Error {
  constructor(status, reason) { super(reason); this.status = status; }
}
/// Calls the control API; the reply's JSON (null for 204). Errors carry the server's reason.
async function api(path, opts) {
  let r;
  try { r = await fetch(API + path, opts); } catch (e) { throw new ApiError(0, "the server cannot be reached"); }
  if (!r.ok) {
    const text = await r.text();
    let reason = text;
    try { reason = JSON.parse(text).error || text; } catch (e) { /* plain text */ }
    throw new ApiError(r.status, reason || `${r.status} ${r.statusText}`);
  }
  return r.status === 204 ? null : r.json();
}
function apiSend(method, path, body) {
  return api(path, { method, headers: { "content-type": "application/json" }, body: body === undefined ? undefined : JSON.stringify(body) });
}

function label(id) { return App.labels.get(id) || (id ? String(id).slice(0, 8) : "server"); }
function client(id) { return App.clients.find(c => c.id === id); }
function cssVar(name) { return getComputedStyle(document.documentElement).getPropertyValue(name).trim(); }

// ---- small DOM helpers ----

/// Sets an element's HTML only when it differs, so what the user is pointing at, has open or is
/// typing into is left alone between refreshes. True if it changed.
function setHtml(el, html) {
  if (typeof el === "string") el = $(el);
  if (!el || el.__html === html) return false;
  el.innerHTML = html;
  el.__html = html;
  return true;
}
function setText(el, text) {
  if (typeof el === "string") el = $(el);
  if (el && el.textContent !== text) el.textContent = text;
}
/// "5 min ago", kept current by a ticker without redrawing what it sits in.
function ago(ns) {
  return ns ? `<time class="ago" data-ns="${ns}" title="${esc(fmtFull(ns))}">${esc(fmtAgo(ns))}</time>` : "never";
}
setInterval(() => {
  for (const t of document.querySelectorAll("time.ago")) setText(t, fmtAgo(Number(t.dataset.ns)));
}, 1000);
/// A small (i) whose tooltip carries an explanation.
function info(text) { return `<span class="info" tabindex="0" title="${esc(text)}" aria-label="${esc(text)}">i</span>`; }
function tile(labelHtml, valueHtml, subHtml, cls, extra) {
  return `<div class="tile ${extra || ""}"><div class="label">${labelHtml}</div><div class="value ${cls || ""}">${valueHtml}</div>` +
    (subHtml ? `<div class="sub">${subHtml}</div>` : "") + `</div>`;
}
/// A species' scientific name, linked (unobtrusively) to Wikipedia. "Go" jumps straight to a
/// matching article or redirect, and falls back to search results for names without one.
function wikiUrl(scientific) { return "https://en.wikipedia.org/wiki/Special:Search?go=Go&search=" + encodeURIComponent(scientific); }
function wikiName(scientific) {
  return `<a class="quiet" href="${esc(wikiUrl(scientific))}" target="_blank" rel="noopener noreferrer" title="Open on Wikipedia">${esc(scientific)}</a>`;
}
function prefs(key, defaults) {
  const p = { ...defaults };
  try { Object.assign(p, JSON.parse(localStorage.getItem(key) || "{}")); } catch (e) { /* not remembered */ }
  p.save = () => { try { const { save, ...rest } = p; localStorage.setItem(key, JSON.stringify(rest)); } catch (e) { /* not remembered */ } };
  return p;
}

// ---- messages and questions ----

/// A message in the corner that goes away by itself (errors stay until dismissed).
function toast(text, kind) {
  let box = document.querySelector(".toasts");
  if (!box) { box = document.createElement("div"); box.className = "toasts"; box.setAttribute("role", "status"); document.body.append(box); }
  const el = document.createElement("div");
  el.className = "toast " + (kind || "");
  el.innerHTML = `<span class="grow">${esc(text)}</span><button class="small" type="button">Dismiss</button>`;
  el.querySelector("button").onclick = () => el.remove();
  box.append(el);
  if (kind !== "bad") setTimeout(() => el.remove(), 5000);
}
/// Asks before something that cannot be taken back. Resolves true if confirmed.
function confirmDialog({ title, body, action, danger }) {
  return new Promise(resolve => {
    const d = document.createElement("dialog");
    d.innerHTML = `<h2>${esc(title)}</h2><div class="body">${body}</div><div class="actions">` +
      `<button type="button" value="no">Cancel</button><button type="button" value="yes" class="${danger ? "danger" : "primary"}">${esc(action)}</button></div>`;
    document.body.append(d);
    let answer = false;
    for (const b of d.querySelectorAll("button")) b.onclick = () => { answer = b.value === "yes"; d.close(); };
    d.addEventListener("close", () => { d.remove(); resolve(answer); });
    d.showModal();
    d.querySelector('button[value="no"]').focus();
  });
}

// ---- pages: #/page/more?param=value ----

function route() {
  const [path, query] = location.hash.replace(/^#\/?/, "").split("?");
  const parts = path.split("/").filter(Boolean).map(decodeURIComponent);
  const page = App.pages.includes(parts[0]) ? parts[0] : App.pages[0];
  return { page, parts: parts.slice(1), params: new URLSearchParams(query || "") };
}
/// "#/page/more?a=b" from its pieces; empty values are left out.
function href(page, parts, params) {
  const q = new URLSearchParams();
  for (const [k, v] of Object.entries(params || {})) if (v !== "" && v != null && v !== false) q.set(k, v);
  const path = [page, ...(parts || [])].filter(p => p != null && p !== "").map(encodeURIComponent).join("/");
  return "#/" + path + (q.toString() ? "?" + q : "");
}
function showPage() {
  const r = route();
  for (const p of App.pages) { const el = $("page-" + p); if (el) el.hidden = p !== r.page; }
  for (const a of document.querySelectorAll(".topbar nav a")) {
    const current = a.dataset.page === r.page;
    a.classList.toggle("current", current);
    if (current) a.setAttribute("aria-current", "page"); else a.removeAttribute("aria-current");
  }
  const name = document.querySelector(`.topbar nav a[data-page="${r.page}"]`)?.textContent || "";
  document.title = `${name} - ${document.body.dataset.title || "Bird Positioning System"}`;
  if (App.onRoute) App.onRoute(r);
  refresh();
}

// ---- polling ----

let refreshing = false, refreshAgain = false, namesKey = "";
async function refresh() {
  if (refreshing) { refreshAgain = true; return; }
  refreshing = true;
  try {
    const [clients, status, positioning] = await Promise.all([api("/clients"), api("/status"), api("/positioning-request")]);
    App.clients = clients;
    App.status = status;
    App.positioning = positioning;
    App.labels = clientLabels(clients);
    setOnline(true);
    renderStatus();
    const key = clients.map(c => c.id + App.labels.get(c.id)).join();
    if (key !== namesKey) { namesKey = key; if (App.onClients) App.onClients(); }
    if (App.onTick) await App.onTick(route());
  } catch (e) {
    if (e instanceof ApiError && e.status === 0) setOnline(false);
    else { console.error(e); showProblem(e.message); }
  } finally {
    refreshing = false;
    if (refreshAgain) { refreshAgain = false; refresh(); }
  }
}
function setOnline(online) {
  App.online = online;
  const b = $("offline");
  if (b) b.hidden = online;
  if (online) showProblem("");
  else renderStatus();
}
function showProblem(text) {
  const b = $("problem");
  if (!b) return;
  b.hidden = !text;
  setText(b.querySelector("span"), text);
}
/// The top bar's right-hand side: in one line, whether the system is listening.
function renderStatus() {
  const el = $("status");
  if (!el) return;
  if (!App.online) { setHtml(el, `<span class="dot bad"></span> server not reachable`); return; }
  const s = App.status;
  if (!s) return;
  const listening = App.clients.filter(c => c.active && c.synced).length, active = App.clients.filter(c => c.active).length;
  const dot = listening ? "ok" : active ? "bad" : "off";
  const text = !active ? "no microphone is listening"
    : listening === active ? `${plural(listening, "microphone")} listening`
    : `${listening} of ${plural(active, "microphone")} synced`;
  const title = `bsp-server ${s.version} (${s.build}), client API ${s.api_version}, identifying with ${s.identifier}. ` +
    `${s.clients_total} known devices, ${active} sending heartbeats, ${listening} with a synced clock.`;
  setHtml(el, `<a href="/system#/devices" title="${esc(title)}"><span class="dot ${dot}"></span> ${esc(text)}</a>`);
}

/// Starts a workspace: `pages` are its page names (sections with id "page-<name>").
function start({ pages, onRoute, onTick, onClients }) {
  Object.assign(App, { pages, onRoute, onTick, onClients });
  initTheme();
  window.addEventListener("hashchange", showPage);
  showPage();
  setInterval(refresh, POLL_MS);
  document.addEventListener("visibilitychange", () => { if (!document.hidden) refresh(); });
}

// ---- theme: the system's, unless one is chosen ----

function initTheme() {
  const sel = $("theme");
  let saved = "";
  try { saved = localStorage.getItem("bps.theme") || ""; } catch (e) { /* ignore */ }
  const apply = v => {
    if (v) document.documentElement.dataset.theme = v; else delete document.documentElement.dataset.theme;
    window.dispatchEvent(new Event("themechange"));
  };
  apply(saved);
  if (!sel) return;
  sel.value = saved;
  sel.onchange = () => { try { localStorage.setItem("bps.theme", sel.value); } catch (e) { /* ignore */ } apply(sel.value); };
}
window.matchMedia("(prefers-color-scheme: dark)").addEventListener("change", () => window.dispatchEvent(new Event("themechange")));
function chartColors() {
  return { surface: cssVar("--surface"), ink: cssVar("--ink"), ink2: cssVar("--ink-2"), muted: cssVar("--muted"), grid: cssVar("--grid"),
           axis: cssVar("--axis"), s1: cssVar("--tick"), tick: cssVar("--tick"), tickU: cssVar("--tick-unexpected"), tag: cssVar("--tag"), ok: cssVar("--ok") };
}
function chartFont() { return { fontFamily: getComputedStyle(document.body).fontFamily }; }
/// The chart in `el` (made on first use), or null with a message in its place when there is
/// nothing to draw (`empty`), the page is hidden, or the charting library did not load.
const charts = new Map();
function chartFor(el, empty) {
  if (typeof el === "string") el = $(el);
  let msg = el.nextElementSibling;
  if (!msg || !msg.classList.contains("chart-empty")) {
    msg = document.createElement("p");
    msg.className = "empty chart-empty";
    el.after(msg);
  }
  if (typeof echarts === "undefined") empty = "The charting library failed to load.";
  el.hidden = !!empty;
  msg.hidden = !empty;
  setText(msg, empty || "");
  if (empty || el.offsetWidth === 0) return null;
  let chart = charts.get(el);
  if (!chart) { chart = echarts.init(el); charts.set(el, chart); }
  chart.resize();
  return chart;
}
window.addEventListener("resize", () => { for (const c of charts.values()) c.resize(); });

// ---- recordings: spectrogram or waveform, and playback (one at a time) ----

function clipUrl(detectionId, clientId) { return `${API}/detections/${detectionId}/audio/${clientId}`; }
/// One microphone's recording of `from..to` (ns): every stored clip in that span, pieced together.
function sampleUrl(clientId, from, to, what = "audio") {
  // Whole nanoseconds; JavaScript would print large ones in exponent form.
  const ns = t => BigInt(Math.round(t)).toString();
  return `${API}/samples/${clientId}/${what}?from=${ns(from)}&to=${ns(to)}`;
}

const clipPrefs = prefs("bsp.clips", { view: "spectrogram", normalize: true });

/// Fetches and decodes a stored clip: { buffer, call: [startS, endS] or null }.
/// Recordings are FLAC, but Safari on iOS cannot decode FLAC with Web Audio; it plays 16-bit
/// PCM WAV, which the server sends with format=wav. Once a FLAC fails to decode, later
/// recordings are fetched as WAV straight away.
let decodeCtx = null, clipsAsWav = false;
class DecodeError extends Error {}
async function fetchClip(url) {
  if (!clipsAsWav) {
    try { return await fetchAndDecode(url); } catch (e) {
      if (!(e instanceof DecodeError)) throw e;
      clipsAsWav = true;
    }
  }
  return fetchAndDecode(url + (url.includes("?") ? "&" : "?") + "format=wav");
}
async function fetchAndDecode(url) {
  const r = await fetch(url);
  if (!r.ok) throw new Error(`${r.status} ${r.statusText}`);
  const range = (r.headers.get("x-call-range") || "").split(",").map(parseFloat);
  // An offline context decodes without needing a user gesture first.
  decodeCtx = decodeCtx || new OfflineAudioContext(1, 1, 48000);
  const data = await r.arrayBuffer();
  let buffer;
  try { buffer = await decodeCtx.decodeAudioData(data); } catch (e) {
    // Safari has rejected with null instead of an EncodingError.
    throw new DecodeError(`cannot decode ${r.headers.get("content-type") || "audio"}: ${e ? e.message || e.name : "unknown error"}`);
  }
  return { buffer, call: range.length === 2 && range.every(Number.isFinite) ? range : null };
}

/// In-place radix-2 FFT of `re`/`im` (length a power of two).
const fftTwiddles = new Map();
function fft(re, im) {
  const n = re.length;
  let tw = fftTwiddles.get(n);
  if (!tw) {
    tw = { cos: new Float32Array(n / 2), sin: new Float32Array(n / 2) };
    for (let k = 0; k < n / 2; k++) { tw.cos[k] = Math.cos(-2 * Math.PI * k / n); tw.sin[k] = Math.sin(-2 * Math.PI * k / n); }
    fftTwiddles.set(n, tw);
  }
  for (let i = 1, j = 0; i < n; i++) {
    let bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) {
      let t = re[i]; re[i] = re[j]; re[j] = t;
      t = im[i]; im[i] = im[j]; im[j] = t;
    }
  }
  for (let len = 2; len <= n; len <<= 1) {
    const half = len >> 1, step = n / len;
    for (let i = 0; i < n; i += len) {
      for (let k = 0; k < half; k++) {
        const a = i + k, b = a + half, wr = tw.cos[k * step], wi = tw.sin[k * step];
        const tr = re[b] * wr - im[b] * wi, ti = re[b] * wi + im[b] * wr;
        re[b] = re[a] - tr; im[b] = im[a] - ti;
        re[a] += tr; im[a] += ti;
      }
    }
  }
}

/// Spectrograms are 5:1, drawn at this size whatever size they are shown at.
const SPECTRO = { w: 1000, h: 200, fftSize: 512, maxHz: 12000, gamma: 1.2, floorDb: 6, minRangeDb: 20 };
const SPECTRO_WINDOW = Float32Array.from({ length: SPECTRO.fftSize }, (_, i) => 0.5 - 0.5 * Math.cos(2 * Math.PI * i / (SPECTRO.fftSize - 1)));

/// Spectrogram of a whole clip as colour-scale indices (0..255), SPECTRO.w x SPECTRO.h, low
/// frequencies at the bottom. Each frequency's typical level (its median over the clip) plus a
/// few dB is the zero point, which flattens steady noise such as wind or traffic so calls stand
/// out; the top of the scale is a high percentile, so one loud click doesn't wash out the rest.
function spectrogramLevels(buffer) {
  const pcm = buffer.getChannelData(0), sr = buffer.sampleRate;
  const { w, h, fftSize: n } = SPECTRO;
  const bins = Math.min(n / 2, Math.ceil(SPECTRO.maxHz * n / sr));
  const db = new Float32Array(w * bins);
  const re = new Float32Array(n), im = new Float32Array(n);
  for (let x = 0; x < w; x++) {
    const start = Math.round((x + 0.5) / w * pcm.length) - n / 2;
    for (let i = 0; i < n; i++) {
      const j = start + i;
      re[i] = j >= 0 && j < pcm.length ? pcm[j] * SPECTRO_WINDOW[i] : 0;
      im[i] = 0;
    }
    fft(re, im);
    for (let k = 0; k < bins; k++) db[x * bins + k] = 10 * Math.log10(re[k] * re[k] + im[k] * im[k] + 1e-12);
  }
  const col = new Float32Array(w);
  for (let k = 0; k < bins; k++) {
    for (let x = 0; x < w; x++) col[x] = db[x * bins + k];
    const zero = col.sort()[w >> 1] + SPECTRO.floorDb;
    for (let x = 0; x < w; x++) db[x * bins + k] -= zero;
  }
  const top = Math.max(SPECTRO.minRangeDb, db.slice().sort()[Math.floor(db.length * 0.998)]);
  const out = new Uint8Array(w * h);
  const binsShown = SPECTRO.maxHz * n / sr;
  for (let y = 0; y < h; y++) {
    const k = Math.floor((1 - (y + 0.5) / h) * binsShown);
    if (k >= bins) continue; // above the recording's Nyquist frequency
    for (let x = 0; x < w; x++) {
      const t = Math.max(0, Math.min(1, db[x * bins + k] / top));
      out[y * w + x] = Math.round(Math.pow(t, SPECTRO.gamma) * 255);
    }
  }
  return out;
}

/// 256-entry colour lookup: one hue, from the surface (quiet) to strong blue (loud); dark mode
/// runs dark to light against its dark surface.
let spectroLut = null, spectroLutFor = "";
function spectrogramLut() {
  const css = getComputedStyle(document.documentElement);
  const dark = css.colorScheme === "dark";
  const surface = css.getPropertyValue("--surface").trim() || (dark ? "#1a1a19" : "#fcfcfb");
  const key = surface + dark;
  if (key === spectroLutFor) return spectroLut;
  const ramp = dark
    ? [surface, "#104281", "#1c5cab", "#3987e5", "#6da7ec", "#b7d3f6", "#ffffff"]
    : [surface, "#cde2fb", "#86b6ef", "#3987e5", "#1c5cab", "#104281", "#0d366b"];
  const rgb = h => [1, 3, 5].map(i => parseInt(h.slice(i, i + 2), 16));
  const stops = ramp.map(rgb);
  spectroLut = new Uint8ClampedArray(256 * 3);
  for (let i = 0; i < 256; i++) {
    const t = i / 255 * (stops.length - 1), a = Math.floor(t), b = Math.min(a + 1, stops.length - 1), f = t - a;
    for (let c = 0; c < 3; c++) spectroLut[i * 3 + c] = stops[a][c] + (stops[b][c] - stops[a][c]) * f;
  }
  spectroLutFor = key;
  return spectroLut;
}

/// Computed spectrograms by recording URL, kept so redrawing or switching theme doesn't
/// refetch; oldest dropped first.
const spectroCache = new Map();
function cacheSpectrogram(key, clip) {
  const entry = { levels: spectrogramLevels(clip.buffer), call: clip.call, duration: clip.buffer.duration };
  spectroCache.set(key, entry);
  if (spectroCache.size > 100) spectroCache.delete(spectroCache.keys().next().value);
  return entry;
}
function paintSpectrogram(canvas, { levels, call, duration }, markCall) {
  const { w, h } = SPECTRO, lut = spectrogramLut();
  const g = canvas.getContext("2d"), img = g.createImageData(w, h), px = img.data;
  for (let i = 0; i < w * h; i++) {
    const v = levels[i] * 3;
    px[i * 4] = lut[v]; px[i * 4 + 1] = lut[v + 1]; px[i * 4 + 2] = lut[v + 2]; px[i * 4 + 3] = 255;
  }
  g.putImageData(img, 0, 0);
  if (markCall && call && duration > 0) {
    // The identified call: a bar along the bottom edge.
    const x0 = Math.max(0, call[0] / duration * w), x1 = Math.min(w, call[1] / duration * w);
    g.fillStyle = cssVar("--call");
    g.fillRect(x0, h - 8, Math.max(3, x1 - x0), 8);
  }
}
function waveformImg(url) {
  const img = document.createElement("img");
  img.alt = "waveform"; img.loading = "lazy";
  img.src = url;
  return img;
}
/// Fills a .clip element (data-url: the audio, data-wave: the server-drawn waveform) with its
/// picture in the chosen style; spectrograms are computed when the element comes into view.
function mountClip(view) {
  view.querySelector(":scope > canvas, :scope > img")?.remove();
  if (clipPrefs.view === "waveform") { view.prepend(waveformImg(view.dataset.wave)); return; }
  const canvas = document.createElement("canvas");
  canvas.width = SPECTRO.w; canvas.height = SPECTRO.h;
  canvas.setAttribute("role", "img");
  canvas.setAttribute("aria-label", "Spectrogram of the recording, 0 to 12 kHz");
  view.prepend(canvas);
  const cached = spectroCache.get(view.dataset.url);
  if (cached) paintSpectrogram(canvas, cached, "mark" in view.dataset);
  else spectroObserver.observe(canvas);
}
async function drawSpectrogram(canvas) {
  const view = canvas.parentElement;
  if (!view) return;
  const key = view.dataset.url;
  try {
    const entry = spectroCache.get(key) || cacheSpectrogram(key, await fetchClip(key));
    if (canvas.isConnected) paintSpectrogram(canvas, entry, "mark" in view.dataset);
  } catch (e) {
    // Fall back to the server-drawn waveform.
    if (canvas.isConnected) canvas.replaceWith(waveformImg(view.dataset.wave));
  }
}
const spectroObserver = new IntersectionObserver(entries => {
  for (const e of entries) {
    if (!e.isIntersecting) continue;
    spectroObserver.unobserve(e.target);
    drawSpectrogram(e.target);
  }
}, { rootMargin: "300px" });
window.addEventListener("themechange", () => {
  for (const canvas of document.querySelectorAll(".clip > canvas")) {
    const entry = spectroCache.get(canvas.parentElement.dataset.url);
    if (entry) paintSpectrogram(canvas, entry, "mark" in canvas.parentElement.dataset);
  }
});

// Playback goes through Web Audio so quiet recordings can be brought up to a common peak level.
const NORMALIZE_PEAK = 0.89;      // -1 dBFS
const NORMALIZE_MAX_GAIN = 31.6;  // +30 dB, so a near-silent clip isn't turned into loud hiss
let audioCtx = null;
const playback = { url: null, source: null, gain: null, normGain: 1, startedAt: 0, duration: 0, frame: 0 };

function normalizeGain(buffer) {
  let peak = 0;
  for (let c = 0; c < buffer.numberOfChannels; c++) {
    for (const x of buffer.getChannelData(c)) if (x > peak) peak = x; else if (-x > peak) peak = -x;
  }
  return peak > 0 ? Math.min(NORMALIZE_MAX_GAIN, NORMALIZE_PEAK / peak) : 1;
}
/// Plays (or, if it is playing, stops) a recording. Every .clip with that data-url shows the
/// playhead, and every button.play with it shows as playing.
async function playClip(url) {
  const toggledOff = playback.url === url;
  stopPlayback();
  if (toggledOff) return;
  // Created and resumed within the click, as browsers require.
  audioCtx = audioCtx || new AudioContext();
  audioCtx.resume();
  playback.url = url;
  markPlaying();
  let clip;
  try { clip = await fetchClip(url); } catch (e) {
    if (playback.url === url) stopPlayback();
    toast("Playback failed: " + e.message, "bad");
    return;
  }
  if (playback.url !== url) return; // something else was played meanwhile
  const source = audioCtx.createBufferSource(), gain = audioCtx.createGain();
  source.buffer = clip.buffer;
  playback.normGain = normalizeGain(clip.buffer);
  gain.gain.value = clipPrefs.normalize ? playback.normGain : 1;
  source.connect(gain).connect(audioCtx.destination);
  source.onended = () => { if (playback.source === source) stopPlayback(); };
  Object.assign(playback, { source, gain, startedAt: audioCtx.currentTime, duration: clip.buffer.duration });
  source.start();
  playback.frame = requestAnimationFrame(movePlayhead);
}
function movePlayhead() {
  playback.frame = requestAnimationFrame(movePlayhead);
  const f = Math.min(1, (audioCtx.currentTime - playback.startedAt) / playback.duration);
  for (const v of document.querySelectorAll(".clip[data-url]")) {
    const on = v.dataset.url === playback.url;
    v.classList.toggle("playing", on);
    if (on) v.querySelector(".playhead").style.left = (f * 100).toFixed(2) + "%";
  }
}
function stopPlayback() {
  if (playback.source) {
    playback.source.onended = null;
    try { playback.source.stop(); } catch (e) { /* already stopped */ }
  }
  cancelAnimationFrame(playback.frame);
  Object.assign(playback, { url: null, source: null, gain: null, frame: 0 });
  for (const v of document.querySelectorAll(".clip.playing")) v.classList.remove("playing");
  markPlaying();
}
/// Shows which play buttons belong to the recording that is playing.
function markPlaying() {
  for (const b of document.querySelectorAll("button.play")) {
    const on = b.dataset.url === playback.url;
    b.classList.toggle("playing", on);
    if (b.dataset.label) b.textContent = on ? "Stop" : b.dataset.label;
  }
}
function setNormalize(on) {
  clipPrefs.normalize = on;
  clipPrefs.save();
  if (playback.gain) playback.gain.gain.value = on ? playback.normGain : 1;
}
// One listener serves every recording and play button, whenever they are drawn.
document.addEventListener("click", e => {
  const el = e.target.closest?.(".clip[data-url], button.play[data-url]");
  if (el) playClip(el.dataset.url);
});

/// Time-axis labels under a recording `len` ns long: its start, its end, and round seconds between.
function timeAxis(len) {
  const secs = len / 1e9, step = [0.5, 1, 2, 5, 10, 20].find(s => secs / s <= 8) || 30;
  const marks = [0];
  for (let t = step; t < secs - step * 0.4; t += step) marks.push(t);
  marks.push(secs);
  return marks.map(t => `<span style="left:${(t / secs * 100).toFixed(2)}%">${t.toFixed(t % 1 ? 1 : 0)} s</span>`).join("");
}

// ---- the microphone map: positions around the site origin, seen from above ----

const MAP_SIZE = 400, MAP_CENTER = 200, MAP_RADIUS = 164;
/// Draws `mics` ([{ label, x, y, z, cls }], metres east and north) into an <svg class="mic-map">
/// with distance rings. `cls` may hold "inactive", "selected" or "draft".
function renderMicMap(svg, mics, emptyText) {
  const key = JSON.stringify([mics, emptyText]);
  if (svg.__key === key) return;
  svg.__key = key;
  // Ring spacing: a round step (1, 2 or 5 x 10^n m) giving a few rings out to the farthest mic.
  const extent = Math.max(1, ...mics.map(m => Math.hypot(m.x, m.y)));
  let step = 1;
  for (const s of [1, 2, 5, 10, 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000]) {
    step = s;
    if (extent / s <= 4) break;
  }
  const rings = Math.max(1, Math.ceil(extent * 1.05 / step));
  const scale = MAP_RADIUS / (rings * step);
  const px = x => MAP_CENTER + x * scale, py = y => MAP_CENTER - y * scale;
  const parts = [];
  for (let i = 1; i <= rings; i++) {
    const r = i * step * scale;
    parts.push(`<circle class="ring" cx="${MAP_CENTER}" cy="${MAP_CENTER}" r="${r.toFixed(1)}"/>`);
    parts.push(`<text class="scale" x="${MAP_CENTER + 4}" y="${(MAP_CENTER - r - 3).toFixed(1)}">${i * step} m</text>`);
  }
  const R = rings * step * scale;
  parts.push(`<line class="axis" x1="${MAP_CENTER - R}" y1="${MAP_CENTER}" x2="${MAP_CENTER + R}" y2="${MAP_CENTER}"/>`);
  parts.push(`<line class="axis" x1="${MAP_CENTER}" y1="${MAP_CENTER - R}" x2="${MAP_CENTER}" y2="${MAP_CENTER + R}"/>`);
  parts.push(`<text class="compass" x="${MAP_CENTER}" y="16" text-anchor="middle">N</text>`);
  parts.push(`<text class="compass" x="${MAP_CENTER}" y="${MAP_SIZE - 6}" text-anchor="middle">S</text>`);
  parts.push(`<text class="compass" x="${MAP_SIZE - 8}" y="${MAP_CENTER + 4}" text-anchor="middle">E</text>`);
  parts.push(`<text class="compass" x="8" y="${MAP_CENTER + 4}" text-anchor="middle">W</text>`);
  // Draw the selected and draft marks last, so they sit on top.
  const order = m => ((m.cls || "").includes("draft") ? 2 : (m.cls || "").includes("selected") ? 1 : 0);
  for (const m of [...mics].sort((a, b) => order(a) - order(b))) {
    const x = px(m.x), y = py(m.y), k = 5;
    // Keep labels inside the map: flip to the left of the mark near the right edge.
    const right = x < MAP_SIZE - 110;
    parts.push(`<g class="${m.cls || ""}"><title>${esc(m.label)}: ${m.x} m east, ${m.y} m north, ${m.z} m up</title>` +
      `<path class="mic" d="M${x - k} ${y - k}L${x + k} ${y + k}M${x - k} ${y + k}L${x + k} ${y - k}"/>` +
      `<text class="mic-label" x="${right ? x + 8 : x - 8}" y="${y - 7}" text-anchor="${right ? "start" : "end"}">${esc(m.label)}</text></g>`);
  }
  if (!mics.length) parts.push(`<text class="scale" x="${MAP_CENTER}" y="${MAP_CENTER - 12}" text-anchor="middle">${esc(emptyText || "No microphone has a position yet.")}</text>`);
  svg.setAttribute("viewBox", `0 0 ${MAP_SIZE} ${MAP_SIZE}`);
  svg.innerHTML = parts.join("");
}
