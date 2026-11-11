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

// ---- splitter: the right panel's width, 33-72 % of the window (50 % by default) ----
const wrap = $('wrap'), splitter = $('splitter');
const LIVE_MIN = 0.33, LIVE_MAX = 0.72;
function setLiveFrac(f, save) {
  f = Math.max(LIVE_MIN, Math.min(LIVE_MAX, f));
  wrap.style.setProperty('--live-w', (100 * f).toFixed(2) + 'vw');
  if (save) store.set('liveWidth', f.toFixed(4));   // (was liveFrac, default 0.4: a new key so the new default shows)
}
setLiveFrac(+store.get('liveWidth', '0.5') || 0.5, false);
resizeCanvas();   // the world view was measured at the default width (the main script ran first): measure it again
splitter.addEventListener('pointerdown', e => {
  if (e.button !== 0) return;
  e.preventDefault();
  splitter.classList.add('drag');
  splitter.setPointerCapture(e.pointerId);
  // the fraction is the panel's width with the settings open: a folded column adds back what it gave up
  const shut = wrap.classList.contains('setShut') ? parseFloat(getComputedStyle(wrap).getPropertyValue('--set-w')) - $('setCol').getBoundingClientRect().width : 0;
  const move = ev => { setLiveFrac((innerWidth - ev.clientX + shut) / innerWidth, true); resizeCanvas(); };
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

// A settings group in the column (Illumination) shows only when one of its rows does at the current level.
function syncHostBoxes() {
  for (const id of ['lv_illumHost']) {
    const box = $(id);
    if (box) box.hidden = ![...box.querySelectorAll('.opt')].some(e => getComputedStyle(e).display !== 'none');
  }
}
// A click on a box's title folds it (Focus, Acquisition, Display, Performance; the settings groups fold as details).
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
// The stage box: the joystick moves the field of view (the world view's centre), the focus wheel beside it steps Z,
// its height under it. A right-click on either opens a box with a typed position and the step.
const xyStep = () => Math.max(0.001, +$('lv_xyStep').value || 1);
const moveXY = (dx, dy) => { view.cx += dx * xyStep(); view.cy += dy * xyStep(); noteInteraction(); requestDraw(); };
$('lv_xyStep').value = store.get('xyStep', '1');
$('lv_xyStep').addEventListener('input', () => store.set('xyStep', $('lv_xyStep').value));
function holdRepeat(btn, act) {
  let t = 0;
  const stop = () => clearTimeout(t);
  btn.addEventListener('pointerdown', ev => {
    if (ev.button > 0) return;   // (not the right button: that opens the step box)
    if (ev.pointerType === 'touch') ev.preventDefault();
    act();
    const again = ms => { t = setTimeout(() => { act(); again(90); }, ms); };
    again(400);
  });
  for (const e of ['pointerup', 'pointerleave', 'pointercancel']) btn.addEventListener(e, stop);
  btn.addEventListener('keydown', ev => { if (ev.key === 'Enter' || ev.key === ' ') { ev.preventDefault(); act(); } });
}
// ---- the joystick: the knob dragged from the middle moves the stage at a speed growing with the square of its
// distance (full: JOY_STEPS steps per second), in its direction (right = the field of view to the right, as ▶); let
// go, it springs back and the stage stops. A click near an edge (no drag) moves one step that way (the diagonals
// too); the arrow keys on it one step. Shift: 10 steps a click or a key, 10x the speed of a drag.
{
  const JOY_STEPS = 12, joy = $('lv_joy'), knob = joy.querySelector('.jk');
  let hold = null, raf = 0, shift = false;
  for (const e of ['keydown', 'keyup']) window.addEventListener(e, ev => { shift = ev.shiftKey; });
  const vec = ev => {
    const r = joy.getBoundingClientRect(), R = r.width / 2;
    let x = (ev.clientX - r.left - R) / R, y = (ev.clientY - r.top - R) / R;
    const d = Math.hypot(x, y);
    if (d > 1) { x /= d; y /= d; }
    return { x, y, R };
  };
  const tick = t => {
    if (!hold) return;
    const dt = Math.min(0.1, (t - hold.t) / 1000), d = Math.hypot(hold.x, hold.y);
    hold.t = t;
    if (d > 0.12) { const v = JOY_STEPS * d * (shift ? 10 : 1); moveXY(hold.x * v * dt, hold.y * v * dt); }   // speed JOY_STEPS d^2 (a dead zone in the middle)
    raf = requestAnimationFrame(tick);
  };
  joy.addEventListener('pointerdown', ev => {
    if (ev.button > 0) return;
    ev.preventDefault();
    joy.focus({ preventScroll: true });   // (then the arrow keys work on it)
    joy.setPointerCapture(ev.pointerId);
    shift = ev.shiftKey;
    const v = vec(ev);
    hold = { x0: ev.clientX, y0: ev.clientY, x: 0, y: 0, moved: 0, click: v, t: performance.now() };
    joy.classList.add('held');
    raf = requestAnimationFrame(tick);
  });
  joy.addEventListener('pointermove', ev => {
    if (!hold) return;
    shift = ev.shiftKey;
    hold.moved = Math.max(hold.moved, Math.hypot(ev.clientX - hold.x0, ev.clientY - hold.y0));
    if (hold.moved < 3) return;
    // the knob follows the drag from where it was pressed
    const r = joy.getBoundingClientRect(), R = r.width / 2;
    let x = (ev.clientX - hold.x0) / R, y = (ev.clientY - hold.y0) / R;
    const d = Math.hypot(x, y);
    if (d > 1) { x /= d; y /= d; }
    hold.x = x; hold.y = y;
    knob.style.transform = `translate(${x * (R - 10)}px,${y * (R - 10)}px)`;
  });
  const end = () => {
    if (!hold) return;
    if (hold.moved < 3) {
      const { x, y } = hold.click;
      const k = shift ? 10 : 1;
      if (Math.hypot(x, y) > 0.4) moveXY(k * (Math.abs(x) > 0.38 ? Math.sign(x) : 0), k * (Math.abs(y) > 0.38 ? Math.sign(y) : 0));
    }
    hold = null; cancelAnimationFrame(raf);
    joy.classList.remove('held'); knob.style.transform = '';
  };
  joy.addEventListener('pointerup', end); joy.addEventListener('pointercancel', end);
  joy.addEventListener('keydown', ev => {
    const d = { ArrowLeft: [-1, 0], ArrowRight: [1, 0], ArrowUp: [0, -1], ArrowDown: [0, 1] }[ev.key];
    if (!d) return;
    ev.preventDefault(); ev.stopPropagation();
    const k = ev.shiftKey ? 10 : 1;
    moveXY(k * d[0], k * d[1]);
  });
}
{
  const fx = $('lv_x'), fy = $('lv_y'), fmt = v => String(+v.toFixed(2));
  const show = () => {
    if (document.activeElement !== fx && fx.value !== fmt(view.cx)) fx.value = fmt(view.cx);
    if (document.activeElement !== fy && fy.value !== fmt(view.cy)) fy.value = fmt(view.cy);
  };
  for (const [f, k] of [[fx, 'cx'], [fy, 'cy']]) f.addEventListener('change', () => {
    if (f.value === '' || !isFinite(+f.value)) { show(); return; }
    view[k] = +f.value; noteInteraction(); requestDraw();
  });
  show();
  setInterval(() => { if (!document.hidden) show(); }, 200);
}
// ---- the focus wheel #mFocus (beside the joystick; on a phone, web/live/mobile.js, alone in the Focus box): one step of
// the step size per KNOB_PX dragged, a fast drag several; a scroll notch one step; ▲ / ▼ (− / + on a phone) step once
// and repeat while held. On a wide window a thumbwheel seen edge-on: its ticks move with the pointer (drag up = focus
// up), a click above or below its line steps once. On a phone a ruler (values grow to the right: swipe left to focus
// up), as before. Two fingers up or down on the live image move the focus too (below).
{
  const KNOB_PX = 12, knob = $('mFocus'), kcv = $('mKnob');
  const phone = () => document.body.classList.contains('mobile');
  const zStep = () => Math.max(0.001, +$('lv_zStep').value || 0.1);
  const draw = () => {
    const dpr = devicePixelRatio, W = Math.round(kcv.clientWidth * dpr), H = Math.round(kcv.clientHeight * dpr);
    if (!W || !H) return;
    if (kcv.width !== W || kcv.height !== H) { kcv.width = W; kcv.height = H; }
    const g = kcv.getContext('2d');
    const z = +$('mv_z').value || 0, st = zStep(), at = z / st, sp = KNOB_PX * dpr;
    g.clearRect(0, 0, W, H);
    g.fillStyle = iscTheme.tok('--color-text-muted');
    if (!phone()) {
      // the wheel's ticks on a cylinder (radius = half the height): spaced KNOB_PX at the line, closer and fainter
      // towards its edges; no numbers (the height is under it)
      const cy = H / 2, r = H / 2, da = sp / r;
      for (let k = Math.floor(at - Math.PI / 2 / da) - 1; k <= Math.ceil(at + Math.PI / 2 / da) + 1; k++) {
        const a = (k - at) * da;
        if (Math.abs(a) >= Math.PI / 2) continue;
        const len = (k % 10 === 0 ? 0.8 : k % 5 === 0 ? 0.55 : 0.35) * W;
        g.globalAlpha = 0.2 + 0.8 * Math.cos(a);
        g.fillRect(Math.round((W - len) / 2), Math.round(cy + r * Math.sin(a)), Math.round(len), dpr);
      }
      g.globalAlpha = 1;
      g.fillStyle = iscTheme.tok('--color-accent');
      g.fillRect(0, Math.round(cy - dpr), W, 2 * dpr);
    } else {
      g.font = `${10 * dpr}px system-ui,sans-serif`; g.textAlign = 'center'; g.textBaseline = 'top';
      const cx = W / 2;
      for (let k = Math.floor(at - cx / sp) - 1; k <= Math.ceil(at + cx / sp) + 1; k++) {
        const x = Math.round(cx + (k - at) * sp) + 0.5, major = k % 10 === 0, mid = k % 5 === 0;
        g.fillRect(x - 0.5 * dpr, H - (major ? 16 : mid ? 11 : 7) * dpr, dpr, (major ? 16 : mid ? 11 : 7) * dpr);
        if (major) g.fillText(+(k * st).toFixed(3) + '', x, 2 * dpr);
      }
      g.fillStyle = iscTheme.tok('--color-accent');
      g.fillRect(cx - dpr, 0, 2 * dpr, H);
    }
    $('mZ').firstChild.textContent = z.toFixed(st < 0.01 ? 3 : 2);   // (the unit: a span shown on a phone)
  };
  let drag = null;
  kcv.addEventListener('pointerdown', ev => {
    if (ev.button !== 0) return;
    drag = { x: ev.clientX, y: ev.clientY, t: ev.timeStamp, acc: 0, moved: 0 };
    kcv.setPointerCapture(ev.pointerId);
  });
  kcv.addEventListener('pointermove', ev => {
    if (!drag) return;
    const d = phone() ? ev.clientX - drag.x : ev.clientY - drag.y, dt = Math.max(1, ev.timeStamp - drag.t), v = Math.abs(d) / dt;   // px per ms
    const gain = v > 0.6 ? Math.min(8, 1 + (v - 0.6) * 6) : 1;   // a fast drag moves further
    drag.x = ev.clientX; drag.y = ev.clientY; drag.t = ev.timeStamp;
    drag.acc -= d * gain; drag.moved += Math.abs(d);
    while (Math.abs(drag.acc) >= KNOB_PX) { const s = Math.sign(drag.acc); stepZ(s); drag.acc -= s * KNOB_PX; }
  });
  kcv.addEventListener('pointerup', ev => {
    // a click (no drag) on the wheel: one step towards that side of its line
    if (drag && drag.moved < 3 && !phone()) { const r = kcv.getBoundingClientRect(); stepZ(ev.clientY < r.top + r.height / 2 ? 1 : -1); }
    drag = null;
  });
  kcv.addEventListener('pointercancel', () => { drag = null; });
  kcv.addEventListener('wheel', ev => { ev.preventDefault(); stepZ(ev.deltaY < 0 ? 1 : -1); }, { passive: false });
  for (const b of knob.querySelectorAll('button')) holdRepeat(b, () => stepZ(+b.dataset.dir));
  knob.addEventListener('contextmenu', ev => { if (phone()) ev.preventDefault(); });   // (a long press would open it)
  for (const e of ['input', 'change']) document.addEventListener(e, ev => { if (/^(mv_z|lv_z|lv_zStep)$/.test(ev.target.id || '')) draw(); }, true);
  window.addEventListener('isc-theme', draw);
  new ResizeObserver(draw).observe(kcv);
  draw();
}
// ---- the right-click boxes: on the joystick x, y and the XY step; on the focus wheel z and the Z step; on the level bar
// the white and black levels (a phone shows the Z step under its ruler and opens none: a long press there is the wheel's)
{
  const pops = [$('lv_xyPop'), $('lv_zPop'), $('lv_dispPop')];
  const close = () => { for (const p of pops) p.hidden = true; };
  const open = (pop, ev) => {
    ev.preventDefault();
    if (document.body.classList.contains('mobile')) return;
    close();
    pop.hidden = false;
    // at the pointer (from the keyboard: at the control), kept in the window
    let x = ev.clientX, y = ev.clientY;
    if (!x && !y) { const r = ev.target.getBoundingClientRect(); x = r.left; y = r.bottom; }
    // (fixed, but measured: an ancestor may hold fixed boxes)
    pop.style.left = pop.style.top = '0px';
    const o = pop.getBoundingClientRect(), w = o.width, h = o.height;
    pop.style.left = Math.max(4, Math.min(x, innerWidth - w - 4)) - o.left + 'px';
    pop.style.top = (y + 4 + h <= innerHeight - 4 ? y + 4 : Math.max(4, y - h - 4)) - o.top + 'px';
    const f = pop.querySelector('[id$=Step]') || pop.querySelector('input');
    f.focus({ preventScroll: true }); f.select();
  };
  $('lv_joy').addEventListener('contextmenu', ev => open(pops[0], ev));
  $('mFocus').addEventListener('contextmenu', ev => open(pops[1], ev));
  $('lv_lvl').addEventListener('contextmenu', ev => open(pops[2], ev));
  document.addEventListener('pointerdown', ev => { if (!pops.some(p => !p.hidden && p.contains(ev.target))) close(); }, true);
  for (const p of pops) p.addEventListener('keydown', ev => { if (ev.key === 'Escape' || ev.key === 'Enter') { ev.preventDefault(); ev.target.dispatchEvent(new Event('change', { bubbles: true })); close(); } });
}
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
L.stepZ = stepZ;
function worker() {
  if (L.worker) return L.worker;
  if (!genWorkers.ok) return null;
  L.worker = spawnGenWorker({ mt: true });   // (the threaded module where the page is cross-origin isolated)
  L.worker.onmessage = e => onResult(e.data);
  // a failure here stops live only (the shared handler would move every worker to the main thread)
  L.worker.onerror = e => { e.preventDefault(); L.err = workerErrorText(e.message || 'the live worker failed'); L.pending = null; L.worker.terminate(); L.worker = null; L.worldKey = null; setLive(false); };
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
  // (a recording chunk: its mean-field bleach basis its own, so its frames are a function of its file: fresh-basis)
  const spec = movieSpec(n ? { frames: 1, 'fresh-basis': 1 } : { frames: 1 });
  const wk = worldKey(spec);
  const reset = wk !== L.worldKey;
  if (reset) { L.worldKey = wk; L.t = 0; L.log = []; }
  if (n && !REC.log) REC.log = L.log.map(r => ({ ...r }));   // the light before the Record (its settings file)
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
  L.pattern = d.pattern || null;
  L.engine = d.engine;
  L.paths = d.paths || null;
  L.perf = { roundMs: L.ms, workerMs: d.workerMs, profile: d.profile || null, memory: d.memory || null };
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
// A recording's ground truth on a worker of its own, closed when done: a fresh heap (on the live worker, beside the
// live sample's caches, a WideField ground truth took the heap past its 4 GB: an abort, and the box waited on), all of
// it freed after. Two jobs: gt-run (its settings file rendered again with the sink on, kept in the worker) and gt-files.
// The run starts as the Save box opens with Ground truth ticked (L.gtWarm), so the sample, the PSF and the frames are
// made while the box is read; Save takes it when it fits (the same file and dye list, rows kept from at most the
// threshold: the same files as a run made at Save, isc_scope_gt_run), else starts another. As the run ends it makes
// the files for the options the box shows then (dyes.csv and the stacks take seconds), which Save takes when they are
// still the options; else the kept run makes them for the new ones. Live pauses while one runs.
const GT = { w: null, key: null, run: null, files: null, pre: null, progress: null, last: null, wasOn: false };
const deferred = () => { let resolve, reject; const p = new Promise((a, b) => { resolve = a; reject = b; }); p.catch(() => {}); return { p, resolve, reject }; };
function gtStop() {
  const { w, run, files } = GT;
  Object.assign(GT, { w: null, key: null, run: null, files: null, pre: null, progress: null, last: null });
  if (w) w.terminate();
  const e = new Error('the ground truth was cancelled');
  if (run) run.reject(e);
  if (files) files.reject(e);
  if (GT.wasOn) { GT.wasOn = false; setLive(true); }
}
function gtRun(text, minPhotons, dyeCsv, filesFor) {
  gtStop();
  if (!genWorkers.ok) return false;
  if (L.on) { L.on = false; syncButtons(); GT.wasOn = true; }
  const w = spawnGenWorker(), run = deferred();
  Object.assign(GT, { w, key: { text, minPhotons, dyeCsv }, run });
  const fail = msg => {
    const e = new Error(workerErrorText(msg));
    run.reject(e);
    if (GT.files) GT.files.reject(e);
    GT.key = null;   // (a later Save starts afresh)
  };
  w.onmessage = e => {
    const d = e.data;
    if (GT.w !== w) return;
    if (d.type === 'movie-progress') { GT.last = [d.frac || 0, d.stage, d.info]; if (GT.progress) GT.progress(...GT.last); }
    else if (d.type === 'gt-run') {
      if (d.error) { fail(d.error); return; }
      if (d.files) GT.pre = { for: d.filesFor, files: d.files };
      run.resolve();
    }
    else if (d.type === 'gt-files' && GT.files) { if (d.error) fail(d.error); else GT.files.resolve(d.files); }
  };
  w.onerror = e => { e.preventDefault(); if (GT.w === w) fail(e.message || 'the ground truth\'s worker failed'); };
  w.postMessage({ type: 'gt-run', id: 1, text, minPhotons, dyeCsv, files: filesFor || null });
  return true;
}
// Whether the run k gives the files of (text, the threshold min, dyeCsv): a run with the dye list serves one without
// (isc_scope_gt_files).
const gtFits = (k, text, min, dyeCsv) => !!k && k.text === text && k.minPhotons <= min && (k.dyeCsv || !dyeCsv);
// Whether made files are those of the options o = {min, vol, dyeCsv}.
const gtSameOpts = (a, o) => !!a && a.min === o.min && a.vol[0] === o.vol[0] &&
  a.vol[1] === o.vol[1] && !!a.dyeCsv === !!o.dyeCsv;
// The Save box: a run for these options now (kept if the one there fits; filesFor: the files to make as it ends);
// L.gtDrop: none.
L.gtWarm = (text, minPhotons, dyeCsv, filesFor) => { if (!gtFits(GT.key, text, minPhotons, dyeCsv)) gtRun(text, minPhotons, dyeCsv, filesFor); };
L.gtDrop = gtStop;
// opt = {text, min (photons in a blink's 500 nm core), vol: [xy, z], dyeCsv}; progress(fraction, stage, info):
// isc_scope_gt's stages (a run
// already on its way reports where it is). Resolves with the files, [[name, bytes], ...]; a failure (out of memory
// too) rejects. The worker is closed after.
L.groundTruth = async (opt, progress) => {
  const min = opt.min;
  if (!gtFits(GT.key, opt.text, min, opt.dyeCsv) && !gtRun(opt.text, min, opt.dyeCsv)) throw new Error('the workers are not up yet');
  const { w, run } = GT;
  GT.progress = progress || null;
  if (progress && GT.last) progress(...GT.last);
  try {
    await run.p;
    if (GT.w !== w) throw new Error('the ground truth was cancelled');
    if (GT.pre && gtSameOpts(GT.pre.for, opt)) return GT.pre.files;   // made as the run ended
    GT.pre = null;   // (let them go before the new ones come)
    const files = GT.files = deferred();
    w.postMessage({ type: 'gt-files', id: 2, min: opt.min, vol: opt.vol, dyeCsv: opt.dyeCsv });
    return await files.p;
  } finally { if (GT.w === w) gtStop(); }
};
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

// ---- the live histogram, upright (the display's level bar): log counts, the display range over it ----
// The axes move slowly: the value axis spans the darkest to the brightest pixel of the frames of the last 10 s (dark at
// the bottom), the count axis the tallest bar of those frames, so a blink or a dim frame does not rescale it. Beside
// it the grey scale the range gives (#lv_lut) and a handle per level with its number (#lv_loTag, #lv_hiTag), kept
// apart when the levels are close. On a phone the bar lies down (dark at the left, the handles under it).
const hc = $('lv_hist'), hg = hc.getContext('2d'), NB = 128, HIST_KEEP_MS = 10000, TAG_H = 12, TAG_W = 44;
const lying = () => document.body.classList.contains('mobile');
let hist = null, hMin = 0, hMax = 1, hTop = 0;
const hRecent = [];   // {t, mn, mx, top} per frame shown
window.addEventListener('isc-theme', () => { if (L.frame) drawHist(); });   // the theme's colours (web/design/theme.js)
new ResizeObserver(() => drawHist()).observe(hc);
function drawHist(newFrame) {
  const fr = L.frame;
  const W = Math.max(1, Math.round(hc.clientWidth * devicePixelRatio)), H = Math.max(1, Math.round(hc.clientHeight * devicePixelRatio));
  if (hc.width !== W || hc.height !== H) { hc.width = W; hc.height = H; }
  hg.clearRect(0, 0, W, H);
  $('lv_lvl').classList.toggle('empty', !fr);
  if (!fr) { $('lv_lut').style.background = ''; return; }
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
  const accent = iscTheme.tok('--color-accent'), muted = iscTheme.tok('--color-text-muted');
  const frac = v => (v - hMin) / (hMax - hMin), yOf = v => H - frac(v) * H;
  const pl = (100 * frac(L.lo)).toFixed(2), ph = (100 * frac(L.hi)).toFixed(2);
  const set = (id, pos, v) => {
    const t = $(id);
    t.style.top = lying() ? '' : pos.toFixed(1) + 'px'; t.style.left = lying() ? pos.toFixed(1) + 'px' : '';
    t.lastChild.textContent = String(Math.round(v));
  };
  if (lying()) {   // the same, lying down: values along x, the bars up from the bottom, the handles under the bar
    const xl = frac(L.lo) * W, xh = frac(L.hi) * W, bw = W / NB;
    hg.fillStyle = muted; hg.globalAlpha = 0.12; hg.fillRect(Math.max(0, xl), 0, Math.max(1, Math.min(W, xh) - Math.max(0, xl)), H);
    hg.globalAlpha = 1;
    for (let b = 0; b < NB; b++) { const hh = top ? (hist[b] / top) * (H - 2) : 0; hg.fillRect(b * bw, H - hh, Math.max(1, bw - 0.5), hh); }
    hg.fillStyle = accent;
    const lw = Math.max(1, Math.round(devicePixelRatio));
    for (const x of [xl, xh]) if (x >= -1 && x <= W + 1) hg.fillRect(Math.round(x - lw / 2), 0, lw, H);
    $('lv_lut').style.background = `linear-gradient(to right, #000 ${pl}%, #fff ${ph}%)`;   // design:allow (the image's own grey scale)
    const Wc = hc.clientWidth, cl = v => Math.min(Wc, Math.max(0, Wc * frac(v)));
    let al = cl(L.lo), ah = cl(L.hi);
    if (ah - al < TAG_W) { const m = (al + ah) / 2; al = m - TAG_W / 2; ah = m + TAG_W / 2; }
    al = Math.max(TAG_W / 2, Math.min(Wc - 1.5 * TAG_W, al)); ah = Math.min(Wc - TAG_W / 2, Math.max(al + TAG_W, ah));
    set('lv_loTag', al, L.lo); set('lv_hiTag', ah, L.hi);
    return;
  }
  // the display range (black .. white) as a shaded band, then the bars (from the left) and the two markers
  const yl = yOf(L.lo), yh = yOf(L.hi);
  hg.fillStyle = muted; hg.globalAlpha = 0.12; hg.fillRect(0, Math.max(0, yh), W, Math.max(1, Math.min(H, yl) - Math.max(0, yh)));
  hg.globalAlpha = 1; hg.fillStyle = muted;
  const bh = H / NB;
  for (let b = 0; b < NB; b++) {
    const ww = top ? (hist[b] / top) * (W - 2) : 0;
    hg.fillRect(0, H - (b + 1) * bh, ww, Math.max(1, bh - 0.5));
  }
  hg.fillStyle = accent;
  const lw = Math.max(1, Math.round(devicePixelRatio));
  for (const y of [yl, yh]) if (y >= -1 && y <= H + 1) hg.fillRect(0, Math.round(y - lw / 2), W, lw);
  // the grey scale shown: black below the black level, white above the white level
  $('lv_lut').style.background = `linear-gradient(to top, #000 ${pl}%, #fff ${ph}%)`;   // design:allow (the image's own grey scale)
  // the handles: centred on their levels, inside the bar, at least a handle's height apart
  const Hc = hc.clientHeight, cl = v => Math.min(Hc, Math.max(0, Hc * (1 - frac(v))));
  let tl = cl(L.lo) - TAG_H / 2, th = cl(L.hi) - TAG_H / 2;
  if (tl - th < TAG_H) { const m = (tl + th) / 2; th = m - TAG_H / 2; tl = m + TAG_H / 2; }
  th = Math.max(0, Math.min(Hc - 2 * TAG_H, th)); tl = Math.max(th + TAG_H, Math.min(Hc - TAG_H, tl));
  set('lv_loTag', tl, L.lo); set('lv_hiTag', th, L.hi);
}
// Dragging on the bar moves the nearer level, on a handle that handle's (the display range by hand: auto off).
$('lv_lvl').addEventListener('pointerdown', ev => {
  if (!L.frame || ev.button > 0) return;
  ev.preventDefault();
  const lvl = $('lv_lvl');
  lvl.setPointerCapture(ev.pointerId);
  const r = hc.getBoundingClientRect(), valAt = e => Math.round(hMin + (lying() ? (e.clientX - r.left) / r.width : 1 - (e.clientY - r.top) / r.height) * (hMax - hMin));
  const tag = ev.target.closest('.tag');
  const which = tag ? (tag.id === 'lv_loTag' ? 'lo' : 'hi') : Math.abs(valAt(ev) - L.lo) <= Math.abs(valAt(ev) - L.hi) ? 'lo' : 'hi';
  const at = tag ? valAt(ev) - L[which] : 0;   // (a handle keeps its offset from the pointer)
  const move = e => {
    const v = valAt(e) - at;
    if (which === 'lo') L.lo = Math.min(v, L.hi - 1); else L.hi = Math.max(v, L.lo + 1);
    $('lv_auto').checked = false;
    $('lv_lo').value = L.lo; $('lv_hi').value = L.hi;
    paint(); drawHist();
  };
  const up = () => { lvl.removeEventListener('pointermove', move); lvl.removeEventListener('pointerup', up); };
  if (!tag) move(ev);
  lvl.addEventListener('pointermove', move); lvl.addEventListener('pointerup', up);
});
// (the box's title folds it; a click on "auto" there only ticks it)
$('lv_auto').closest('label').addEventListener('click', ev => ev.stopPropagation());
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
  $('lv_xyStep').value = '1'; $('lv_xyStep').dispatchEvent(new Event('input'));
  $('lv_auto').checked = true; $('lv_auto').dispatchEvent(new Event('change'));
  setLiveFrac(0.5, true); resizeCanvas(); drawScale();   // the panel's width (the splitter) too
});
$('lv_auto').addEventListener('change', () => { L.hiSet = false; if (L.frame && $('lv_auto').checked) { const [lo, hi] = autoRange(L.frame); L.lo = lo; L.hi = hi; $('lv_lo').value = lo; $('lv_hi').value = hi; paint(); drawHist(); } });

const MODES = ['dSTORM', 'PALM', 'DNA-PAINT', 'WideField'];
// One short line, always: the speed alone (fps and ms per frame; a snap the ms), or an error. The rest -- the image and
// pixel size, the pattern state, the recording's message (frames, its saved file) -- is in its tooltip. How the frame
// was rendered, the drift and the sample time are in the performance box (Expert).
function showInfo() {
  syncIdle();
  const el = $('lv_info'), ms = L.ms.toFixed(1) + ' ms/frame', fps = L.ms > 0 ? (1000 / L.ms).toFixed(1) + ' fps · ' : '';
  const text = L.err ? 'live failed: ' + L.err
    : REC.on ? (REC.live && REC.got ? fps + ms : '')   // a fresh sample's movie: its count is on the button
    : REC.msg && !L.on ? (L.ms > 0 ? fps + ms : '')
    : !L.frames ? (L.on ? 'starting…' : '')
    : L.on ? L.fps.toFixed(1) + ' fps · ' + ms
    : ms;
  const more = [L.frames ? `${L.w}×${L.h} px · ${(+$('mv_pixel-nm').value || 100).toFixed(1)} nm/px` : '',
    L.pattern ? `pattern ${L.pattern.frame % L.pattern.period + 1}/${L.pattern.period}` : '',
    REC.on ? `recording ${REC.got}/${REC.n}` : REC.msg && !L.on ? REC.msg : ''].filter(Boolean);
  el.textContent = text;
  el.title = [text, ...more].filter(Boolean).join('\n');
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
  if (L.pattern) lines.push(`pattern: angle ${L.pattern.angle}, phase ${L.pattern.phase} (frame ${L.pattern.frame % L.pattern.period} of ${L.pattern.period})`);
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
  ['psf.normalize', 'PSF normalize', 2], ['psf.block-sums', 'PSF block sums', 2], ['psf.memo-hit', 'PSF kernel (memo)', 1],
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
  // Memory (MemoryReport.h): the live worker's heap (it never shrinks: its high-water mark) and what the
  // engine's caches hold, the largest owners; a block of its own under the times (its rows come and go with the owners).
  const M = P.memory, mem = [];
  if (M) {
    const mb = v => (v >= 1000 ? (v / 1024).toFixed(2) + ' GB' : v.toFixed(0) + ' MB');
    mem.push(`<span class="tot" title="The live worker's WASM heap: it never shrinks, so this is its high-water mark">memory: worker heap</span><span class="tot">${mb(M.heapMB)}</span>`);
    mem.push(`<span style="padding-left:8px">engine caches</span><span>${mb(M.totalMB)}</span>`);
    for (const [name, v] of M.owners.slice(0, 6)) if (v >= 1)
      mem.push(`<span style="padding-left:16px">${name.replace(/&/g, '&amp;').replace(/</g, '&lt;')}</span><span>${mb(v)}</span>`);
  }
  out.innerHTML = rows.join('');
  $('lv_memOut').innerHTML = mem.join('');
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
// interval, pixel size) and every setting (the engine spec). By default a fresh sample: one movie job of N frames from
// the box's start time (default 0 s) on the live worker (= "Make movie here", = insiliscope_cli --spec with the header's spec), the live sample untouched;
// Record again cancels it (the worker is replaced, so the live sample starts afresh). With "continue the live sample"
// (Advanced): chunks of frames, each one movie on the sample's clock (isc_live_frames; ~2 M pixels per chunk), back to
// back, the last frame of each chunk shown; Record again stops early and saves the frames so far. At most 1 GB.
const REC = L.rec = { on: false, stop: false, live: false, n: 0, got: 0, buf: null, w: 0, h: 0, spec: '', tStart: 0,
  litStart: 0, t0: 0, engineMs: 0, msg: '', now: null, log: null, chunks: [] };
// the fresh sample's time at its first frame (the movie's start-sec; 0 = the light comes on): not the Start time
// setting, which live keeps; hidden with "continue the live sample", which runs on from the live sample's time
const syncRecT = () => { $('lv_recTRow').hidden = $('lv_recLive').checked; };
$('lv_recLive').checked = store.get('recLive', '0') === '1';
$('lv_recLive').addEventListener('change', () => { store.set('recLive', $('lv_recLive').checked ? '1' : '0'); syncRecT(); });
syncRecT();
$('lv_recN').value = store.get('recN', '100');
$('lv_recN').addEventListener('change', () => store.set('recN', $('lv_recN').value));
$('lv_recT').value = store.get('recT', '0');
$('lv_recT').addEventListener('change', () => store.set('recT', $('lv_recT').value));
const recStartSec = () => Math.max(0, +$('lv_recT').value || 0);
function startRec() {
  if (REC.on && !REC.live) {   // cancel the movie: replace the worker (its live sample goes with it)
    if (L.worker) L.worker.terminate();
    L.worker = null; L.pending = null; L.worldKey = null;
    gtStop();   // (its ground truth's run)
    Object.assign(REC, { on: false, buf: null, msg: 'recording cancelled' });
    syncButtons(); showInfo();
    return;
  }
  if (REC.on) { REC.stop = true; return; }
  closePlayer();
  const n = Math.max(1, Math.round(+$('lv_recN').value || 0)), side = Math.max(1, +$('mv_size').value || 128);
  const max = Math.floor(1e9 / (2 * side * side));
  Object.assign(REC, { on: true, stop: false, live: $('lv_recLive').checked, n: Math.min(n, max), got: 0, buf: null,
    exp: exposureSec(), t0: performance.now(), engineMs: 0, msg: n > max ? `at most ${max} frames of ${side}×${side} px (1 GB)` : '',
    log: null, chunks: [], pxUm: (+$('mv_pixel-nm').value || 100) / 1000 });
  // the settings as the Record begins (its file: web/live/settings_file.js recordFile)
  // (before the engine's option list has come, its values would be empty: the spec kept, the values made at the end)
  const over = REC.live ? { frames: REC.n } : { frames: REC.n, 'start-sec': recStartSec() };
  REC.nowArgs = [over, movieSpec(over)];
  REC.now = self.iscSettings && self.ISC_ENGINE_OPTIONS ? self.iscSettings.current(...REC.nowArgs) : null;
  // A fresh sample's settings file is known now (a sample continued is known at its end); kept for the player.
  REC.file = null;
  try { if (!REC.live && REC.now) REC.file = self.iscSettings.recordFile({ mode: 'fresh', now: REC.now, frames: REC.n }, 'insiliscope-recording'); }
  catch (e) { console.warn('the recording\'s settings file:', e); }
  recGtEarly();
  iscTrack('record', { frames: iscTrack.framesBucket(REC.n), continue: REC.live });
  if (L.on) L.on = false;
  syncButtons();
  if (!L.pending) recNext();   // else after the frame in flight
}
// The ground truth's run beside the Record, on its own worker (the Save box takes it: L.gtWarm keeps a run that fits),
// when the last Save asked for one; a WideField one with the dye list as that Save had it. (Its heap, up to a few GB
// beside the live worker's, comes at the Save box too: the live worker's never shrinks.)
async function recGtEarly() {
  const o = saveOpts(), file = REC.file;
  if (!o.gt || !file) return;
  const kind = await gtKind(movieSpec({ frames: REC.n, 'start-sec': recStartSec(), 'fresh-basis': 1 }));
  if (REC.file !== file || !REC.on) return;
  const min = Math.max(0, +o.minCore || 0), dyeCsv = kind === 'wf' && !!o.dyes;
  gtRun(JSON.stringify(file, null, 1), min, dyeCsv, { min, vol: GT_VOL, dyeCsv });
}
function recMovie() {
  const w = worker();
  if (!w) { L.err = 'the workers are not up yet'; REC.on = false; syncButtons(); showInfo(); return; }
  const job = { type: 'movie', id: ++L.id, spec: movieSpec({ frames: REC.n, 'start-sec': recStartSec(), 'fresh-basis': 1 }), fresh: true, t0: performance.now() };
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
  Object.assign(REC, { w: d.w, h: d.h, spec: d.spec, buf: d.frames, got: n, engineMs: performance.now() - job.t0,
    gpu: d.gpu && d.gpu !== 'CPU' ? d.gpu : '' });
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
    REC.chunks.push({ spec: job.spec, n: k, lit: true });
    REC.engineMs += d.workerMs || 0;
  }
  L.err = '';
  L.t = d.t; L.clock = d.clock; L.drift = d.drift; L.pattern = d.pattern || null; L.engine = d.engine; L.paths = d.paths || null;
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
  else gtStop();   // (no recording: a ground truth's run begun with it let go)
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
  closePlayer(true);   // (a ground truth's run begun with the Record goes on)
  // the recording's settings file: the movie reproducible from it (insiliscope_cli --settings)
  if (!REC.now && self.iscSettings && self.ISC_ENGINE_OPTIONS && REC.nowArgs) REC.now = self.iscSettings.current(...REC.nowArgs);
  let settings = !REC.live && REC.file && n === REC.n ? REC.file : null;
  try {
    if (!settings && REC.now && self.iscSettings) settings = self.iscSettings.recordFile({ mode: REC.live ? 'live' : 'fresh', now: REC.now, frames: n,
      log: REC.log || [], chunks: REC.chunks, tSec: L.t }, 'insiliscope-recording');
  } catch (e) { console.warn('the recording\'s settings file:', e); }
  Object.assign(PLAY, { on: true, frames: REC.buf.subarray(0, n * REC.w * REC.h), w: REC.w, h: REC.h, n, desc, spec: REC.spec,
    exp, pxUm: REC.pxUm || (+$('mv_pixel-nm').value || 100) / 1000, i: 0, run: n > 1, settings, gpu: REC.live ? '' : REC.gpu || '' });
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
function closePlayer(keepGt) {
  if (!keepGt) gtStop();   // (a ground truth's run of this recording)
  cancelAnimationFrame(PLAY.timer);
  Object.assign(PLAY, { on: false, frames: null, run: false });
  $('lv_play').hidden = true;
  $('liveView').classList.remove('playing');
}
$('lv_playBtn').addEventListener('click', () => { PLAY.run = !PLAY.run && PLAY.n > 1; syncPlay(); if (PLAY.run) schedulePlay(); });
$('lv_playPos').addEventListener('input', () => { PLAY.run = false; syncPlay(); cancelAnimationFrame(PLAY.timer); showPlayFrame(+$('lv_playPos').value); });
$('lv_playClose').addEventListener('click', () => { closePlayer(); REC.msg = ''; showInfo(); });
// Save: the movie alone (a .tif), or with its settings file (settings.isc.json: the recording reproducible) and its
// ground truth (gt/, docs/ground-truth.md: computed now by rendering the settings file's movie again with the sink on)
// in one .zip (web/live/zip.js).
const esc = s => String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/"/g, '&quot;');
function download(blob, name) {
  const a = document.createElement('a');
  a.href = URL.createObjectURL(blob);
  a.download = name;
  a.click();
  setTimeout(() => URL.revokeObjectURL(a.href), 30000);
}
const SAVE_DEF = { settings: true, gt: false, minCore: 300, dyes: true };
const GT_VOL = [10, 10];   // the density stacks' voxel (nm, lateral and axial; finer when it fits 1 GB)
// The recording's settings file as text (made once per recording: the ground truth's run is keyed by it).
const playSettingsText = () => {
  if (PLAY.settingsText === undefined || PLAY.settingsOf !== PLAY.settings)
    Object.assign(PLAY, { settingsText: JSON.stringify(PLAY.settings, null, 1), settingsOf: PLAY.settings });
  return PLAY.settingsText;
};
function saveOpts() { try { return Object.assign({}, SAVE_DEF, JSON.parse(store.get('saveOpts', '{}'))); } catch (e) { return { ...SAVE_DEF }; } }
// What the recording's ground truth is: 'smlm' (a blinking label: localization tables), 'wf' (continuous dyes only:
// the dye list and density stacks) or 'none' (no label on: the frames, and the beads if any). From the recording's
// own spec (the setup job: each mounted target's mode); GroundTruth::Blinking decides the files alike.
async function gtKind(spec) {
  try {
    const S = await setupJob(spec), on = S.labels.filter(l => !l.off);
    return on.some(l => l.mode !== 'WideField') ? 'smlm' : on.length ? 'wf' : 'none';
  } catch (e) { return 'smlm'; }
}
async function openSave() {
  if (!PLAY.on) return;
  const D = self.iscSettings;
  if (!D || !D.dialog) { saveRecording({ settings: false, gt: false }, `insiliscope_live_${PLAY.w}x${PLAY.h}x${PLAY.n}`); return; }
  const o = saveOpts(), spec = PLAY.spec || '', kind = await gtKind(spec);
  if (!PLAY.on) return;
  const opt = k => { const m = new RegExp('(?:^| )' + k + '=(\\S+)').exec(spec); return m ? +m[1] : NaN; };
  const cut = opt('blink-min-photons'), warn = [];
  if (cut > 0 && kind === 'smlm') warn.push(`Blinks under ${cut} photons were not drawn (Quality), so they are not in the table.`);
  if (!PLAY.settings) warn.push('No settings file, so no ground truth.');
  // What each box writes (GroundTruth::Files decides the ground truth's files alike: localizations with a blinking
  // label, else the beads; density stacks for dyes that glow throughout; dyes.csv only without a blinking label)
  const box = D.dialog('<h3>Save the recording</h3>' +
    '<div class="row"><label for="svName">Name</label><input type="text" id="svName" maxlength="80"></div>' +
    `<label class="part"><input type="checkbox" checked disabled> <b>Movie</b><span class="s">movie.tif, ${PLAY.n} frames, 16-bit</span></label>` +
    '<label class="part"><input type="checkbox" id="svSet"> <b>Settings file</b><span class="s">settings.isc.json, ' +
    'reproduces the movie (☰ › Load)</span></label>' +
    '<label class="part"><input type="checkbox" id="svGt"> <b>Ground truth</b><span class="s">' +
    (kind === 'smlm'
      ? 'localizations.csv: blinks with ≥ <input type="number" id="svMin" min="0" step="10"> photons in the 500 nm square ' +
        'around them. '
      : kind === 'wf' ? 'a density stack per target (10 nm voxels). '
      : 'the beads, if any. ') +
    'Computed on save; takes a while</span></label>' +
    (kind === 'wf'
      ? '<label class="part" id="svDyesRow"><input type="checkbox" id="svDyes"> <b>Dye list</b><span class="s">dyes.csv, every ' +
        'dye (large: 100-600 MB)</span></label>'
      : '') +
    warn.map(w => `<div class="warn">${esc(w)}</div>`).join('') +
    '<div class="msg" id="svMsg">Movie alone: a .tif. Otherwise a .zip; drop its <code>open_in_napari.py</code> on napari ' +
    'to view it.</div>' +
    '<progress id="svBar" max="1" hidden style="width:100%"></progress>' +
    '<div class="row"><span class="sp"></span><button id="svCancel">Cancel</button><button id="svGo">Save</button></div>',
    () => L.gtDrop());   // (closed however: the run let go)
  const q = s => box.querySelector(s);
  q('#svName').value = `insiliscope_live_${PLAY.w}x${PLAY.h}x${PLAY.n}`;
  q('#svSet').checked = o.settings && !!PLAY.settings;
  q('#svSet').disabled = !PLAY.settings;
  q('#svGt').checked = o.gt && !!PLAY.settings;
  q('#svGt').disabled = !PLAY.settings;
  if (q('#svMin')) q('#svMin').value = o.minCore;
  if (q('#svDyes')) q('#svDyes').checked = o.dyes;
  const sync = () => { if (q('#svDyes')) { q('#svDyesRow').style.opacity = q('#svGt').checked ? '' : '.5'; q('#svDyes').disabled = !q('#svGt').checked; } };
  q('#svGt').addEventListener('change', sync);
  sync();
  // The ground truth's run starts while the box is open (Save takes it if the options still fit: L.gtWarm), and again
  // when a change needs another one (a lower threshold, the dye list on); unticked, none.
  const minOf = () => (q('#svMin') ? Math.max(0, +q('#svMin').value || 0) : o.minCore);
  const warm = () => {
    if (!q('#svGt').checked || !PLAY.settings) return;
    const min = minOf(), dyeCsv = kind === 'wf' && (q('#svDyes') ? q('#svDyes').checked : o.dyes);
    L.gtWarm(playSettingsText(), min, dyeCsv, { min, vol: GT_VOL, dyeCsv });
  };
  for (const id of ['#svGt', '#svMin', '#svDyes']) if (q(id)) q(id).addEventListener('change', warm);
  warm();
  q('#svCancel').onclick = () => D.closeDialog();
  q('#svGo').onclick = async () => {
    // the box shows the threshold or the dye list, never both: the hidden one keeps its stored value
    const c = { settings: q('#svSet').checked, gt: q('#svGt').checked, dyes: q('#svDyes') ? q('#svDyes').checked : o.dyes,
      minCore: minOf() };
    store.set('saveOpts', JSON.stringify(c));
    if (kind !== 'wf') c.dyes = false;
    const name = (q('#svName').value.trim() || `insiliscope_live_${PLAY.w}x${PLAY.h}x${PLAY.n}`).replace(/[\\/:*?"<>|]+/g, '_');
    q('#svGo').disabled = true;
    try {
      // say(text, fraction): a bar under the text; fraction null = no fraction yet (a moving bar), undefined = none
      await saveRecording(c, name, (t, f) => {
        q('#svMsg').textContent = t;
        const bar = q('#svBar');
        bar.hidden = f === undefined;
        if (f === null) bar.removeAttribute('value'); else if (f !== undefined) bar.value = f;
      });
      D.closeDialog();
    } catch (e) {
      q('#svBar').hidden = true;
      q('#svMsg').innerHTML = `<span class="warn">${esc(e.message || e)}</span>`;
      q('#svGo').disabled = false;
    }
  };
}
// c = {settings, gt, dyes, minCore}; say(text): progress.
async function saveRecording(c, name, say = () => {}) {
  iscTrack('save-tiff', { source: 'record', frames: iscTrack.framesBucket(PLAY.n), settings: !!c.settings, gt: !!c.gt });
  const tif = writeTiff16(PLAY.frames, PLAY.w, PLAY.h, PLAY.n, PLAY.desc, PLAY.pxUm);
  if (!c.settings && !c.gt) {
    download(tif, name + '.tif');
    REC.msg = `saved ${name}.tif (${(tif.size / 1e6).toFixed(1)} MB)`;
    showInfo();
    return;
  }
  const settingsText = playSettingsText();
  const entries = [{ name: 'movie.tif', data: new Uint8Array(await tif.arrayBuffer()) }];
  if (c.settings) entries.push({ name: 'settings.isc.json', data: settingsText });
  if (c.gt) {
    // The stages of isc_scope_gt: setup (the sample, the PSF: no fraction, often most of the time), the frames (a
    // fraction), the files; the seconds tick on here (the worker is busy in one call).
    const t0 = performance.now(), P = { stage: 'setup', f: 0, info: null };
    const show = () => {
      const s = `${Math.round((performance.now() - t0) / 1000)} s`;
      if (P.stage === 'setup') say(`Ground truth: preparing (${s})`, null);
      else if (P.stage === 'files') say(`Ground truth: writing files (${s})`, null);
      else say(`Ground truth: frame ${P.info && P.info.frame || Math.round(P.f * PLAY.n)} of ` +
        `${P.info && P.info.frames || PLAY.n} (${s})`, P.f);
    };
    show();
    const tick = setInterval(show, 500);
    let files;
    try {
      files = await L.groundTruth({ text: settingsText, min: c.minCore, vol: GT_VOL, dyeCsv: !!c.dyes },
        (f, stage, info) => { Object.assign(P, { f, stage: stage || 'frames', info }); show(); });
    } finally { clearInterval(tick); }
    for (const [n, b] of files) entries.push({ name: 'gt/' + n, data: b });
    // the converter only beside the dye list it reads (the stacks need none; the page is also in ☰ › Tools)
    if (files.some(([n]) => n === 'dyes.csv'))
      try { const r = await fetch('voxelize.html'); if (r.ok) entries.push({ name: 'gt/voxelize.html', data: await r.text() }); } catch (e) { /* offline: without it */ }
    say(`Packing ${Math.round(entries.reduce((s, e) => s + (e.data.length || 0), 0) / 1e6)} MB ...`, null);
  } else say('Packing ...');
  // the napari opener (web/open_in_napari.py): unpacked (or beside the zip), dropped on napari's window, it opens the
  // recording there
  let napari = false;
  try {
    const r = await fetch('open_in_napari.py');
    if (r.ok) { entries.push({ name: 'open_in_napari.py', data: await r.text() }); napari = true; }
  } catch (e) { /* offline: without it */ }
  const vox = entries.some(e => e.name === 'gt/voxelize.html');
  entries.push({ name: 'README.txt', data: `inSiliScope recording ${name}, saved ${new Date().toISOString()} (viewer ${self.ISC_VERSION || ''}).\n\n` +
    `movie.tif          ${PLAY.n} frames of ${PLAY.w} x ${PLAY.h} px, 16-bit, ${+(PLAY.pxUm * 1000).toFixed(2)} nm pixels, ${PLAY.exp} s frames\n` +
    (c.settings ? 'settings.isc.json  reproduces the movie: load it in the viewer, or insiliscope_cli --settings settings.isc.json\n' : '') +
    (c.gt ? 'gt/                the ground truth (gt/README.txt)\n' : '') +
    (vox ? 'gt/voxelize.html   open in a browser, drop gt/dyes.csv on it: a stack at any voxel size\n' : '') +
    (napari ? 'open_in_napari.py  drop on napari\'s window to view everything (pip install "napari[all]" tifffile)\n' : '') });
  const blob = await self.iscZip.zip(entries);
  download(blob, name + '.zip');
  self.iscLastSave = { name: name + '.zip', entries: entries.map(e => e.name), blob };   // (the browser test reads it)
  REC.msg = `saved ${name}.zip (${(blob.size / 1e6).toFixed(1)} MB)`;
  showInfo();
}
$('lv_playSave').addEventListener('click', openSave);
// ● Record opens a small box under the buttons for the number of frames (and, Advanced, "continue the live sample");
// its Start (or Enter) records. While recording, ● Record stops.
const recPop = (open) => {
  $('lv_recPop').hidden = !open;
  if (open) { $('lv_recN').focus(); $('lv_recN').select(); }
};
$('lv_rec').addEventListener('click', () => { if (REC.on) startRec(); else recPop($('lv_recPop').hidden); });
$('lv_recGo').addEventListener('click', () => { recPop(false); startRec(); });
$('lv_recPop').addEventListener('keydown', ev => {
  if (ev.key === 'Enter') { ev.preventDefault(); for (const id of ['lv_recN', 'lv_recT']) $(id).dispatchEvent(new Event('change')); recPop(false); startRec(); }
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
