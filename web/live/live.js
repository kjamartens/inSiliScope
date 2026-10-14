// SPDX-License-Identifier: BSD-3-Clause
// The viewer's live microscope (the right panel): the camera image at the stage position (= the world view's
// centre), rendered frame after frame by a worker of its own; Snap, Record (the movie player), the focus (Z stage),
// exposure and display range with a histogram; the performance breakdown per frame; streaming the frames to webSMLM
// (through tools/websmlm_relay.mjs); the splitter between the world view and the panel; the View pop-over.
//
// Loaded after the viewer's main script (web/index.html) and uses its globals: movieSpec, spawnGenWorker,
// genWorkers, view, requestDraw, resizeCanvas, makeMovie, uiKit. A live frame is a `live` job (cli/scope_wasm.cpp
// isc_live_frame: sim::LiveScope, the adapter's live mode, with the sample's illumination history); an engine
// without it (the lab's JS reference) gets single-frame `movie` jobs at an advancing start time instead.
'use strict';
(function () {
const $ = id => document.getElementById(id);
const store = uiKit.store;

// ---- splitter: the right panel's width, 33-62 % of the window (50 % by default) ----
const wrap = $('wrap'), splitter = $('splitter');
const LIVE_MIN = 0.33, LIVE_MAX = 0.62;
function setLiveFrac(f, save) {
  f = Math.max(LIVE_MIN, Math.min(LIVE_MAX, f));
  wrap.style.setProperty('--live-w', (100 * f).toFixed(2) + 'vw');
  if (save) store.set('liveWidth', f.toFixed(4));   // (was liveFrac, default 0.4: a new key so the new default shows)
}
setLiveFrac(+store.get('liveWidth', '0.5') || 0.5, false);
splitter.addEventListener('pointerdown', e => {
  if (e.button !== 0) return;
  e.preventDefault();
  splitter.classList.add('drag');
  splitter.setPointerCapture(e.pointerId);
  const move = ev => { setLiveFrac((innerWidth - ev.clientX) / innerWidth, true); resizeCanvas(); };
  const up = () => { splitter.classList.remove('drag'); splitter.removeEventListener('pointermove', move); splitter.removeEventListener('pointerup', up); resizeCanvas(); drawScale(); };
  splitter.addEventListener('pointermove', move); splitter.addEventListener('pointerup', up);
});

// ---- the View pop-over over the world view ----
const viewBtn = $('viewBtn'), viewPanel = $('viewPanel');
function showViewPanel(on) {
  viewPanel.hidden = !on;
  viewBtn.classList.toggle('on', on);
  viewBtn.setAttribute('aria-expanded', on);
  store.set('viewPanel', on ? '1' : '0');
}
viewBtn.addEventListener('click', () => showViewPanel(viewPanel.hidden));
showViewPanel(store.get('viewPanel', '0') === '1');
document.addEventListener('keydown', ev => { if (ev.key === 'Escape' && !viewPanel.hidden && viewPanel.contains(document.activeElement)) showViewPanel(false); });

// ---- controls mirrored from the settings (one value, two places) ----
function mirror(id, srcId) {
  const a = $(id), b = $(srcId);
  a.value = b.value;
  a.addEventListener('input', () => { if (a.value !== '' && b.value !== a.value) { b.value = a.value; b.dispatchEvent(new Event('input', { bubbles: true })); b.dispatchEvent(new Event('change', { bubbles: true })); } });
  b.addEventListener('input', () => { a.value = b.value; });
  b.addEventListener('change', () => { a.value = b.value; });
}
mirror('lv_z', 'mv_z');
mirror('lv_exp', 'mv_exposure-ms');
const stepZ = dir => {
  const z = $('mv_z'), st = Math.max(0.001, +$('lv_zStep').value || 0.1);
  z.value = +(+z.value + dir * st).toFixed(4);
  z.dispatchEvent(new Event('input', { bubbles: true }));
  z.dispatchEvent(new Event('change', { bubbles: true }));
};
$('lv_zUp').addEventListener('click', () => stepZ(1));
$('lv_zDn').addEventListener('click', () => stepZ(-1));
$('lv_zStep').value = store.get('zStep', '0.1');
$('lv_zStep').addEventListener('input', () => store.set('zStep', $('lv_zStep').value));
// Up/Down arrows over the live image (or PageUp/PageDown anywhere outside an input) step the focus.
document.addEventListener('keydown', ev => {
  const t = ev.target, inField = t && (t.tagName === 'INPUT' || t.tagName === 'SELECT' || t.tagName === 'TEXTAREA');
  if (inField) return;
  if (ev.key === 'PageUp' || ev.key === 'PageDown') { ev.preventDefault(); stepZ(ev.key === 'PageUp' ? 1 : -1); }
});
$('liveView').addEventListener('wheel', ev => { ev.preventDefault(); stepZ(ev.deltaY < 0 ? 1 : -1); }, { passive: false });
$('lv_rec').addEventListener('click', () => makeMovie());

// ---- the live worker ----
// A worker of its own (the same engine as the others), so a live frame never waits behind the data layers, the dye
// events or a movie; spawned on the first Live or Snap.
const L = self.liveState = { on: false, worker: null, pending: null, id: 0, frames: 0, t: 0, history: null,
  lastShown: 0, fps: 0, ms: 0, w: 0, h: 0, frame: null, lo: 0, hi: 1, spec: '', snap: false, err: '' };
function worker() {
  if (L.worker) return L.worker;
  if (!genWorkers.ok) return null;
  L.worker = spawnGenWorker();
  L.worker.onmessage = e => onResult(e.data);
  // a failure here stops live only (the shared handler would move every worker to the main thread)
  L.worker.onerror = e => { e.preventDefault(); L.err = e.message || 'the live worker failed'; L.pending = null; L.worker.terminate(); L.worker = null; setLive(false); };
  return L.worker;
}
// The live session follows the sample: a new world (seed, cell or specimen parameters) is a fresh sample, as the
// adapter resets its history on a world change.
function worldKey(spec) { return spec.split(' ').filter(t => /^(world-seed|specimen|p\.)/.test(t)).join(' '); }
function request() {
  const w = worker();
  if (!w) { L.err = 'the workers are not up yet'; showInfo(); return; }
  if (L.pending) return;
  const spec = movieSpec({ frames: 1 });
  const wk = worldKey(spec);
  const reset = wk !== L.worldKey;
  if (reset) { L.worldKey = wk; L.t = 0; }
  const job = { type: 'live', id: ++L.id, spec, reset, t: L.t, lit: true, t0: performance.now(), profile: $('lv_perf').checked };
  L.pending = job;
  w.postMessage(job);
}
function onResult(d) {
  const job = L.pending;
  if (!job || d.id !== job.id) return;
  L.pending = null;
  if (d.error) { L.err = d.error; L.on = false; syncButtons(); showInfo(); return; }
  L.err = '';
  L.ms = performance.now() - job.t0;
  L.t = d.t !== undefined ? d.t : L.t + exposureSec();
  L.clock = d.clock;
  L.drift = d.drift;
  L.engine = d.engine;
  L.paths = d.paths || null;
  L.perf = { roundMs: L.ms, workerMs: d.workerMs, profile: d.profile || null };
  const due = L.lastShown + exposureSec() * 1000, now = performance.now();
  L.perf.waitMs = L.on ? Math.max(0, due - now) : 0;
  const show = () => {
    const t = performance.now();
    if (L.lastShown) L.fps = 0.8 * L.fps + 0.2 * (1000 / Math.max(1, t - L.lastShown));
    L.lastShown = t;
    L.frames++;
    draw(d);
    streamFrame(d);
    showPerf();
    if (L.on && !document.hidden) request();
  };
  if (L.on && now < due) setTimeout(show, due - now); else show();
}
const exposureSec = () => Math.max(0.001, (+$('mv_exposure-ms').value || 50) / 1000);

// ---- display: grey LUT, the range from the frame (auto) or the boxes ----
const cv = $('lv_cv'), g = cv.getContext('2d');
let img = null, u32 = null, lut = null, lutKey = '';
const LE = new Uint8Array(new Uint32Array([1]).buffer)[0] === 1;
function autoRange(fr) {
  const s = [], step = Math.max(1, (fr.length / 20000) | 0);
  for (let i = 0; i < fr.length; i += step) s.push(fr[i]);
  s.sort((a, b) => a - b);
  const lo = s[Math.floor(s.length * 0.001)], hi = Math.max(lo + 1, s[Math.min(s.length - 1, Math.floor(s.length * 0.999))]);
  return [lo, hi];
}
function draw(d) {
  L.frame = d.frames; L.w = d.w; L.h = d.h; L.spec = d.spec || L.pendingSpec || '';
  $('lv_idle').style.display = 'none';
  if ($('lv_auto').checked) {
    const [lo, hi] = autoRange(d.frames);
    // a little smoothing so the display does not flicker with the blinks
    L.lo = L.frames > 1 && L.hiSet ? Math.round(0.7 * L.lo + 0.3 * lo) : lo;
    L.hi = L.frames > 1 && L.hiSet ? Math.round(0.7 * L.hi + 0.3 * hi) : hi;
    L.hiSet = true;
    $('lv_lo').value = L.lo; $('lv_hi').value = L.hi;
  } else { L.lo = +$('lv_lo').value || 0; L.hi = Math.max(L.lo + 1, +$('lv_hi').value || 1); }
  const t0 = performance.now();
  paint();
  const t1 = performance.now();
  drawHist(true);
  if (L.perf) { L.perf.paintMs = t1 - t0; L.perf.histMs = performance.now() - t1; }
  showInfo();
  drawScale();
  requestDraw();
}

// ---- the live histogram: log counts, the display range over it ----
// The axes move slowly: x spans the darkest to the brightest pixel of the frames of the last 10 s, y the tallest bar of
// those frames, so a blink or a dim frame does not rescale it.
const hc = $('lv_hist'), hg = hc.getContext('2d'), NB = 128, HIST_KEEP_MS = 10000;
let hist = null, hMin = 0, hMax = 1, hTop = 0;
const hRecent = [];   // {t, mn, mx, top} per frame shown
function drawHist(newFrame) {
  const fr = L.frame;
  const W = Math.max(1, Math.round(hc.clientWidth * devicePixelRatio)), H = Math.round(56 * devicePixelRatio);
  if (hc.width !== W || hc.height !== H) { hc.width = W; hc.height = H; }
  hg.clearRect(0, 0, W, H);
  if (!fr) return;
  if (newFrame || !hist) {
    let mn = 65535, mx = 0;
    for (let i = 0; i < fr.length; i++) { const v = fr[i]; if (v < mn) mn = v; if (v > mx) mx = v; }
    const now = performance.now(), rec = { t: now, mn, mx, top: 0 };
    hRecent.push(rec);
    while (hRecent.length > 1 && now - hRecent[0].t > HIST_KEEP_MS) hRecent.shift();
    hMin = Math.min(...hRecent.map(r => r.mn)); hMax = Math.max(hMin + 1, ...hRecent.map(r => r.mx));
    hist = new Float64Array(NB);
    const k = NB / (hMax - hMin + 1);
    for (let i = 0; i < fr.length; i++) hist[Math.min(NB - 1, ((fr[i] - hMin) * k) | 0)]++;
    for (let b = 0; b < NB; b++) { hist[b] = Math.log10(1 + hist[b]); rec.top = Math.max(rec.top, hist[b]); }
    hTop = Math.max(...hRecent.map(r => r.top));
  }
  const top = hTop;
  const css = getComputedStyle(document.body), accent = css.getPropertyValue('--accent').trim() || '#4a90d9';
  const muted = css.getPropertyValue('--muted').trim() || '#8b97a8', pad = 9 * devicePixelRatio, ph = H - pad;
  const xOf = v => ((v - hMin) / (hMax - hMin)) * W;
  // the display range (black .. white) as a shaded band, then the bars and the two markers
  const xl = xOf(L.lo), xh = xOf(L.hi);
  hg.fillStyle = muted; hg.globalAlpha = 0.12; hg.fillRect(Math.max(0, xl), 0, Math.max(1, Math.min(W, xh) - Math.max(0, xl)), ph);
  hg.globalAlpha = 1; hg.fillStyle = muted;
  const bw = W / NB;
  for (let b = 0; b < NB; b++) {
    const hh = top ? (hist[b] / top) * (ph - 2) : 0;
    hg.fillRect(b * bw, ph - hh, Math.max(1, bw - 0.5), hh);
  }
  hg.strokeStyle = accent; hg.lineWidth = Math.max(1, devicePixelRatio);
  for (const x of [xl, xh]) if (x >= -1 && x <= W + 1) { hg.beginPath(); hg.moveTo(x, 0); hg.lineTo(x, ph); hg.stroke(); }
  hg.fillStyle = muted; hg.font = `${9 * devicePixelRatio}px system-ui, Segoe UI, sans-serif`; hg.textBaseline = 'bottom';
  hg.textAlign = 'left'; hg.fillText(String(hMin), 1, H);
  hg.textAlign = 'right'; hg.fillText(String(hMax) + ' ADU', W - 1, H);
}
// Dragging in the histogram moves the nearer marker (the display range by hand: auto off).
hc.addEventListener('pointerdown', ev => {
  if (!L.frame) return;
  ev.preventDefault();
  hc.setPointerCapture(ev.pointerId);
  const r = hc.getBoundingClientRect(), valAt = e => Math.round(hMin + ((e.clientX - r.left) / r.width) * (hMax - hMin));
  const which = Math.abs(valAt(ev) - L.lo) <= Math.abs(valAt(ev) - L.hi) ? 'lo' : 'hi';
  const move = e => {
    const v = valAt(e);
    if (which === 'lo') L.lo = Math.min(v, L.hi - 1); else L.hi = Math.max(v, L.lo + 1);
    $('lv_auto').checked = false;
    $('lv_lo').value = L.lo; $('lv_hi').value = L.hi;
    paint(); drawHist();
  };
  const up = () => { hc.removeEventListener('pointermove', move); hc.removeEventListener('pointerup', up); };
  move(ev);
  hc.addEventListener('pointermove', move); hc.addEventListener('pointerup', up);
});
function paint() {
  if (!L.frame) return;
  const w = L.w, h = L.h;
  if (cv.width !== w || cv.height !== h) { cv.width = w; cv.height = h; img = null; }
  if (!img) { img = g.createImageData(w, h); u32 = new Uint32Array(img.data.buffer); }
  const key = L.lo + '|' + L.hi;
  if (key !== lutKey) {
    lut = new Uint32Array(65536);
    const k = 255 / (L.hi - L.lo);
    for (let a = 0; a < 65536; a++) {
      const b = Math.max(0, Math.min(255, Math.round((a - L.lo) * k)));
      lut[a] = LE ? (0xff000000 | (b << 16) | (b << 8) | b) >>> 0 : ((b << 24) | (b << 16) | (b << 8) | 0xff) >>> 0;
    }
    lutKey = key;
  }
  const fr = L.frame, n = w * h;
  for (let i = 0; i < n; i++) u32[i] = lut[fr[i]];
  g.putImageData(img, 0, 0);
}
for (const id of ['lv_lo', 'lv_hi']) $(id).addEventListener('input', () => { $('lv_auto').checked = false; L.lo = +$('lv_lo').value; L.hi = Math.max(L.lo + 1, +$('lv_hi').value); paint(); drawHist(); });
$('lv_auto').addEventListener('change', () => { L.hiSet = false; if (L.frame && $('lv_auto').checked) { const [lo, hi] = autoRange(L.frame); L.lo = lo; L.hi = hi; $('lv_lo').value = lo; $('lv_hi').value = hi; paint(); drawHist(); } });

const MODES = ['dSTORM', 'PALM', 'DNA-PAINT', 'WideField'];
function showInfo() {
  const el = $('lv_info');
  if (L.err) { el.textContent = 'live failed: ' + L.err; return; }
  if (!L.frames) { el.textContent = L.on ? 'starting…' : ''; return; }
  const bf = $('mv_modality').value === '1', both = self.ISC_MM_EXTRA && self.ISC_MM_EXTRA['light-trans'] === 1 && !bf;
  const lines = [(L.on ? '● ' + L.fps.toFixed(1) + ' fps' : 'snap') + ' · ' + Math.round(L.ms) + ' ms/frame',
    `${L.w}×${L.h} px · ${bf ? 'BrightField' : both ? 'fluorescence + BrightField' : 'fluorescence'} · z ${(+$('mv_z').value).toFixed(2)} µm`,
    `t ${L.t.toFixed(1)} s` + (L.clock !== undefined ? ` · lit here ${L.clock.toFixed(1)} s` : '') + ` · ${L.lo}–${L.hi} ADU`];
  const how = renderPaths(bf, both);
  if (how) lines.push('render: ' + how);
  if (L.drift && L.drift.some(v => Math.abs(v) >= 0.5)) lines.push(`drift ${L.drift.map(v => v.toFixed(0)).join(', ')} nm`);
  if (L.engine === 'movie') lines.push('(no sample history in this engine)');
  el.textContent = lines.join('\n');
}
// How the frame was drawn (LiveScope's render paths, spec/ALGORITHM.md "Blink render regimes"): blinks splatted one by
// one (SMLM), binned on a grid and FFT-convolved (the intermediate, approximate SMLM) or mean-field (the expected
// image, widefield-like); continuous populations (WideField labels, dSTORM initial ON, PALM pre states) mean-field
// or per dye. BrightField: the lamp's multislice image.
function renderPaths(bf, both) {
  const parts = [];
  if (bf || both) parts.push('BrightField (multislice)');
  if (!bf) for (const p of L.paths || []) {
    let m;
    if ((m = /^SMLM: (\d+) blinks \(splat\)$/.exec(p))) parts.push(`SMLM splat (${m[1]} blinks)`);
    else if ((m = /^SMLM: (\d+) blinks \(binned FFT\)$/.exec(p))) parts.push(`binned SMLM (${m[1]} blinks)`);
    else if ((m = /^(.*): mean-field \(FFT\)$/.exec(p))) parts.push(`${m[1]} mean-field (widefield)`);
    else parts.push(p.replace(/: /, ' '));
  }
  return parts.join(' + ');
}
// ---- performance: where a frame's time goes ----
// The engine's phases (Simulation/Timing.h, collected per frame by the module: isc_live_profile), the rest of the
// worker's call, the message to and from it, the display; and the wait that paces frames to the exposure.
const PHASES = {
  'fl.total': 'fluorescence, total', 'fl.setup': 'setup', 'fl.make-setup': 'resolve the spec', 'fl.configure': 'labels, light path',
  'fl.history': 'illumination history', 'fl.clock-regions': 'clock regions', 'fl.events-query': 'blink events (core)',
  'fl.events-sort': 'sort events', 'fl.windows': 'per-dye windows', 'fl.dye-counts': 'dye counts', 'fl.psf-kernels': 'PSF kernels',
  'fl.mean-field-scene': 'mean-field scene', 'fl.mean-field-weights': 'mean-field weights', 'fl.mean-field-render': 'mean-field image',
  'fl.frame.blinks (batches)': 'blinks (splat / binned)', 'fl.frame.continuous': 'continuous dyes', 'fl.frame.noise (batches)': 'camera noise',
  'fl.frame.onFrame': 'hand-over', 'bf.total': 'BrightField, total', 'bf.scene': 'BrightField scene', 'bf.scene-update': 'BrightField scene update',
  'bf.image': 'BrightField image', 'bf.frame.noise': 'BrightField noise', 'bf.frame.onFrame': 'BrightField hand-over',
  'psf.compute': 'PSF kernel (compute)', 'psf.zernike-planes': 'PSF planes', 'psf.memo-hit': 'PSF kernel (memo)',
};
const TOTALS = new Set(['fl.total', 'bf.total']);
function showPerf() {
  const out = $('lv_perfOut');
  if (!$('lv_perf').checked || !L.perf) { out.innerHTML = ''; return; }
  const P = L.perf, rows = [], ms = v => (v >= 100 ? v.toFixed(0) : v >= 10 ? v.toFixed(1) : v.toFixed(2)) + ' ms';
  const row = (name, v, cls) => rows.push(`<span class="${cls || ''}">${name}</span><span class="${cls || ''}">${ms(v)}</span>`);
  row('frame: request → result', P.roundMs, 'tot');
  if (P.workerMs !== undefined) {
    row('engine (worker)', P.workerMs, 'sub');
    const ph = P.profile && P.profile.phases ? Object.entries(P.profile.phases) : [];
    let inner = 0;
    for (const [k, v] of ph.sort((a, b) => b[1].totalMs - a[1].totalMs)) {
      if (v.totalMs < 0.05) continue;
      const nm = (PHASES[k] || k) + (v.count > 1 ? ` ×${v.count}` : '');
      row(nm, v.totalMs, TOTALS.has(k) ? 'sub' : 'sub" style="padding-left:16px');
      if (TOTALS.has(k)) inner += v.totalMs;
    }
    if (inner) row('rest of the engine call', Math.max(0, P.workerMs - inner), 'sub" style="padding-left:16px');
    row('to and from the worker', Math.max(0, P.roundMs - P.workerMs), 'sub');
  }
  if (P.paintMs !== undefined) row('display (image, histogram)', P.paintMs + (P.histMs || 0), 'tot');
  if (P.streamMs !== undefined) row('stream (TIFF, send)', P.streamMs, 'tot');
  if (L.on) row('wait (paced to the exposure)', P.waitMs || 0, 'tot');
  out.innerHTML = rows.join('');
}
$('lv_perf').checked = store.get('livePerf', '0') === '1';
$('lv_perf').addEventListener('change', () => { store.set('livePerf', $('lv_perf').checked ? '1' : '0'); showPerf(); if (!L.on && L.frame && $('lv_perf').checked) request(); });

// ---- streaming to webSMLM ----
// webSMLM's Live streaming panel connects out to a WebSocket server (the Micro-Manager plugin webSMLM_Streaming is one)
// and takes each binary message as one chunk: an ImageJ TIFF of the frames. A page cannot be a server, so the frames go
// to a relay (tools/websmlm_relay.mjs: senders on /source, webSMLM on the bare port), one 16-bit TIFF per frame shown;
// stopping sends {"cmd":"stop"}. Frames are dropped, not queued, while the socket is backed up.
const ST = { on: false, ws: null, sent: 0, dropped: 0, acked: null, retry: 0 };
$('lv_streamUrl').value = store.get('streamUrl', 'ws://localhost:8765/source');
$('lv_streamUrl').addEventListener('change', () => { store.set('streamUrl', $('lv_streamUrl').value.trim()); if (ST.on) { closeStream(false); openStream(); } });
const streamStatus = msg => { $('lv_streamStatus').textContent = msg; };
function openStream() {
  clearTimeout(ST.retry);
  let ws;
  try { ws = new WebSocket($('lv_streamUrl').value.trim()); } catch (e) { streamStatus('bad address: ' + e.message); return; }
  ws.binaryType = 'arraybuffer';
  ST.ws = ws;
  streamStatus('connecting…');
  ws.onopen = () => { if (ST.ws === ws) streamStatus('relay connected: in webSMLM, Live streaming → Connect to ' + $('lv_streamUrl').value.trim().replace(/\/source$/, '')); };
  ws.onmessage = e => {
    if (typeof e.data !== 'string') return;
    try { const m = JSON.parse(e.data); if (m.cmd === 'ack' || m.cmd === 'stopAck') { ST.acked = m.framesReceived; showStream(); } } catch (_) { /* not ours */ }
  };
  ws.onclose = () => {
    if (ST.ws !== ws) return;
    ST.ws = null;
    if (!ST.on) return;
    streamStatus('no relay at ' + $('lv_streamUrl').value.trim() + ' — run: node tools/websmlm_relay.mjs (retrying)');
    ST.retry = setTimeout(() => { if (ST.on && !ST.ws) openStream(); }, 2000);
  };
}
function closeStream(sendStop) {
  clearTimeout(ST.retry);
  const ws = ST.ws;
  ST.ws = null;
  if (!ws) return;
  if (sendStop && ws.readyState === WebSocket.OPEN) ws.send(JSON.stringify({ cmd: 'stop' }));
  ws.close();
}
function showStream() {
  if (!ST.on || !ST.ws || ST.ws.readyState !== WebSocket.OPEN) return;
  streamStatus(`streaming: ${ST.sent} frames sent` + (ST.acked !== null ? `, ${ST.acked} in webSMLM` : '') + (ST.dropped ? `, ${ST.dropped} dropped (busy)` : ''));
}
function setStream(on) {
  ST.on = on;
  $('lv_stream').classList.toggle('on', on);
  $('lv_streamRow').hidden = !on;
  if (on) { ST.sent = 0; ST.dropped = 0; ST.acked = null; openStream(); } else closeStream(true);
}
$('lv_stream').addEventListener('click', () => setStream(!ST.on));
function streamFrame(d) {
  const ws = ST.ws;
  if (!ST.on || !ws || ws.readyState !== WebSocket.OPEN || !d.frames) { if (L.perf) delete L.perf.streamMs; return; }
  const t0 = performance.now();
  if (ws.bufferedAmount > 16 * d.frames.length * 2) ST.dropped++;
  else { ws.send(writeTiff16(d.frames, d.w, d.h, 1, 'ImageJ=1.11a\nimages=1\nslices=1\n')); ST.sent++; }
  if (L.perf) L.perf.streamMs = performance.now() - t0;
  showStream();
}

// Scale bar: a 1-2-5 length near a quarter of the displayed image.
function drawScale() {
  const el = $('lv_scale');
  if (!L.frame) { el.innerHTML = ''; return; }
  const r = cv.getBoundingClientRect(), dispPx = Math.min(r.width / L.w, r.height / L.h);
  const pxUm = (+$('mv_pixel-nm').value || 100) / 1000, cssPerUm = dispPx / pxUm;
  const want = r.width * 0.22 / cssPerUm;
  const nice = [0.1, 0.2, 0.5, 1, 2, 5, 10, 20, 50, 100, 200];
  const len = nice.reduce((a, b) => (Math.abs(b - want) < Math.abs(a - want) ? b : a));
  el.innerHTML = `${len < 1 ? len * 1000 + ' nm' : len + ' µm'}<i style="width:${(len * cssPerUm).toFixed(1)}px"></i>`;
}
window.addEventListener('resize', drawScale);

// ---- buttons ----
function syncButtons() {
  $('lv_live').classList.toggle('on', L.on);
  $('lv_live').textContent = L.on ? '❚❚ Stop' : '▶ Live';
  showInfo();
  requestDraw();
}
function setLive(on) {
  L.on = on;
  syncButtons();
  if (on) { L.lastShown = 0; L.fps = 0; request(); }
}
$('lv_live').addEventListener('click', () => setLive(!L.on));
$('lv_fresh').addEventListener('click', () => { L.worldKey = null; L.t = 0; if (!L.on && L.frame) request(); });
$('lv_snap').addEventListener('click', () => { if (L.on) setLive(false); L.lastShown = 0; request(); });
document.addEventListener('visibilitychange', () => { if (!document.hidden && L.on && !L.pending) request(); });
window.liveRedraw = () => { paint(); drawScale(); };
})();
