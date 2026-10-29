// SPDX-License-Identifier: BSD-3-Clause
// The viewer's live microscope (the right panel): the camera image at the stage position (= the world view's
// centre), rendered frame after frame by a worker of its own; Snap, Record (the movie player), the focus (Z stage),
// exposure and display range with a histogram; the performance breakdown per frame; the splitter between the world view
// and the panel; the View pop-over.
//
// Loaded after the viewer's main script (web/index.html) and uses its globals: movieSpec, spawnGenWorker,
// genWorkers, view, requestDraw, resizeCanvas, makeMovie, uiKit. A live frame is a `live` job (cli/scope_wasm.cpp
// isc_live_frame: sim::LiveScope, the adapter's live mode, with the sample's light history).
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
// the idle image's ▶ Live = the bar's
$('lv_idleLive').addEventListener('click', () => $('lv_live').click());
showViewPanel(store.get('viewPanel', '0') === '1');
// a click anywhere outside the pop-over (and its button) closes it
document.addEventListener('pointerdown', ev => {
  if (!viewPanel.hidden && !viewPanel.contains(ev.target) && !viewBtn.contains(ev.target)) showViewPanel(false);
}, true);
document.addEventListener('keydown', ev => { if (ev.key === 'Escape' && !viewPanel.hidden && viewPanel.contains(document.activeElement)) showViewPanel(false); });

// ---- the laser rows of Illumination & light path (in the controls column): a slider beside each number ----
for (const num of document.querySelectorAll('#lv_illumHost input[type=number][id^="mv_laser-"]')) {
  if (num.id === 'mv_laser-custom-nm') continue;
  const r = Object.assign(document.createElement('input'), { type: 'range', min: num.min || 0, max: num.max || 10, step: 0.001, value: num.value });
  r.setAttribute('aria-label', num.id.replace('mv_', ''));
  num.parentElement.classList.add('lsl');
  num.before(r);
  r.addEventListener('input', () => { num.value = r.value; num.dispatchEvent(new Event('input', { bubbles: true })); num.dispatchEvent(new Event('change', { bubbles: true })); });
  const back = () => { if (num.value !== '') r.value = num.value; };
  num.addEventListener('input', back);
  num.addEventListener('change', back);
}

// A settings group in the column (Illumination & light path, Objective & PSF, Camera) shows only when one of its rows
// does at the current level (Objective & PSF has no Basic row).
function syncHostBoxes() {
  for (const id of ['lv_illumHost', 'lv_objHost', 'lv_camHost']) {
    const box = $(id);
    if (box) box.hidden = ![...box.querySelectorAll('.opt')].some(e => getComputedStyle(e).display !== 'none');
  }
}
// A click on a box's title folds it (Focus, Configuration, Display, Performance; the settings groups fold as details).
for (const t of document.querySelectorAll('#liveCtl .lc > h5')) t.addEventListener('click', () => t.parentElement.classList.toggle('shut'));
let hostTimer = 0;
const syncHostsSoon = () => { clearTimeout(hostTimer); hostTimer = setTimeout(syncHostBoxes, 30); };
new MutationObserver(syncHostsSoon).observe(document.body, { attributes: true, attributeFilter: ['data-level'] });
document.addEventListener('change', syncHostsSoon);
syncHostBoxes();

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
// the Z field's own arrows step by the step size
const syncZStep = () => { $('lv_z').step = String(Math.max(0.001, +$('lv_zStep').value || 0.1)); };
const stepZ = dir => {
  const z = $('mv_z'), st = Math.max(0.001, +$('lv_zStep').value || 0.1);
  z.value = +(+z.value + dir * st).toFixed(4);
  z.dispatchEvent(new Event('input', { bubbles: true }));
  z.dispatchEvent(new Event('change', { bubbles: true }));
};
$('lv_zStep').value = store.get('zStep', '0.1');
syncZStep();
$('lv_zStep').addEventListener('input', () => { store.set('zStep', $('lv_zStep').value); syncZStep(); });
// Up/Down arrows over the live image (or PageUp/PageDown anywhere outside an input) step the focus.
document.addEventListener('keydown', ev => {
  const t = ev.target, inField = t && (t.tagName === 'INPUT' || t.tagName === 'SELECT' || t.tagName === 'TEXTAREA');
  if (inField) return;
  if (ev.key === 'PageUp' || ev.key === 'PageDown') { ev.preventDefault(); stepZ(ev.key === 'PageUp' ? 1 : -1); }
});
$('liveView').addEventListener('wheel', ev => { ev.preventDefault(); stepZ(ev.deltaY < 0 ? 1 : -1); }, { passive: false });

// ---- the live worker ----
// A worker of its own (the same engine as the others), so a live frame never waits behind the data layers, the dye
// events or a movie; spawned on the first Live or Snap.
const L = self.liveState = { on: false, worker: null, pending: null, id: 0, frames: 0, t: 0, history: null,
  lastShown: 0, fps: 0, ms: 0, w: 0, h: 0, frame: null, lo: 0, hi: 1, spec: '', snap: false, err: '' };
L.stepZ = stepZ;   // (the phone's focus knob, web/live/mobile.js)
function worker() {
  if (L.worker) return L.worker;
  if (!genWorkers.ok) return null;
  L.worker = spawnGenWorker();
  L.worker.onmessage = e => onResult(e.data);
  // a failure here stops live only (the shared handler would move every worker to the main thread)
  L.worker.onerror = e => { e.preventDefault(); L.err = e.message || 'the live worker failed'; L.pending = null; L.worker.terminate(); L.worker = null; L.worldKey = null; setLive(false); };
  return L.worker;
}
// The live session follows the sample: a new world (seed, cell or specimen parameters) is a fresh sample, as the
// adapter resets its history on a world change.
function worldKey(spec) { return spec.split(' ').filter(t => /^(world-seed|specimen|p\.)/.test(t)).join(' '); }
// n > 1: a recording chunk (n frames at once, LiveScope::Frames).
function request(n) {
  const w = worker();
  if (!w) { L.err = 'the workers are not up yet'; showInfo(); return; }
  if (L.pending) return;
  const spec = movieSpec({ frames: 1 });
  const wk = worldKey(spec);
  const reset = wk !== L.worldKey;
  if (reset) { L.worldKey = wk; L.t = 0; L.log = []; }
  const job = { type: 'live', id: ++L.id, spec, reset, t: L.t, lit: true, t0: performance.now(), profile: !n && expert() };
  if (n) { job.n = n; job.rec = true; }
  L.pending = job;
  w.postMessage(job);
  syncIdle();
}
function onResult(d) {
  const job = L.pending;
  if (!job || d.id !== job.id) return;
  if (d.type === 'movie-progress') { if (job.fresh) onRecProgress(d); return; }
  L.pending = null;
  if (job.type === 'live-replay') { onReplay(d, job); return; }
  if (d.error) { L.err = d.error; L.on = false; if (REC.on) finishRec(); syncButtons(); showInfo(); return; }
  if (job.fresh) { onRecMovie(d, job); return; }
  logFrames(job.spec, job.n || 1);
  if (job.rec) { onRecChunk(d, job); return; }
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
    showPerf();
    if (REC.on) recNext();
    else if (L.on && !document.hidden) request();
  };
  if (L.on && now < due) setTimeout(show, due - now); else show();
}
const exposureSec = () => Math.max(0.001, (+$('mv_exposure-ms').value || 50) / 1000);

// ---- the sample's light history: what has been done to it, saved with the measurement (web/live/settings_file.js) ----
// Every frame the live sample took, as runs of one spec (L.log: {spec, n, lit}; a new world or a fresh sample starts it
// afresh). A load replays a saved one (a live-replay job: isc_live_advance per run, no rendering), so the next frame is
// the one the saved session would have rendered.
L.log = [];
function logFrames(spec, n) {
  const last = L.log[L.log.length - 1];
  if (last && last.spec === spec) last.n += n; else L.log.push({ spec, n, lit: true });
}
// records: [{spec, n, lit}] (full specs). Live pauses while it runs; resolves with the sample's time after it.
L.replay = records => new Promise((resolve, reject) => {
  const wasOn = L.on;
  if (L.on) { L.on = false; syncButtons(); }
  const go = () => {
    const w = worker();
    if (!w) { reject(new Error('the workers are not up yet')); return; }
    if (L.pending) { setTimeout(go, 20); return; }
    L.pending = { type: 'live-replay', id: ++L.id, records, resolve, reject, wasOn };
    w.postMessage({ type: 'live-replay', id: L.id, records });
  };
  go();
});
function onReplay(d, job) {
  if (d.error) { L.worldKey = null; job.reject(new Error(d.error)); }
  else {
    L.t = d.t;
    L.worldKey = worldKey(movieSpec({ frames: 1 }));
    L.log = job.records.map(r => ({ ...r }));
    job.resolve(d.t);
  }
  if (job.wasOn) setLive(true);
}
// A fresh sample at the next frame (a measurement without history).
L.freshSample = () => { L.worldKey = null; L.t = 0; L.log = []; };
L.setLive = on => setLive(on);

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
// Zoom into the camera image (Ctrl + wheel, about the pointer; a pinch on a touchpad too): a display zoom of the frame,
// 1-32x. L.zoom = { z, x, y }: the shown part's top-left corner as a fraction of the image. The inset at the top left
// shows the factor and where the shown part sits; a click on it goes back to the whole image.
const Z = L.zoom = { z: 1, x: 0, y: 0 };
function applyZoom() {
  Z.z = Math.min(32, Math.max(1, Z.z));
  if (Z.z < 1.01) Object.assign(Z, { z: 1, x: 0, y: 0 });
  const span = 1 / Z.z;
  Z.x = Math.min(1 - span, Math.max(0, Z.x)); Z.y = Math.min(1 - span, Math.max(0, Z.y));
  const S = $('liveView').clientWidth;
  cv.style.transformOrigin = '0 0';
  cv.style.transform = Z.z === 1 ? '' : `translate(${(-Z.x * Z.z * S).toFixed(2)}px,${(-Z.y * Z.z * S).toFixed(2)}px) scale(${Z.z})`;
  const nav = $('lv_zoomNav');
  nav.hidden = Z.z === 1;
  nav.querySelector('i').style.cssText = `left:${(100 * Z.x).toFixed(2)}%;top:${(100 * Z.y).toFixed(2)}%;width:${(100 * span).toFixed(2)}%;height:${(100 * span).toFixed(2)}%`;
  nav.querySelector('span').textContent = (Z.z < 10 ? Z.z.toFixed(1) : Math.round(Z.z)) + '×';
  cv.style.cursor = 'grab';
  drawScale();
}
// Zoom by a factor about a point on screen (the image under it stays put).
function zoomAt(factor, clientX, clientY) {
  const r = $('liveView').getBoundingClientRect(), S = r.width || 1;
  const px = (clientX - r.left) / S, py = (clientY - r.top) / S;   // the point, as a fraction of the view
  const u = Z.x + px / Z.z, v = Z.y + py / Z.z;                    // ... and of the image
  Z.z *= factor;
  Z.x = u - px / Math.min(32, Math.max(1, Z.z)); Z.y = v - py / Math.min(32, Math.max(1, Z.z));
  applyZoom();
}
$('liveView').addEventListener('wheel', ev => {
  if (!ev.ctrlKey) return;   // the plain wheel steps the focus (above)
  ev.preventDefault(); ev.stopImmediatePropagation();
  zoomAt(Math.exp(-ev.deltaY * 0.0025), ev.clientX, ev.clientY);
}, { passive: false, capture: true });
$('lv_zoomNav').addEventListener('click', () => { Object.assign(Z, { z: 1, x: 0, y: 0 }); applyZoom(); });
window.addEventListener('resize', applyZoom);

// A drag in the live image moves the stage (the world view's centre): the sample follows the pointer, at the image's
// scale (the camera's field over the image's width on screen); not while a recording plays. Zoomed in, a drag moves
// the shown part within the image instead (the stage stays). Two fingers (touch) either move the focus, both moving up
// or down together (one focus step per FOCUS_PX_PER_STEP, up = focus up), or pinch the zoom; the gesture locks to the
// one their first ~12 px of movement looks like.
const FOCUS_PX_PER_STEP = 16;
{
  let drag = null, two = null;
  const fingers = new Map();
  const pair = () => { const [a, b] = [...fingers.values()]; return { mx: (a.x + b.x) / 2, my: (a.y + b.y) / 2, d: Math.max(1, Math.hypot(b.x - a.x, b.y - a.y)) }; };
  cv.style.cursor = 'grab';
  cv.addEventListener('pointerdown', ev => {
    if (ev.button !== 0) return;
    fingers.set(ev.pointerId, { x: ev.clientX, y: ev.clientY });
    cv.setPointerCapture(ev.pointerId);
    if (fingers.size === 2) { drag = null; const p = pair(); two = { ...p, my0: p.my, d0: p.d, lock: null, acc: 0 }; return; }
    if (fingers.size > 2 || (PLAY.on && Z.z === 1)) return;
    drag = { x: ev.clientX, y: ev.clientY };
    cv.style.cursor = 'grabbing';
  });
  cv.addEventListener('pointermove', ev => {
    if (!fingers.has(ev.pointerId)) return;
    fingers.set(ev.pointerId, { x: ev.clientX, y: ev.clientY });
    if (two) {
      if (fingers.size < 2) return;
      const g = pair();
      if (!two.lock) {
        const dy = Math.abs(g.my - two.my0), dd = Math.abs(g.d - two.d0);
        if (dy + dd > 12) { two.lock = dy > dd ? 'focus' : 'zoom'; two.my = two.my0; two.d = two.d0; }   // (counted from the touch)
      }
      if (two.lock === 'zoom') zoomAt(g.d / two.d, g.mx, g.my);
      else if (two.lock === 'focus') {
        two.acc += two.my - g.my;
        while (Math.abs(two.acc) >= FOCUS_PX_PER_STEP) { const s = Math.sign(two.acc); stepZ(s); two.acc -= s * FOCUS_PX_PER_STEP; }
      }
      Object.assign(two, { mx: g.mx, my: g.my, d: g.d });
      return;
    }
    if (!drag) return;
    const dx = ev.clientX - drag.x, dy = ev.clientY - drag.y, S = $('liveView').clientWidth || 1;
    drag = { x: ev.clientX, y: ev.clientY };
    if (Z.z > 1) { Z.x -= dx / (S * Z.z); Z.y -= dy / (S * Z.z); applyZoom(); cv.style.cursor = 'grabbing'; return; }
    const n = +$('mv_size').value || 128, px = (+$('mv_pixel-nm').value || 100) / 1000, um = n * px / S;
    view.cx -= dx * um; view.cy -= dy * um;
    noteInteraction(); requestDraw();
  });
  const end = ev => {
    if (!fingers.delete(ev.pointerId)) return;
    if (fingers.size < 2) two = null;   // (the finger left does nothing until it is lifted)
    if (!fingers.size) { drag = null; cv.style.cursor = 'grab'; }
  };
  cv.addEventListener('pointerup', end); cv.addEventListener('pointercancel', end);
}
// The Configuration's Exposure carries its setting's "differs from the default" mark
{
  const row = $('mv_exposure-ms').closest('.opt'), lab = $('lv_exp').closest('.cfg') ? $('lv_exp').parentElement.previousElementSibling : null;
  const syncMark = () => { if (lab) lab.classList.toggle('changed', row.classList.contains('changed')); };
  new MutationObserver(syncMark).observe(row, { attributes: true, attributeFilter: ['class'] });
  syncMark();
}
// Reset all to defaults (the settings menu): the column's own controls too (the mirrored ones follow their settings)
window.addEventListener('isc-reset', () => {
  $('lv_zStep').value = '0.1'; $('lv_zStep').dispatchEvent(new Event('input'));
  $('lv_auto').checked = true; $('lv_auto').dispatchEvent(new Event('change'));
  setLiveFrac(0.5, true); resizeCanvas(); drawScale();   // the panel's width (the splitter) too
});
$('lv_auto').addEventListener('change', () => { L.hiSet = false; if (L.frame && $('lv_auto').checked) { const [lo, hi] = autoRange(L.frame); L.lo = lo; L.hi = hi; $('lv_lo').value = lo; $('lv_hi').value = hi; paint(); drawHist(); } });

const MODES = ['dSTORM', 'PALM', 'DNA-PAINT', 'WideField'];
// One line, always: the speed and the image size (or the recording, its saved file, an error); the full text in its
// tooltip. How the frame was rendered, the drift and the sample time are in the performance box (Expert).
function showInfo() {
  syncIdle();
  // the image size and the pixel size in the sample (one decimal)
  const el = $('lv_info'), size = L.frames ? ` · ${L.w}×${L.h} px · ${(+$('mv_pixel-nm').value || 100).toFixed(1)} nm/px` : '';
  const text = L.err ? 'live failed: ' + L.err
    : REC.on ? `● recording ${REC.got}/${REC.n}` + size
    : REC.msg && !L.on ? REC.msg
    : !L.frames ? (L.on ? 'starting…' : '')
    : (L.on ? '● ' + L.fps.toFixed(1) + ' fps' : 'snap') + ' · ' + L.ms.toFixed(1) + ' ms/frame' + size;
  el.textContent = text;
  el.title = text;
  showPerfNote();
}
function expert() { return document.body.dataset.level === 'expert'; }
function showPerfNote() {
  const el = $('lv_perfNote');
  if (!expert() || !L.frames) { el.textContent = ''; return; }
  const bf = $('mv_modality').value === '1', both = self.ISC_MM_EXTRA && self.ISC_MM_EXTRA['light-trans'] === 1 && !bf;
  const lines = [];
  const how = renderPaths(bf, both);
  if (how) lines.push('render: ' + how);
  lines.push(`sample time ${L.t.toFixed(1)} s` + (L.clock !== undefined ? ` · lit here ${L.clock.toFixed(1)} s` : ''));
  if (L.drift && L.drift.some(v => Math.abs(v) >= 0.5)) lines.push(`drift ${L.drift.map(v => v.toFixed(0)).join(', ')} nm`);
  el.innerHTML = lines.map(s => s.replace(/&/g, '&amp;').replace(/</g, '&lt;')).join('<br>');
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
// worker's call, the message to and from it, the display; and the wait that paces frames to the exposure. Every row is
// always there, in a fixed order ("—" when the frame did not have it), so nothing jumps; a phase run more than once in
// the frame shows its count in the row's tooltip. Phases not in the list add up in "other engine phases".
const PHASES = [
  ['fl.total', 'fluorescence, total', 1], ['fl.setup', 'setup', 2], ['fl.make-setup', 'resolve the spec', 2],
  ['fl.configure', 'labels, light path', 2], ['fl.clock-map', 'light history: clock map', 2], ['fl.survey', 'survival survey', 2],
  ['fl.events-query', 'blink events (core)', 2], ['fl.events-sort', 'sort events', 2], ['fl.windows', 'per-dye windows', 2], ['fl.windows-query', 'open windows (core)', 2],
  ['fl.dye-counts', 'dye counts', 2], ['fl.psf-kernels', 'PSF kernels', 2], ['fl.mean-field-scene', 'mean-field scene', 2],
  ['fl.mean-field-entries', 'mean-field map entries', 2], ['fl.mean-field-weights', 'mean-field weights', 2], ['fl.mean-field-render', 'mean-field image', 2],
  ['fl.frame.blinks (batches)', 'blinks (splat / binned)', 2], ['fl.frame.continuous', 'continuous dyes', 2],
  ['fl.frame.noise (batches)', 'camera noise', 2], ['fl.frame.onFrame', 'hand-over', 2],
  ['bf.total', 'BrightField, total', 1], ['bf.scene', 'BrightField scene', 2], ['bf.scene-update', 'BrightField scene update', 2],
  ['bf.image', 'BrightField image', 2], ['bf.frame.noise', 'BrightField noise', 2], ['bf.frame.onFrame', 'BrightField hand-over', 2],
  ['bf.focus-cached', 'BrightField focus (cached)', 2], ['bf.prefetch-image', 'BrightField prefetch', 2],
  ['psf.compute', 'PSF kernel (compute)', 1], ['psf.zernike-planes', 'PSF planes', 2],
  ['psf.normalize+block-sums', 'PSF normalize, block sums', 2], ['psf.memo-hit', 'PSF kernel (memo)', 1],
  ['psf.disk-hit', 'PSF kernel (disk)', 1],
];
const KNOWN = new Set(PHASES.map(r => r[0]));
function showPerf() {
  const out = $('lv_perfOut');
  if (!expert()) { out.innerHTML = ''; return; }
  const P = L.perf || {}, rows = [];
  const ms = v => (v === undefined || v === null || !isFinite(v) ? '—' : v.toFixed(1) + ' ms');
  const pad = ['', 'padding-left:8px', 'padding-left:16px', 'padding-left:24px'];
  const row = (name, v, level, cls, tip) => {
    const a = `class="${cls || ''}" style="${pad[level] || ''}"` + (tip ? ` title="${tip}"` : '');
    rows.push(`<span ${a}>${name}</span><span class="${cls || ''}">${ms(v)}</span>`);
  };
  const ph = (P.profile && P.profile.phases) || {};
  row('frame: request → result', P.roundMs, 0, 'tot');
  row('engine (worker)', P.workerMs, 1);
  let tops = 0, other = 0, anyOther = false;
  for (const [k, name, level] of PHASES) {
    const v = ph[k];
    row(name, v ? v.totalMs : undefined, level + 1, '', v && v.count > 1 ? `${v.count} times in this frame` : '');
    if (v && (k === 'fl.total' || k === 'bf.total')) tops += v.totalMs;   // the PSF runs inside them
  }
  for (const [k, v] of Object.entries(ph)) if (!KNOWN.has(k)) { other += v.totalMs; anyOther = true; }
  row('other engine phases', anyOther ? other : P.profile ? 0 : undefined, 2);
  row('rest of the engine call', P.workerMs !== undefined && P.profile ? Math.max(0, P.workerMs - tops) : undefined, 2);
  row('to and from the worker', P.workerMs !== undefined ? Math.max(0, P.roundMs - P.workerMs) : undefined, 1);
  row('display (image, histogram)', P.paintMs !== undefined ? P.paintMs + (P.histMs || 0) : undefined, 0, 'tot');
  row('wait (paced to the exposure)', L.on ? P.waitMs || 0 : undefined, 0, 'tot');
  out.innerHTML = rows.join('');
}
// the performance box shows at the Expert level (a frame taken there is profiled; a snap fills it)
new MutationObserver(() => { showPerf(); showPerfNote(); if (expert() && !L.on && !REC.on && L.frame && !L.pending) request(); })
  .observe(document.body, { attributes: true, attributeFilter: ['data-level'] });

// Scale bar: a 1-2-5 length near a quarter of the displayed image.
function drawScale() {
  const el = $('lv_scale');
  if (!L.frame) { el.innerHTML = ''; return; }
  const S = $('liveView').clientWidth, dispPx = S / Math.max(L.w, L.h) * ((L.zoom && L.zoom.z) || 1);   // with the zoom
  const pxUm = (+$('mv_pixel-nm').value || 100) / 1000, cssPerUm = dispPx / pxUm;
  const want = S * 0.22 / cssPerUm;
  const nice = [0.1, 0.2, 0.5, 1, 2, 5, 10, 20, 50, 100, 200];
  const len = nice.reduce((a, b) => (Math.abs(b - want) < Math.abs(a - want) ? b : a));
  el.innerHTML = `${len < 1 ? len * 1000 + ' nm' : len + ' µm'}<i style="width:${(len * cssPerUm).toFixed(1)}px"></i>`;
}
window.addEventListener('resize', drawScale);

// ---- Record: the next N frames at the stage position, as fast as the engine goes ----
// The live sample goes on (its light history, drift, noise counter), the frames are not paced to the exposure:
// Record: N frames rendered as fast as the engine goes, saved as one 16-bit TIFF: ImageJ's header (frames, frame
// interval, pixel size) and every setting (the engine spec). By default a fresh sample: one movie job of N frames on
// the live worker (= "Make movie here", = insiliscope_cli --spec with the header's spec), the live sample untouched;
// Record again cancels it (the worker is replaced, so the live sample starts afresh). With "continue the live sample"
// (Advanced): chunks of frames, each one movie on the sample's clock (isc_live_frames; ~2 M pixels per chunk), back to
// back, the last frame of each chunk shown; Record again stops early and saves the frames so far. At most 1 GB.
const REC = L.rec = { on: false, stop: false, live: false, n: 0, got: 0, buf: null, w: 0, h: 0, spec: '', tStart: 0,
  litStart: 0, t0: 0, engineMs: 0, msg: '' };
$('lv_recLive').checked = store.get('recLive', '0') === '1';
$('lv_recLive').addEventListener('change', () => store.set('recLive', $('lv_recLive').checked ? '1' : '0'));
$('lv_recN').value = store.get('recN', '100');
$('lv_recN').addEventListener('change', () => store.set('recN', $('lv_recN').value));
function startRec() {
  if (REC.on && !REC.live) {   // cancel the movie: replace the worker (its live sample goes with it)
    if (L.worker) L.worker.terminate();
    L.worker = null; L.pending = null; L.worldKey = null;
    Object.assign(REC, { on: false, buf: null, msg: 'recording cancelled' });
    syncButtons(); showInfo();
    return;
  }
  if (REC.on) { REC.stop = true; return; }
  closePlayer();
  const n = Math.max(1, Math.round(+$('lv_recN').value || 0)), side = Math.max(1, +$('mv_size').value || 128);
  const max = Math.floor(1e9 / (2 * side * side));
  Object.assign(REC, { on: true, stop: false, live: $('lv_recLive').checked, n: Math.min(n, max), got: 0, buf: null,
    exp: exposureSec(), t0: performance.now(), engineMs: 0, msg: n > max ? `at most ${max} frames of ${side}×${side} px (1 GB)` : '' });
  iscTrack('record', { frames: iscTrack.framesBucket(REC.n), continue: REC.live });
  if (L.on) L.on = false;
  syncButtons();
  if (!L.pending) recNext();   // else after the frame in flight
}
function recMovie() {
  const w = worker();
  if (!w) { L.err = 'the workers are not up yet'; REC.on = false; syncButtons(); showInfo(); return; }
  const job = { type: 'movie', id: ++L.id, spec: movieSpec({ frames: REC.n }), fresh: true, t0: performance.now() };
  L.pending = job;
  w.postMessage(job);
}
function onRecProgress(d) {
  REC.got = Math.min(REC.n, Math.floor((d.frac || 0) * REC.n));
  syncButtons(); showInfo();
}
function onRecMovie(d, job) {
  if (!REC.on) return;
  const px = d.w * d.h, n = Math.min(REC.n, Math.floor(d.frames.length / Math.max(1, px)));
  Object.assign(REC, { w: d.w, h: d.h, spec: d.spec, buf: d.frames, got: n, engineMs: performance.now() - job.t0 });
  L.err = '';
  L.ms = n ? (performance.now() - REC.t0) / n : 0;
  L.frames++;
  draw({ ...d, frames: d.frames.slice((n - 1) * px, n * px) });
  finishRec();
}
function recNext() {
  if (!REC.live) { if (!REC.buf) recMovie(); return; }
  if (REC.stop || REC.got >= REC.n) { finishRec(); return; }
  const side = Math.max(1, +$('mv_size').value || 128);
  request(Math.max(1, Math.min(REC.n - REC.got, Math.floor(2e6 / (side * side)), 500)));
}
function onRecChunk(d, job) {
  const px = d.w * d.h, n = d.n || 1;
  if (!REC.buf) {
    Object.assign(REC, { w: d.w, h: d.h, spec: d.spec, tStart: d.tStart !== undefined ? d.tStart : job.t, litStart: d.clock });
    REC.buf = new Uint16Array(REC.n * px);
  }
  if (d.w !== REC.w || d.h !== REC.h) { REC.msg = 'the image size changed: stopped'; REC.stop = true; }
  else {
    const k = Math.min(n, REC.n - REC.got);
    REC.buf.set(d.frames.subarray(0, k * px), REC.got * px);
    REC.got += k;
    REC.engineMs += d.workerMs || 0;
  }
  L.err = '';
  L.t = d.t; L.clock = d.clock; L.drift = d.drift; L.engine = d.engine; L.paths = d.paths || null;
  const s = (performance.now() - REC.t0) / 1000;
  L.ms = REC.got ? s * 1000 / REC.got : 0;
  L.frames++;
  draw({ ...d, frames: d.frames.slice((n - 1) * px, n * px) });
  syncButtons();
  if (REC.stop || REC.got >= REC.n) finishRec(); else recNext();
}
function finishRec() {
  const n = REC.got, s = (performance.now() - REC.t0) / 1000;
  REC.on = false;
  if (n && REC.buf) {
    REC.msg = `recorded ${n} frames in ${s.toFixed(1)} s, ${(n / s).toFixed(1)} frames/s` +
      (REC.got < REC.n ? ` (stopped at ${n} of ${REC.n})` : '') + (REC.msg ? ' · ' + REC.msg : '');
    openPlayer(n);
  }
  REC.buf = null;
  syncButtons();
}

// ---- a finished recording plays in the live view (looping at the frame interval); Save .tif saves it ----
const PLAY = L.play = { on: false, frames: null, w: 0, h: 0, n: 0, desc: '', i: 0, run: false, timer: 0 };
function openPlayer(n) {
  const exp = REC.exp || exposureSec();   // the exposure the frames were taken at
  const desc = [`ImageJ=1.11a`, `images=${n}`, `frames=${n}`, `unit=micron`, `finterval=${exp}`, `loop=false`,
    ...(REC.live ? [`insiliscope=live recording`, `insiliscope.spec=${REC.spec}`, `insiliscope.version=${self.ISC_VERSION || ''}`, `insiliscope.sample-time-sec=${REC.tStart}`,
      `insiliscope.lit-sec-at-start=${REC.litStart}`, `insiliscope.recorded=${new Date().toISOString()}`,
      'insiliscope.note=the frames continue the live sample (its light history and drift); insiliscope_cli --spec renders a fresh sample']
      : [`insiliscope=recording`, `insiliscope.spec=${REC.spec}`, `insiliscope.version=${self.ISC_VERSION || ''}`, `insiliscope.recorded=${new Date().toISOString()}`,
        'insiliscope.note=a fresh sample (a movie from start-sec, no history); insiliscope_cli --spec renders the same movie'])].join('\n') + '\n';
  closePlayer();
  Object.assign(PLAY, { on: true, frames: REC.buf.subarray(0, n * REC.w * REC.h), w: REC.w, h: REC.h, n, desc, spec: REC.spec,
    exp, pxUm: (+$('mv_pixel-nm').value || 100) / 1000, i: 0, run: n > 1 });
  $('lv_playPos').max = n - 1;
  $('lv_play').hidden = false;
  $('liveView').classList.add('playing');
  L.hiSet = false;
  showPlayFrame(0);
  if (PLAY.run) schedulePlay();
  syncPlay();
}
function showPlayFrame(i) {
  const px = PLAY.w * PLAY.h;
  PLAY.i = i;
  draw({ w: PLAY.w, h: PLAY.h, spec: PLAY.spec, frames: PLAY.frames.subarray(i * px, (i + 1) * px) });
  $('lv_playPos').value = i;
  $('lv_playN').textContent = `${i + 1}/${PLAY.n}`;
}
// Playback in real time at the recording's exposure: each display refresh shows the frame the clock is at (100 frames
// of 1 ms play in 0.1 s; the screen shows the ones its refreshes land on), looping.
function schedulePlay() {
  cancelAnimationFrame(PLAY.timer);
  const t0 = performance.now() - PLAY.i * PLAY.exp * 1000;
  // the clock is performance.now(), not the rAF timestamp: that is the frame's start and can lag by a second when the
  // page is busy (the index went negative or stood still)
  const tick = () => {
    if (!PLAY.on || !PLAY.run) return;
    const k = Math.floor((performance.now() - t0) / (PLAY.exp * 1000)), i = ((k % PLAY.n) + PLAY.n) % PLAY.n;
    if (i !== PLAY.i && !document.hidden) showPlayFrame(i);
    PLAY.timer = requestAnimationFrame(tick);
  };
  PLAY.timer = requestAnimationFrame(tick);
}
function syncPlay() { $('lv_playBtn').textContent = PLAY.run ? '❚❚' : '▶'; }
function closePlayer() {
  cancelAnimationFrame(PLAY.timer);
  Object.assign(PLAY, { on: false, frames: null, run: false });
  $('lv_play').hidden = true;
  $('liveView').classList.remove('playing');
}
$('lv_playBtn').addEventListener('click', () => { PLAY.run = !PLAY.run && PLAY.n > 1; syncPlay(); if (PLAY.run) schedulePlay(); });
$('lv_playPos').addEventListener('input', () => { PLAY.run = false; syncPlay(); cancelAnimationFrame(PLAY.timer); showPlayFrame(+$('lv_playPos').value); });
$('lv_playClose').addEventListener('click', () => { closePlayer(); REC.msg = ''; showInfo(); });
$('lv_playSave').addEventListener('click', () => {
  if (!PLAY.on) return;
  iscTrack('save-tiff', { source: 'record', frames: iscTrack.framesBucket(PLAY.n) });
  const blob = writeTiff16(PLAY.frames, PLAY.w, PLAY.h, PLAY.n, PLAY.desc, PLAY.pxUm);
  const a = document.createElement('a'), name = `insiliscope_live_${PLAY.w}x${PLAY.h}x${PLAY.n}.tif`;
  a.href = URL.createObjectURL(blob);
  a.download = name;
  a.click();
  setTimeout(() => URL.revokeObjectURL(a.href), 30000);
  REC.msg = `saved ${name} (${(blob.size / 1e6).toFixed(1)} MB)`;
  showInfo();
});
// ● Record opens a small box under the buttons for the number of frames (and, Advanced, "continue the live sample");
// its Start (or Enter) records. While recording, ● Record stops.
const recPop = (open) => {
  $('lv_recPop').hidden = !open;
  if (open) { $('lv_recN').focus(); $('lv_recN').select(); }
};
$('lv_rec').addEventListener('click', () => { if (REC.on) startRec(); else recPop($('lv_recPop').hidden); });
$('lv_recGo').addEventListener('click', () => { recPop(false); startRec(); });
$('lv_recPop').addEventListener('keydown', ev => {
  if (ev.key === 'Enter') { ev.preventDefault(); $('lv_recN').dispatchEvent(new Event('change')); recPop(false); startRec(); }
  else if (ev.key === 'Escape') { ev.preventDefault(); recPop(false); $('lv_rec').focus(); }
});
document.addEventListener('pointerdown', ev => {
  if (!$('lv_recPop').hidden && !$('lv_recPop').contains(ev.target) && ev.target !== $('lv_rec')) recPop(false);
});

// ---- buttons ----
// The idle image: "Press ▶ Live", or "Starting..." while live or a snap waits for its first frame.
// "Working..." before the info while the engine works on a snap, a recording or live's first frame (not the frames
// after it: live then runs, the info line says how fast): shown once the job has been out 250 ms, hidden 300 ms after
let busyT = 0, busyWant = false;
function syncBusy() {
  const busy = (!!L.pending && !(L.on && L.lastShown)) || REC.on;
  if (busy === busyWant) return;
  busyWant = busy;
  clearTimeout(busyT);
  busyT = setTimeout(() => { $('lv_busy').hidden = !busyWant; }, busy ? 250 : 300);
}
function syncIdle() {
  syncBusy();
  const starting = (L.on || !!L.pending || L.auto) && !L.frames;
  $('lv_idlePress').hidden = starting; $('lv_idleStart').hidden = !starting;
}
function syncButtons() {
  syncIdle();
  $('lv_live').classList.toggle('on', L.on);
  $('lv_live').innerHTML = L.on ? '<span class="ico">■</span> Stop' : '<span class="ico">▶</span> Live';
  $('lv_rec').classList.toggle('on', REC.on);
  $('lv_rec').textContent = REC.on ? `■ ${REC.got}/${REC.n}` : '● Record';
  for (const id of ['lv_live', 'lv_snap', 'lv_fresh']) $(id).disabled = REC.on;
  showInfo();
  requestDraw();
}
function setLive(on) {
  L.on = on;
  syncButtons();
  if (on) { closePlayer(); L.lastShown = 0; L.fps = 0; REC.msg = ''; request(); }
}
$('lv_live').addEventListener('click', () => { setLive(!L.on); if (L.on) iscTrack('live'); });
// Live starts by itself once the engine is up (not counted as a pick), unless the user pressed Live, Snap or Record
// first; ?live=0 keeps it off (the browser tests, which drive it themselves).
L.auto = !/[?&]live=0(&|$)/.test(location.search);
for (const id of ['lv_live', 'lv_snap', 'lv_rec']) $(id).addEventListener('click', () => { if (L.auto) { L.auto = false; syncIdle(); } }, true);
if (L.auto) {
  const t = setInterval(() => {
    if (!L.auto) { clearInterval(t); return; }
    if (!genWorkers.ok) return;
    clearInterval(t); L.auto = false;
    if (!L.on && !REC.on) setLive(true);
  }, 200);
}
$('lv_fresh').addEventListener('click', () => { L.worldKey = null; L.t = 0; if (!L.on && L.frame) request(); });
$('lv_snap').addEventListener('click', () => { iscTrack('snap'); if (L.on) setLive(false); closePlayer(); L.lastShown = 0; REC.msg = ''; request(); });
document.addEventListener('visibilitychange', () => { if (!document.hidden && L.on && !L.pending) request(); });
window.liveRedraw = () => { paint(); drawScale(); };
})();
