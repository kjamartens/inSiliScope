// SPDX-License-Identifier: BSD-3-Clause
// The viewer's live microscope (the right panel): the camera image at the stage position (= the world view's
// centre), rendered frame after frame by a worker of its own; Snap, Record (the movie player), the focus (Z stage),
// exposure and display range; the splitter between the world view and the panel; the View pop-over.
//
// Loaded after the viewer's main script (web/index.html) and uses its globals: movieSpec, spawnGenWorker,
// genWorkers, view, requestDraw, resizeCanvas, makeMovie, uiKit. A live frame is a `live` job (cli/scope_wasm.cpp
// isc_live_frame: sim::LiveScope, the adapter's live mode, with the sample's illumination history); an engine
// without it (the lab's JS reference) gets single-frame `movie` jobs at an advancing start time instead.
'use strict';
(function () {
const $ = id => document.getElementById(id);
const store = uiKit.store;

// ---- splitter: the right panel's width, 33-50 % of the window ----
const wrap = $('wrap'), splitter = $('splitter');
const LIVE_MIN = 0.33, LIVE_MAX = 0.5;
function setLiveFrac(f, save) {
  f = Math.max(LIVE_MIN, Math.min(LIVE_MAX, f));
  wrap.style.setProperty('--live-w', (100 * f).toFixed(2) + 'vw');
  if (save) store.set('liveFrac', f.toFixed(4));
}
setLiveFrac(+store.get('liveFrac', '0.4') || 0.4, false);
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
  const job = { type: 'live', id: ++L.id, spec, reset, t: L.t, lit: true, t0: performance.now() };
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
  const due = L.lastShown + exposureSec() * 1000, now = performance.now();
  const show = () => {
    const t = performance.now();
    if (L.lastShown) L.fps = 0.8 * L.fps + 0.2 * (1000 / Math.max(1, t - L.lastShown));
    L.lastShown = t;
    L.frames++;
    draw(d);
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
  paint();
  showInfo();
  drawScale();
  requestDraw();
}
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
for (const id of ['lv_lo', 'lv_hi']) $(id).addEventListener('input', () => { $('lv_auto').checked = false; L.lo = +$('lv_lo').value; L.hi = Math.max(L.lo + 1, +$('lv_hi').value); paint(); });
$('lv_auto').addEventListener('change', () => { L.hiSet = false; if (L.frame && $('lv_auto').checked) { const [lo, hi] = autoRange(L.frame); L.lo = lo; L.hi = hi; $('lv_lo').value = lo; $('lv_hi').value = hi; paint(); } });

const MODES = ['dSTORM', 'PALM', 'DNA-PAINT', 'WideField'];
function showInfo() {
  const el = $('lv_info');
  if (L.err) { el.textContent = 'live failed: ' + L.err; return; }
  if (!L.frames) { el.textContent = L.on ? 'starting…' : ''; return; }
  const bf = $('mv_modality').value === '1', both = self.ISC_MM_EXTRA && self.ISC_MM_EXTRA['light-trans'] === 1 && !bf;
  const lines = [(L.on ? '● ' + L.fps.toFixed(1) + ' fps' : 'snap') + ' · ' + Math.round(L.ms) + ' ms/frame',
    `${L.w}×${L.h} px · ${bf ? 'BrightField' : both ? 'fluorescence + BrightField' : 'fluorescence'} · z ${(+$('mv_z').value).toFixed(2)} µm`,
    `t ${L.t.toFixed(1)} s` + (L.clock !== undefined ? ` · lit here ${L.clock.toFixed(1)} s` : '') + ` · ${L.lo}–${L.hi} ADU`];
  if (L.drift && L.drift.some(v => Math.abs(v) >= 0.5)) lines.push(`drift ${L.drift.map(v => v.toFixed(0)).join(', ')} nm`);
  if (L.engine === 'movie') lines.push('(no sample history in this engine)');
  el.textContent = lines.join('\n');
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
