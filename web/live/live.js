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

// ---- splitter: the right panel's width, 33-72 % of the window (41.6 % by default: 2026-11-13, the live image ~18 %
// smaller than at 50 %, the world view gets the room) ----
const wrap = $('wrap'), splitter = $('splitter');
const LIVE_MIN = 0.33, LIVE_MAX = 0.72;
function setLiveFrac(f, save) {
  f = Math.max(LIVE_MIN, Math.min(LIVE_MAX, f));
  wrap.style.setProperty('--live-w', (100 * f).toFixed(2) + 'vw');
  if (save) store.set('livePanel', f.toFixed(4));   // (was liveWidth, default 0.5, and liveFrac, 0.4: a new key so the new default shows)
}
setLiveFrac(+store.get('livePanel', '0.416') || 0.416, false);
resizeCanvas();   // the world view was measured at the default width (the main script ran first): measure it again
splitter.addEventListener('pointerdown', e => {
  if (e.button !== 0) return;
  e.preventDefault();
  splitter.classList.add('drag');
  splitter.setPointerCapture(e.pointerId);
  // (the panel's width is the same with the settings column open or folded)
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
  L.worker = spawnGenWorker({ mt: true, remote: 'live', gpuWarm: true });   // (the threaded module where the page is cross-origin isolated)
  L.worker.onmessage = e => onResult(e.data);
  // a failure here stops live only (the shared handler would move every worker to the main thread)
  L.worker.onerror = e => { e.preventDefault(); L.err = workerErrorText(e.message || 'the live worker failed'); L.pending = null; L.worker.terminate(); L.worker = null; L.worldKey = null; setLive(false); };
  return L.worker;
}
// The live session follows the sample: a new world (seed, cell or specimen parameters) is a fresh sample, as the
// adapter resets its history on a world change.
function worldKey(spec) { return spec.split(' ').filter(t => /^(world-seed|specimen|p\.)/.test(t)).join(' '); }
// n > 1: a recording chunk (n frames at once, LiveScope::Frames); ov: a multi-D block's overrides of the spec (its
// position, focus, exposure and settings; the controls left as they are).
function request(n, ov) {
  const w = worker();
  if (!w) { L.err = 'the workers are not up yet'; showInfo(); return; }
  if (L.pending) return;
  // (a recording chunk: its mean-field bleach basis its own, so its frames are a function of its file: fresh-basis)
  const spec = movieSpec(n ? { frames: 1, 'fresh-basis': 1, ...(ov || {}) } : { frames: 1 });
  const wk = worldKey(spec);
  const reset = wk !== L.worldKey;
  if (reset) { L.worldKey = wk; L.t = 0; L.log = []; }
  if (n && !REC.log) REC.log = L.log.map(r => ({ ...r }));   // the light before the Record (its settings file)
  const job = { type: 'live', id: ++L.id, spec, reset, t: L.t, lit: true, t0: performance.now(), profile: !n && expert() };
  if (n) {
    job.n = n; job.rec = true;
    if (recOpfs()) job.disk = { path: DISK.path(REC.disk.dir, 'frames.raw'), first: REC.got };   // (written at their place)
  }
  L.pending = job;
  w.postMessage(job);
  syncIdle();
}
function onResult(d) {
  const job = L.pending;
  if (!job || d.id !== job.id) return;
  if (d.type === 'movie-progress') { if (job.fresh) onRecProgress(d); return; }
  if (d.type === 'movie-part') { if (job.fresh) onRecPart(d); return; }
  if (d.type === 'movie-rows') { if (job.fresh) onRecRows(d); return; }
  L.pending = null;
  // (a Record started while the sample's light was replayed waits for it: it starts now)
  if (job.type === 'live-replay') { onReplay(d, job); if (REC.on && !L.pending) recNext(); return; }
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
  L.remote = !!d.remote;
  L.paths = d.paths || null;
  L.perf = { roundMs: L.ms, workerMs: d.workerMs, profile: d.profile || null, memory: d.memory || null };
  // Live is paced to the exposure on an absolute schedule (2026-11-12): each frame is due one exposure after the last
  // one's due time (not after it was shown: the timer's lateness and the drawing added up, 50 ms gave 16 fps), and the
  // next frame is asked for just early enough (its round trip) to arrive by its due time, before this one is drawn, so
  // the worker renders while the page draws (10 ms with ~7 ms frames gave 56 fps: drawing and rendering in series).
  L.trip = L.trip ? 0.8 * L.trip + 0.2 * L.ms : L.ms;
  const now = performance.now(), due = L.on && L.nextDue ? L.nextDue : now;
  L.perf.waitMs = L.on ? Math.max(0, due - now) : 0;
  const show = () => {
    const t = performance.now(), exp = exposureSec() * 1000;
    if (L.lastShown) L.fps = 0.8 * L.fps + 0.2 * (1000 / Math.max(1, t - L.lastShown));
    L.lastShown = t;
    L.frames++;
    // the next due time: one exposure after this one's; behind by more than that (frames slower than the exposure),
    // from now (no burst to catch up)
    L.nextDue = Math.max(due + exp, t);
    if (REC.on) { draw(d); showPerf(); recNext(); return; }
    clearTimeout(L.reqTimer);
    const ask = () => { if (L.on && !document.hidden && !REC.on) request(); };
    const askIn = L.nextDue - t - L.trip - 2;   // (2 ms for the message's way)
    if (askIn <= 0) ask(); else L.reqTimer = setTimeout(ask, askIn);
    draw(d);
    showPerf();
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
// the next spool file of the run's files on the disk storage (each made set of files its own), or none
const gtDisk = () => GT.dir ? { path: DISK.path(GT.dir, `gt-${++GT.spool}.spool`) } : undefined;
// The made files as [[name, bytes or Blob]]: those written to a spool file are its slices (on disk).
async function gtFilesOf(r) {
  if (!r.spool || !GT.dir) return r.files;
  const f = await DISK.file(GT.dir, r.spool.split('/').pop());
  return r.files.map(([n, b]) => [n, b instanceof Uint8Array ? b : f.slice(b.at, b.at + b.size)]);
}
function gtStop() {
  const { w, run, files } = GT;
  Object.assign(GT, { w: null, key: null, run: null, files: null, pre: null, progress: null, last: null, dir: null });
  if (w) w.terminate();
  const e = new Error('the ground truth was cancelled');
  if (run) run.reject(e);
  if (files) files.reject(e);
  if (GT.wasOn) { GT.wasOn = false; setLive(true); }
}
// dir: the recording's folder on the disk storage (its files written there, not held), or none.
function gtRun(text, minPhotons, dyeCsv, filesFor, dir) {
  gtStop();
  if (!genWorkers.ok) return false;
  if (L.on) { L.on = false; syncButtons(); GT.wasOn = true; }
  const w = spawnGenWorker({ remote: 'gt' }), run = deferred();
  Object.assign(GT, { w, key: { text, minPhotons, dyeCsv }, run, dir: dir || null, spool: 0 });
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
      if (d.files) GT.pre = { for: d.filesFor, files: d.files, spool: d.spool };
      run.resolve();
    }
    else if (d.type === 'gt-files' && GT.files) { if (d.error) fail(d.error); else GT.files.resolve({ files: d.files, spool: d.spool }); }
  };
  w.onerror = e => { e.preventDefault(); if (GT.w === w) fail(e.message || 'the ground truth\'s worker failed'); };
  w.postMessage({ type: 'gt-run', id: 1, text, minPhotons, dyeCsv, files: filesFor || null, disk: gtDisk() });
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
L.gtWarm = (text, minPhotons, dyeCsv, filesFor, dir) => { if (!gtFits(GT.key, text, minPhotons, dyeCsv)) gtRun(text, minPhotons, dyeCsv, filesFor, dir); };
L.gtDrop = gtStop;
// opt = {text, min (photons in a blink's 500 nm core), vol: [xy, z], dyeCsv}; progress(fraction, stage, info):
// isc_scope_gt's stages (a run
// already on its way reports where it is). Resolves with the files, [[name, bytes], ...]; a failure (out of memory
// too) rejects. The worker is closed after.
L.groundTruth = async (opt, progress) => {
  const min = opt.min;
  if (!gtFits(GT.key, opt.text, min, opt.dyeCsv) && !gtRun(opt.text, min, opt.dyeCsv, null, opt.dir)) throw new Error('the workers are not up yet');
  const { w, run } = GT;
  GT.progress = progress || null;
  if (progress && GT.last) progress(...GT.last);
  try {
    await run.p;
    if (GT.w !== w) throw new Error('the ground truth was cancelled');
    if (GT.pre && gtSameOpts(GT.pre.for, opt)) return await gtFilesOf(GT.pre);   // made as the run ended
    GT.pre = null;   // (let them go before the new ones come)
    const files = GT.files = deferred();
    w.postMessage({ type: 'gt-files', id: 2, min: opt.min, vol: opt.vol, dyeCsv: opt.dyeCsv, disk: gtDisk() });
    return await gtFilesOf(await files.p);
  } finally { if (GT.w === w) gtStop(); }
};
// A fresh sample at the next frame (a measurement without history).
L.freshSample = () => { L.worldKey = null; L.t = 0; L.log = []; };
// The remote server came or went (web/live/remote.js): a new live worker (linked to it or not), the sample's light so
// far replayed on it (a live-replay job), so the next frame is the one the old worker would have rendered. Not during a
// Record (it waits for its end).
L.rebind = () => {
  if (REC.on || (L.pending && L.pending.type === 'live-replay')) { setTimeout(L.rebind, 500); return; }
  if (!L.worker) return;
  const wasOn = L.on, log = L.log.map(r => ({ ...r }));
  L.on = false;
  L.worker.onmessage = null; L.worker.onerror = null; L.worker.terminate();
  L.worker = null; L.pending = null;
  const back = () => { if (wasOn) setLive(true); else syncButtons(); showInfo(); };
  if (!log.length || !L.worldKey) { L.worldKey = null; back(); return; }
  L.replay(log).then(back, e => { L.err = e.message || String(e); L.worldKey = null; back(); });
};
L.showInfo = () => showInfo();
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
// A frame is taken at once; its pixels go to the canvases at the display's next refresh (requestAnimationFrame, the
// newest frame when several came in between). Drawn straight from the worker's message, Chrome's 2D-canvas rate
// limiter held the page ~150-180 ms for the GPU several times a second (frames faster than the screen: 10 ms
// exposures ran at ~20 fps, 2026-11-12).
function draw(d) {
  L.frame = d.frames; L.w = d.w; L.h = d.h; L.spec = d.spec || L.pendingSpec || '';
  $('lv_idle').style.display = 'none';
  L.newFrame = true;
  if (!L.drawRaf) L.drawRaf = requestAnimationFrame(drawNow);
}
function drawNow() {
  L.drawRaf = 0;
  if (!L.newFrame || !L.frame) return;
  L.newFrame = false;
  if ($('lv_auto').checked) {
    const d = { frames: L.frame };
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
  // the world view shows the live frame only through the drift (the camera's field and the focal plane move with it):
  // redrawn when that moved, not after every frame (a world view drawn per frame held Firefox's page ~240 ms each)
  const dr = L.drift || [0, 0, 0], wd = L.worldDrift;
  if (!wd || Math.abs(dr[0] - wd[0]) + Math.abs(dr[1] - wd[1]) + Math.abs(dr[2] - wd[2]) > 1) { L.worldDrift = dr.slice(); requestDraw(); }
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
    : REC.on ? (REC.live && REC.got ? fps + ms : '') + (REC.plan ? `${REC.got ? ' · ' : ''}block ${recBlock().i + 1}/${REC.plan.blocks.length}` : '')   // a fresh sample's movie: its count is on the button
    : REC.msg && !L.on ? (L.ms > 0 ? fps + ms : '')
    : !L.frames ? (L.on ? 'starting…' : '')
    : L.on ? L.fps.toFixed(1) + ' fps · ' + ms
    : ms;
  // computed on the remote server (web/live/remote.js)
  const host = L.remote && self.iscRemote ? self.iscRemote.host() : '';
  const more = [L.frames ? `${L.w}×${L.h} px · ${(+$('mv_pixel-nm').value || 100).toFixed(1)} nm/px` : '',
    L.pattern ? `pattern ${L.pattern.frame % L.pattern.period + 1}/${L.pattern.period}` : '',
    REC.on ? `recording ${REC.got}/${REC.n}` + recBlockText() : REC.msg && !L.on ? REC.msg : ''].filter(Boolean);
  el.textContent = text + (host && text ? ' · ' + host : '');
  el.title = [text, ...more, host ? 'computed on the remote server ' + host : ''].filter(Boolean).join('\n');
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
// back, the last frame of each chunk shown; Record again stops early and saves the frames so far.
// The frames (2026-11-19; at most 1 GB before): a fresh sample's stream out of the engine as they are made (the movie
// job's stream: 'movie-part' messages, ~16 MB each), every recording's kept as the parts they came in (frameStore), so
// a recording is bounded by the tab's memory, not by one buffer or the engine's 4 GB heap; a part the worker cannot
// allocate stops the Record with the frames so far. Past 4 GB the saved TIFF is a BigTIFF (writeTiff16).
const REC = L.rec = { on: false, stop: false, live: false, n: 0, got: 0, store: null, w: 0, h: 0, spec: '', tStart: 0,
  litStart: 0, t0: 0, engineMs: 0, msg: '', now: null, log: null, chunks: [], disk: null };
// ---- a Record's files on the browser's disk storage (2026-11-20, docs/dev/LARGE_RECORDS.md part A) ----
// "Write frames to disk" (the Record box, on by default): the worker writes each frame into a raw frames file on the
// origin's private file system (OPFS: isc-rec/<tab>-<k>/frames.raw, frame f at f x w x h x 2 bytes), and the ground
// truth's files into spool files beside it, so neither the tab nor the zip holds them: the player reads frames back,
// Save makes the TIFF / zip as Blobs of those files (writeTiff16, zip.js: the bytes on disk, never in memory). A
// browser without it (no OPFS sync access handles, a private window) keeps the frames in memory as before. This tab's
// last recording stays until the next Record (a download may still read it) and every recording until the tab closes;
// at load and at each Record the recordings of tabs no longer open (their Web Lock let go) are deleted.
const DISK = L.disk = (() => {
  // (sync access handles exist in workers only: a worker that has none says so, the job runs in memory)
  const ok = !!(self.navigator && navigator.storage && navigator.storage.getDirectory);
  const tab = Date.now().toString(36) + Math.random().toString(36).slice(2, 8);
  let k = 0;
  if (ok && navigator.locks) navigator.locks.request('isc-rec-' + tab, () => new Promise(() => {})).catch(() => {});
  const root = async () => (await navigator.storage.getDirectory()).getDirectoryHandle('isc-rec', { create: true });
  return {
    ok,
    newDir: () => `${tab}-${++k}`,
    path: (dir, name) => `isc-rec/${dir}/${name}`,
    async file(dir, name) { return (await (await (await root()).getDirectoryHandle(dir)).getFileHandle(name)).getFile(); },
    // bytes this page may still write (the browser's quota for the origin less what it holds)
    async room() { const e = await navigator.storage.estimate(); return Math.max(0, (e.quota || 0) - (e.usage || 0)); },
    // delete the recordings of closed tabs, and this tab's but those in keep
    async sweep(keep = []) {
      if (!ok) return;
      try {
        const r = await root(), held = navigator.locks ? new Set((await navigator.locks.query()).held.map(l => l.name)) : null;
        const names = [];
        for await (const name of r.keys()) names.push(name);
        for (const name of names) {
          const t = name.split('-')[0];
          if (t === tab ? !keep.includes(name) : held && !held.has('isc-rec-' + t)) await r.removeEntry(name, { recursive: true }).catch(() => {});
        }
      } catch (e) { /* no storage: nothing to delete */ }
    },
  };
})();
DISK.sweep();
// ---- a Record straight to a file the person chooses (2026-11-20; Chrome and Edge: the File System Access API) ----
// "save to a file as recorded" (the Record box, where the browser has a save picker): Start asks for a .tif and the
// frames go into it as the engine makes them, with no storage quota (the browser's disk storage gives a page a share
// of the disk, 2.7 GB in the preview pane). The worker posts the frames (whole frames, or a strip's rows: 'movie-part',
// 'movie-rows'), the page writes each at its place in the TIFF (createWritable: the browser keeps the writes in a file
// beside the chosen one and puts it in place at the end), with room in front for the tags, written last for the frames
// it has (tiffHead in index.html; a BigTIFF past 4 GB), and the file cut after the last frame. A frame written goes
// from memory; the frames wait only while the disk is slower than the engine. The player reads them back from the
// file; Save .tif has nothing left to do, a .zip takes the file as its movie.tif. The ground truth's spool files go
// to the disk storage as before (or memory).
const FILE = L.file = { ok: typeof self.showSaveFilePicker === 'function' };
const recOpfs = () => !!(REC.disk && !REC.disk.file);   // (the frames on the disk storage, not in a file)
function fileOut(handle, writable, w, h, nMax, descBytes) {
  const px = w * h;
  // room for the tags: a BigTIFF's for nMax pages, the description's bytes and 64 KB to spare
  const at = tiffHeadBytes(nMax, descBytes + 65536, 1, true);
  let chain = Promise.resolve(), error = null, queued = 0, got = 0;
  const put = (data, position) => {
    queued += data.byteLength;
    const k = data.byteLength;
    chain = chain.then(() => error ? null : writable.write({ type: 'write', position, data }))
      .catch(e => { error = error || e; }).then(() => { queued -= k; });
  };
  return {
    toFile: true, handle, at, w, h,
    get n() { return got; },
    get queued() { return queued; },
    get error() { return error; },
    // whole frames, in order
    add(data, n) {
      if (!n) return;
      if (data.length !== n * px) { error = error || new Error(`the frames are not ${w} × ${h} px`); return; }
      put(data, at + got * px * 2);
      got += n;
    },
    // frames first .. first + n - 1, rows y0 .. y0 + rows - 1 (a strip)
    rows(first, n, y0, rows, data) {
      const k = rows * w;
      for (let i = 0; i < n; i++) put(data.subarray(i * k, (i + 1) * k), at + ((first + i) * h + y0) * w * 2);
      if (y0 + rows >= h) got = Math.max(got, first + n);
    },
    // the tags for n frames, the file cut after them and put in place: its File
    async finish(n, desc, pxUm) {
      await chain;
      if (error) throw error;
      const head = tiffHead(w, h, n, desc, pxUm, at + n * px * 2 >= 2 ** 32 - 2 ** 26, at);
      if (head.byteLength > at) throw new Error('the TIFF tags do not fit in front of the frames');
      await writable.write({ type: 'write', position: 0, data: head });
      await writable.truncate(at + n * px * 2);
      await writable.close();
      return handle.getFile();
    },
    abort() { error = error || new Error('cancelled'); chain = chain.then(() => writable.abort()).catch(() => {}); },
  };
}
// the fresh sample's time at its first frame (the movie's start-sec; 0 = the light comes on): not the Start time
// setting, which live keeps; hidden with "continue the live sample", which runs on from the live sample's time
const syncRecT = (init) => { $('lv_recTRow').hidden = $('lv_recFieldRow').hidden = $('lv_recLive').checked; if (!init) syncRecField(); };
$('lv_recLive').checked = store.get('recLive', '0') === '1';
$('lv_recLive').addEventListener('change', () => { store.set('recLive', $('lv_recLive').checked ? '1' : '0'); syncRecT(); });
syncRecT(true);
$('lv_recN').value = store.get('recN', '100');
$('lv_recN').addEventListener('change', () => store.set('recN', $('lv_recN').value));
$('lv_recT').value = store.get('recT', '0');
$('lv_recDisk').checked = store.get('recDisk', '1') === '1';
$('lv_recDisk').addEventListener('change', () => store.set('recDisk', $('lv_recDisk').checked ? '1' : '0'));
$('lv_recDiskRow').hidden = !DISK.ok;
$('lv_recFile').checked = FILE.ok && store.get('recFile', '0') === '1';
$('lv_recFile').addEventListener('change', () => store.set('recFile', $('lv_recFile').checked ? '1' : '0'));
$('lv_recFileRow').hidden = !FILE.ok;
$('lv_recT').addEventListener('change', () => store.set('recT', $('lv_recT').value));
const recStartSec = () => Math.max(0, +$('lv_recT').value || 0);
// The Record's field (2026-11-20, docs/dev/LARGE_RECORDS.md part C): W x H px centred on the stage, empty = the live
// field (mv_size); a fresh sample only (a sample continued keeps the live field). Up to 16384 px a side: past 4096 px,
// or past the module's memory, the engine renders it in strips of rows (frames on the disk storage only). The beam
// scales with the field (the user's choice): illum-fwhm stays a share of the field's longer side, which the box says
// before Start (lv_recFieldNote). While the box is open the world view outlines the field (self.ISC_REC_FIELD).
const REC_MAX_SIDE = 16384, REC_WHOLE_SIDE = 4096, VIEW_MAX = 2048;
function recField() {
  const live = Math.max(1, +($('mv_size') || {}).value || 128);
  const side = v => { const n = Math.round(+v); return Number.isFinite(n) && n > 0 ? Math.min(REC_MAX_SIDE, Math.max(8, n)) : 0; };
  const w = side($('lv_recW').value) || live, h = side($('lv_recH').value) || w;
  return { w, h, live, larger: !$('lv_recLive').checked && !planOn() && (w !== live || h !== live) };
}
// What stops a Record before it starts, said in the box (lv_recMsg) as the numbers are typed, and again when Start is
// refused (recRefused: the box reopens with the reason; the info line alone was easy to miss): the frames past the
// room the browser gives this page's disk storage (recRoom, asked when the box opens), a field past 4096 px without it.
let recRoom = null;
const recBytes = () => {
  const f = recField(), live = $('lv_recLive').checked || planOn(), w = live ? f.live : f.w, h = live ? f.live : f.h;
  const n = planOn() ? Math.max(1, planCheck().frames) : Math.max(1, Math.round(+$('lv_recN').value || 0));
  return { w, h, n, bytes: n * w * h * 2 };
};
const gbText = b => (b / 1e9 < 0.1 ? (b / 1e9).toFixed(3) : (b / 1e9).toFixed(1)) + ' GB';
function recProblem(room = recRoom) {
  const { w, h, n, bytes } = recBytes(), file = FILE.ok && $('lv_recFile').checked && !planOn(), disk = DISK.ok && $('lv_recDisk').checked;
  if (!disk && !file && Math.max(w, h) > REC_WHOLE_SIDE)
    return `A field past ${REC_WHOLE_SIDE} px renders in strips, which write to the disk storage: tick "write frames to disk"` +
      (FILE.ok ? ' or "save to a file as recorded".' : DISK.ok ? '.' : ' (this browser offers no disk storage to this page).');
  if (disk && !file && room !== null && bytes > room)
    return `Not enough disk storage: ${n} frames of ${w} × ${h} px are ${gbText(bytes)}, this browser lets the page store ` +
      `${gbText(room)} (at most ${Math.floor(room / (w * h * 2))} frames of this field). ` +
      (FILE.ok ? 'Tick "save to a file as recorded" (no limit but the disk), or record fewer frames or a smaller field.'
               : 'Record fewer frames or a smaller field, or free disk space (the browser\'s share grows with it).');
  return '';
}
function syncRecMsg(text = recProblem()) {
  const m = $('lv_recMsg');
  m.textContent = text;
  m.hidden = !text;
}
function recRefused(text) {
  REC.msg = 'not recorded: ' + text;
  showInfo();
  $('lv_recPop').hidden = false;
  syncRecField();
  syncRecMsg(text);
}
function syncRecField() {
  syncRecMsg();
  const f = recField(), note = $('lv_recFieldNote'), open = !$('lv_recPop').hidden;
  $('lv_recW').placeholder = $('lv_recH').placeholder = String(f.live);
  self.ISC_REC_FIELD = open && f.larger ? { w: f.w, h: f.h } : null;
  if (typeof requestDraw === 'function') requestDraw();
  note.hidden = !f.larger;
  if (!f.larger) return;
  const pxUm = (+($('mv_pixel-nm') || {}).value || 100) / 1000, fwhm = +(($('mv_illum-fwhm') || {}).value) || 100;
  const prof = $('mv_illum-profile'), uniform = prof && prof.selectedOptions[0] && /uniform/i.test(prof.selectedOptions[0].textContent);
  const um = n => (n * pxUm).toFixed(n * pxUm < 100 ? 1 : 0);
  const gb = f.w * f.h * 2 * Math.max(1, +$('lv_recN').value || 1) / 1e9;
  const disk = DISK.ok && $('lv_recDisk').checked, strips = Math.max(f.w, f.h) > REC_WHOLE_SIDE;
  note.textContent = '';
  const b = document.createElement('b');
  b.textContent = `Field ${f.w} × ${f.h} px (${um(f.w)} × ${um(f.h)} µm), centred on the stage. `;
  note.append(b, uniform
    ? 'The illumination covers the whole recorded field (beam profile Uniform). '
    : `The beam scales with the field: its width stays ${fwhm} % of the field (Beam FWHM), so this Record's beam is ` +
      `${um(Math.max(f.w, f.h) * fwhm / 100)} µm wide instead of the live view's ${um(f.live * fwhm / 100)} µm. `,
    `${gb < 0.1 ? gb.toFixed(3) : gb.toFixed(1)} GB of frames.` +
    (strips ? (disk || (FILE.ok && $('lv_recFile').checked) ? ' Past 4096 px it renders in strips of rows (all frames of one strip, then the next).'
                    : ' Past 4096 px it needs "write frames to disk" (it renders in strips).') : '') +
    (f.w * f.h > 4096 * 4096 ? ` The player shows its centre ${Math.min(f.w, VIEW_MAX)} × ${Math.min(f.h, VIEW_MAX)} px.` : '') +
    ' Large fields are slow: every dye of the field is made.');
}
for (const id of ['lv_recW', 'lv_recH']) {
  $(id).value = store.get(id === 'lv_recW' ? 'recW' : 'recH', '');
  $(id).addEventListener('input', syncRecField);
  $(id).addEventListener('change', () => store.set(id === 'lv_recW' ? 'recW' : 'recH', $(id).value));
}
// ---- a multi-D Record (2026-11-23, Advanced and Expert; web/live/mda.js, the editor web/live/mda_ui.js) ----
// "multi-D plan" in the Record box: Start runs the plan's blocks (for blocks over positions, Z heights and any
// setting, recordings of N frames at an exposure inside them) in order, as one continuing live sample: the "continue
// the live sample" path (chunks of isc_live_frames, LiveScope::Frames), each chunk within one block, with the block's
// position, focus, exposure and settings as the spec's overrides (the controls stay as they are). The frames are kept
// in run order (the disk storage or memory, never "save to a file as recorded"); REC.place has each frame's sample
// time; Save makes the files of the format chosen (OME-TIFF, ImageJ hyperstacks, flat) and index.csv. The plan is kept
// in the box's store and in a Record's settings file (measurement.viewer.recordPlan).
const MDA = () => self.ISC_MDA;
let recPlan = null;
try { recPlan = JSON.parse(store.get('recPlan', '') || 'null'); } catch (e) { recPlan = null; }
if (!recPlan || !Array.isArray(recPlan.root)) recPlan = MDA() ? MDA().example() : { positions: [], root: [] };
$('lv_recPlan').checked = store.get('recPlanOn', '0') === '1';
function planOn() { return document.body.dataset.level !== 'basic' && $('lv_recPlan').checked; }
// the stage and focus at Start: where no Position loop says
const planBase = () => ({ x: view.cx, y: view.cy, z: +$('mv_z').value || 0 });
const planEng = () => self.ISC_ENGINE_OPTIONS || null;
// a key a plan may set: an engine option, or a key the spec takes without one (zern.<j>, beam.<j>.<col>, dye fields)
const planKnown = k => { const e = planEng(); return !e || e.has(k) || !!(self.iscSettingsCore && self.iscSettingsCore.partOf(e, k)); };
// the plan's setting names: the Micro-Manager names the settings' help shows (web/live/mm_cfg.js planKeys: Lasers.Pattern,
// Lasers.PatternPhaseMatrix, ...) and the engine's keys; made again when the engine's options arrive
let planKeysOf = null, planKeysMemo = null;
function planKeys() {
  const e = planEng();
  if (planKeysMemo && planKeysOf === e) return planKeysMemo;
  planKeysOf = e;
  planKeysMemo = self.iscMmCfg && self.ISC_MM_CFG ? self.iscMmCfg.planKeys({ engine: e, known: planKnown }) : null;
  return planKeysMemo;
}
// A block's phase matrix (sim-pair-phase.<row>.<pair>, from Lasers.PatternPhaseMatrix) has the program's rows and pairs
// where the block leaves the Pattern as it is now (index.html PHASES.shape; the engine would take the entries that fit
// and drop the rest without a word). A block that sets the Pattern, its phases, orientations or beams is not checked.
const PATTERN_KEYS = /^(sim-type|sim-phases|sim-angles|beam\.\d+\.[A-Za-z]+)$/;
function planBlockCheck(b) {
  const sh = self.ISC_PHASES && ISC_PHASES.shape, keys = Object.keys(b.ov);
  const ph = keys.map(k => /^sim-pair-phase\.(\d+)\.(\d+)$/.exec(k)).filter(Boolean);
  if (!sh || !ph.length || keys.some(k => PATTERN_KEYS.test(k))) return [];
  const rows = Math.max(...ph.map(m => +m[1])) + 1, pairs = Math.max(...ph.map(m => +m[2])) + 1;
  if (!sh[0]) return [`Block ${b.path}: a phase matrix, but the Pattern (Lasers.Pattern) has no beam pairs: set a SIM Pattern first`];
  if (rows === sh[0] && pairs === sh[1]) return [];
  return [`Block ${b.path}: the phase matrix has ${rows} row(s) of ${pairs}; the Pattern's needs ${sh[0]} rows (frames of the cycle) of ${sh[1]} (beam pairs)`];
}
// the plan's checks and blocks now: { errs, blocks, frames }
function planCheck(plan = recPlan) {
  const D = MDA();
  if (!D) return { errs: ['the plan\'s code did not load'], blocks: [], frames: 0 };
  const base = planBase(), keys = planKeys() || undefined;
  const errs = D.validate(plan, base, { eng: planEng(), known: planKnown, keys, blockCheck: planBlockCheck });
  let blocks = [];
  if (!errs.length) try { blocks = D.expand(plan, base, planEng(), keys); } catch (e) { errs.push(e.message || String(e)); }
  return { errs, blocks, frames: blocks.reduce((s, b) => s + b.frames, 0) };
}
function syncPlan() {
  const on = planOn(), pop = $('lv_recPop');
  pop.classList.toggle('plan', on);
  const note = $('lv_recPlanNote'), sum = $('lv_recPlanSum');
  if (!on) { note.hidden = true; sum.textContent = ''; syncRecMsg(); return; }
  const c = planCheck(), side = recField().live;
  sum.textContent = c.errs.length ? 'plan not ready' : `${c.frames} frames`;
  note.classList.toggle('bad', !!c.errs.length);
  note.textContent = c.errs.length ? c.errs[0] + (c.errs.length > 1 ? ` (and ${c.errs.length - 1} more: Edit…)` : '')
    : `${c.blocks.length} blocks · ${c.frames} frames of ${side} × ${side} px · ${gbText(c.frames * side * side * 2)}; ` +
      'one continuing live sample, the live field.';
  note.hidden = false;
  syncRecMsg();
}
$('lv_recPlan').addEventListener('change', () => { store.set('recPlanOn', $('lv_recPlan').checked ? '1' : '0'); syncPlan(); syncRecField(); });
new MutationObserver(syncPlan).observe(document.body, { attributes: true, attributeFilter: ['data-level'] });
// the editor's way in (web/live/mda_ui.js)
L.recPlan = {
  get: () => JSON.parse(JSON.stringify(recPlan)),
  set(p) { recPlan = JSON.parse(JSON.stringify(p)); store.set('recPlan', JSON.stringify(recPlan)); syncPlan(); },
  check: planCheck, base: planBase, eng: planEng, known: planKnown, keys: planKeys, on: planOn,
  side: () => recField().live, msPerFrame: () => L.ms || 0,
  goTo(x, y, z) {
    view.cx = x; view.cy = y;
    if (Number.isFinite(z)) { $('mv_z').value = z; for (const t of ['input', 'change']) $('mv_z').dispatchEvent(new Event(t, { bubbles: true })); }
    noteInteraction(); requestDraw();
  },
};
$('lv_recPlanEdit').addEventListener('click', () => { if (self.iscMdaEditor) self.iscMdaEditor.open(); });
// the block the run is in (REC.got: the frames so far) and its line for the info box
const recBlock = () => {
  const P = REC.plan, at = P && MDA().blockAt(P.blocks, Math.min(REC.got, REC.n - 1));
  return at ? { i: at.b, k: at.k, b: P.blocks[at.b] } : { i: 0, k: 0, b: null };
};
function recBlockText() {
  if (!REC.plan) return '';
  const { i, b } = recBlock();
  return b ? `\nblock ${i + 1}/${REC.plan.blocks.length} · ${b.pName || 'stage'} · z ${MDA().fmt(b.z)} µm · ${b.cName}` : '';
}
for (const id of ['lv_recN', 'lv_recDisk', 'lv_recFile']) $(id).addEventListener('input', syncRecField);
syncRecField();
// A recording's frames as the parts they came in (each whole frames, in order): no one buffer of the whole movie.
function frameStore() {
  const parts = [];   // { first, n, data }
  let total = 0, px = 0;
  return {
    get n() { return total; },
    add(data, n) {
      if (!n) return;
      px = px || Math.round(data.length / n);
      parts.push({ first: total, n, data: data.length === n * px ? data : data.subarray(0, n * px) });
      total += n;
    },
    frame(i) {
      let lo = 0, hi = parts.length - 1;
      while (lo < hi) { const m = (lo + hi + 1) >> 1; if (parts[m].first <= i) lo = m; else hi = m - 1; }
      const p = parts[lo], k = i - p.first;
      return p.data.subarray(k * px, (k + 1) * px);
    },
    part(i) { return this.frame(i); },
    // the first n frames as arrays in order (writeTiff16 takes them as they are)
    arrays(n = total) {
      const out = [];
      for (const p of parts) {
        if (p.first >= n) break;
        out.push(p.first + p.n <= n ? p.data : p.data.subarray(0, (n - p.first) * px));
      }
      return out;
    },
  };
}
// A recording's frames in its file on the disk storage (a File: read back by slices): frame(i) is a promise; the
// last few frames read are kept for the player. A frame past 4096 x 4096 px (a large Record field) is shown as its
// centre, at most VIEW_MAX px a side (vw x vh: its rows read as one slice, the columns taken from them); the file and
// Save keep every pixel (VIEW_MAX, above).
function diskStore(file, w, h, n, base = 0) {
  const cache = new Map(), px = w * h, big = px > 4096 * 4096;
  const vw = big ? Math.min(w, VIEW_MAX) : w, vh = big ? Math.min(h, VIEW_MAX) : h;
  const x0 = (w - vw) >> 1, y0 = (h - vh) >> 1;
  const read = i => {
    if (!big) return file.slice(base + i * px * 2, base + (i + 1) * px * 2).arrayBuffer().then(b => new Uint16Array(b));
    const at = base + (i * px + y0 * w) * 2;
    return file.slice(at, at + vh * w * 2).arrayBuffer().then(b => {
      const rows = new Uint16Array(b), out = new Uint16Array(vw * vh);
      for (let y = 0; y < vh; y++) out.set(rows.subarray(y * w + x0, y * w + x0 + vw), y * vw);
      return out;
    });
  };
  return {
    disk: true, file, n, vw, vh, cropped: big,
    frame(i) {
      if (!cache.has(i)) {
        cache.set(i, read(i));
        if (cache.size > 8) cache.delete(cache.keys().next().value);
      }
      return cache.get(i);
    },
    arrays(m = n) { return [file.slice(base, base + m * px * 2)]; },
    part(i) { return file.slice(base + i * px * 2, base + (i + 1) * px * 2); },
  };
}
async function startRec() {
  if (REC.on && !REC.live) {   // cancel the movie: replace the worker (its live sample goes with it)
    if (L.worker) L.worker.terminate();
    L.worker = null; L.pending = null; L.worldKey = null;
    gtStop();   // (its ground truth's run)
    if (REC.out) { REC.out.abort(); REC.out = null; }   // (the chosen file left as it was)
    Object.assign(REC, { on: false, store: null, msg: 'recording cancelled' });
    syncButtons(); showInfo();
    return;
  }
  if (REC.on) { REC.stop = true; return; }
  if (REC.starting) return;
  // a multi-D plan: checked and run in order as one continuing live sample (its blocks known now)
  const plan = planOn() ? planCheck() : null;
  if (plan && plan.errs.length) { recRefused('the multi-D plan: ' + plan.errs.join('; ')); return; }
  const liveRec = !!plan || $('lv_recLive').checked;
  const n = plan ? plan.frames : Math.max(1, Math.round(+$('lv_recN').value || 0)), field = recField();
  const fw = liveRec ? field.live : field.w, fh = liveRec ? field.live : field.h;
  const bytes = n * fw * fh * 2, gb = bytes / 1e9;
  // the frames to the disk storage: room for them (the saved .tif or .zip is a Blob of the same file)
  let disk = DISK.ok && $('lv_recDisk').checked ? { dir: DISK.newDir() } : null;
  const early = recProblem(null);   // (the field; the room is asked below)
  if (early) { recRefused(early); return; }
  // straight to a file: the picker first (it needs the click), then the file opened for writing
  let out = null;
  if (FILE.ok && $('lv_recFile').checked && !plan) {
    REC.starting = true;
    try {
      const handle = await self.showSaveFilePicker({ suggestedName: `insiliscope_rec_${fw}x${fh}x${n}.tif`,
        types: [{ description: 'TIFF stack', accept: { 'image/tiff': ['.tif', '.tiff'] } }] });
      const spec = movieSpec(recFieldSpecOf(field));
      out = fileOut(handle, await handle.createWritable(), fw, fh, n, new TextEncoder().encode(spec).length * 2 + 4096);
    } catch (e) {
      if (e && e.name === 'AbortError') { REC.msg = 'not recorded: no file chosen'; showInfo(); }
      else recRefused(`the file could not be opened for writing: ${e && e.message || e}`);
      return;
    } finally { REC.starting = false; }
    if (disk) disk.file = true;   // (the disk storage for the ground truth's files alone)
  }
  if (disk && !disk.file) {
    REC.starting = true;
    let refused = '';
    try {
      await DISK.sweep(PLAY.disk ? [PLAY.disk.dir] : []);   // (the recording shown stays: a download may read it)
      recRoom = await DISK.room();
      refused = recProblem(recRoom);
    } catch (e) { disk = null; }
    finally { REC.starting = false; }
    if (refused) { recRefused(refused); return; }
  }
  syncRecMsg('');
  closePlayer();
  Object.assign(REC, { on: true, stop: false, live: liveRec, n, got: 0, store: null, disk, diskFrames: 0, out, desc: '',
    plan: plan ? { plan: JSON.parse(JSON.stringify(recPlan)), blocks: plan.blocks } : null, place: [],
    field: liveRec || !field.larger ? null : { size: fw, height: fh },
    exp: exposureSec(), t0: performance.now(), engineMs: 0,
    msg: gb > 2 ? `${gb.toFixed(1)} GB of frames, ` + (out ? `written to ${out.handle.name}` : disk ? 'written to this browser\'s disk storage' : 'held in this tab\'s memory') +
      (gb > 4 ? ' (saved as a BigTIFF)' : '') : '',
    log: null, chunks: [], pxUm: (+$('mv_pixel-nm').value || 100) / 1000 });
  // the settings as the Record begins (its file: web/live/settings_file.js recordFile)
  // (before the engine's option list has come, its values would be empty: the spec kept, the values made at the end)
  const over = REC.live ? { frames: REC.n } : { frames: REC.n, 'start-sec': recStartSec(), ...recFieldSpec() };
  REC.nowArgs = [over, movieSpec(over)];
  REC.now = self.iscSettings && self.ISC_ENGINE_OPTIONS ? self.iscSettings.current(...REC.nowArgs) : null;
  // A fresh sample's settings file is known now (a sample continued is known at its end); kept for the player.
  REC.file = null;
  try { if (!REC.live && REC.now) REC.file = self.iscSettings.recordFile({ mode: 'fresh', now: REC.now, frames: REC.n }, 'insiliscope-recording'); }
  catch (e) { console.warn('the recording\'s settings file:', e); }
  recGtEarly();
  iscTrack('record', { frames: iscTrack.framesBucket(REC.n), continue: REC.live, plan: !!REC.plan });
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
  const kind = await gtKind(movieSpec({ frames: REC.n, 'start-sec': recStartSec(), 'fresh-basis': 1, ...recFieldSpec() }));
  if (REC.file !== file || !REC.on) return;
  const min = Math.max(0, +o.minCore || 0), dyeCsv = kind === 'wf' && !!o.dyes;
  gtRun(JSON.stringify(file, null, 1), min, dyeCsv, { min, vol: GT_VOL, dyeCsv }, REC.disk && REC.disk.dir);
}
// The Record's field in the spec (none: the live field's size, no height)
const recFieldSpec = () => REC.field ? { size: REC.field.size, height: REC.field.height } : {};
// (before the Record: the box's field, the live sample's own)
const recFieldSpecOf = f => $('lv_recLive').checked ? { frames: 1 } : { frames: 1, ...(f.larger ? { size: f.w, height: f.h } : {}) };
function recMovie() {
  const w = worker();
  if (!w) { L.err = 'the workers are not up yet'; REC.on = false; syncButtons(); showInfo(); return; }
  REC.store = REC.out || (REC.disk ? null : frameStore());   // (on disk: made at the end; parts only if the storage failed)
  const job = { type: 'movie', id: ++L.id, spec: movieSpec({ frames: REC.n, 'start-sec': recStartSec(), 'fresh-basis': 1, ...recFieldSpec() }),
    fresh: true, stream: true, t0: performance.now(), ...(typeof gpuFlags === 'function' ? gpuFlags() : {}) };
  if (recOpfs()) job.disk = { path: DISK.path(REC.disk.dir, 'frames.raw') };
  if (REC.out) job.rowsOut = true;   // (strips' rows posted: the page writes them into the file)
  L.pending = job;
  w.postMessage(job);
}
function onRecProgress(d) {
  REC.got = Math.min(REC.n, Math.floor((d.frac || 0) * REC.n));
  if (d.info) REC.info = d.info;   // (the last frame's backends: tests/web/viewer_gpu_splat.mjs)
  syncButtons(); showInfo();
}
// a part of a fresh sample's frames, as the engine made them
function onRecPart(d) {
  if (!REC.on) return;
  if (!REC.store) REC.store = frameStore();   // (the disk storage failed: in memory)
  REC.store.add(d.frames, d.n);
  REC.got = Math.min(REC.n, REC.store.n);
}
// a part of a strip's rows (a Record straight to a file)
function onRecRows(d) {
  if (!REC.on || !REC.out) return;
  REC.out.rows(d.first, d.n, d.y0, d.rows, d.frames);
}
function onRecMovie(d, job) {
  if (!REC.on) return;
  const px = d.w * d.h;
  if (d.disk) REC.diskFrames = d.got;
  else {
    if (recOpfs()) { REC.disk = null; if (d.diskError) REC.msg = `frames kept in memory (the disk storage: ${d.diskError})` + (REC.msg ? ' · ' + REC.msg : ''); }
    if (!REC.store) REC.store = frameStore();
    if (!d.streamed) REC.store.add(d.frames, Math.min(REC.n, Math.floor(d.frames.length / Math.max(1, px))));
  }
  const n = Math.min(REC.n, d.disk ? d.got : REC.store.n);
  Object.assign(REC, { w: d.w, h: d.h, spec: d.spec, got: n, engineMs: performance.now() - job.t0,
    gpu: d.gpu && d.gpu !== 'CPU' ? d.gpu : '', remote: !!d.remote });
  if (d.stopped) REC.msg = `stopped: ${d.stopped}` + (REC.msg ? ' · ' + REC.msg : '');
  L.err = '';
  L.ms = n ? (performance.now() - REC.t0) / n : 0;
  L.frames++;
  if (n && (d.disk ? d.last : !REC.store.toFile)) draw({ ...d, frames: d.disk ? d.last : REC.store.frame(n - 1).slice() });
  finishRec();
}
function recNext() {
  if (!REC.live) { if (!REC.store) recMovie(); return; }
  if (REC.stop || REC.got >= REC.n) { finishRec(); return; }
  const side = Math.max(1, +$('mv_size').value || 128), cap = Math.max(1, Math.min(Math.floor(2e6 / (side * side)), 500));
  if (REC.plan) {   // a multi-D block: its frames left, its overrides (a chunk never spans two blocks)
    const { k, b } = recBlock();
    request(Math.max(1, Math.min(b.frames - k, cap)), b.ov);
    return;
  }
  request(Math.max(1, Math.min(REC.n - REC.got, cap)));
}
function onRecChunk(d, job) {
  const px = d.w * d.h, n = d.n || 1;
  if (!REC.got) {
    Object.assign(REC, { w: d.w, h: d.h, spec: d.spec, tStart: d.tStart !== undefined ? d.tStart : job.t, litStart: d.clock });
    if (recOpfs() && !d.disk) { REC.disk = null; REC.msg = `frames kept in memory (the disk storage: ${d.diskError || 'failed'})`; }
    if (REC.out) REC.store = REC.out;
    else if (!REC.disk) REC.store = frameStore();
  }
  if (d.w !== REC.w || d.h !== REC.h || REC.out && (d.w !== REC.out.w || d.h !== REC.out.h)) { REC.msg = 'the image size changed: stopped'; REC.stop = true; }
  else if (recOpfs() && !d.disk) { REC.msg = `stopped: ${d.diskError || 'the disk storage failed'}`; REC.stop = true; }
  else if (REC.out && REC.out.error) { REC.msg = `stopped: the file: ${REC.out.error.message || REC.out.error}`; REC.stop = true; }
  else {
    const k = Math.min(n, REC.n - REC.got);
    if (d.disk) REC.diskFrames = REC.got + k;   // (written at their place in the frames file: d.frames is the last)
    else REC.store.add(d.frames.subarray(0, k * px), k);   // (the chunk's frames, transferred: kept as they came)
    if (REC.plan) {   // each frame's sample time at its start (a frame lasts its exposure)
      const exp = recBlock().b.exposureMs / 1000;
      for (let i = 0; i < k; i++) REC.place.push({ t: +(job.t + i * exp).toFixed(9) });
    }
    REC.got += k;
    REC.chunks.push({ spec: job.spec, n: k, lit: true });
    REC.engineMs += d.workerMs || 0;
  }
  L.err = '';
  L.t = d.t; L.clock = d.clock; L.drift = d.drift; L.pattern = d.pattern || null; L.engine = d.engine; L.paths = d.paths || null;
  const s = (performance.now() - REC.t0) / 1000;
  L.ms = REC.got ? s * 1000 / REC.got : 0;
  L.frames++;
  draw({ ...d, frames: d.disk ? d.frames : d.frames.slice((n - 1) * px, n * px) });
  syncButtons();
  if (REC.stop || REC.got >= REC.n) finishRec(); else recNext();
}
async function finishRec() {
  const n = REC.got, s = (performance.now() - REC.t0) / 1000;
  REC.on = false;
  if (REC.out) {   // the tags written, the file cut and put in place, read back for the player
    const out = REC.out;
    REC.out = null;
    if (n) {
      try {
        REC.desc = recDesc(n);
        REC.store = diskStore(await out.finish(n, REC.desc, recPxUm()), REC.w, REC.h, n, out.at);
        REC.store.saved = out.handle.name;
      } catch (e) {
        REC.store = null;
        REC.msg = `the file ${out.handle.name} could not be written: ${e.message || e}`;
      }
    } else out.abort();
  }
  else if (n && REC.disk && REC.diskFrames) {
    try { REC.store = diskStore(await DISK.file(REC.disk.dir, 'frames.raw'), REC.w, REC.h, Math.min(n, REC.diskFrames)); }
    catch (e) { REC.store = null; REC.msg = `the recording's frames could not be read back: ${e.message || e}`; }
  }
  if (n && REC.store) {
    REC.msg = `recorded ${n} frames in ${s.toFixed(1)} s, ${(n / s).toFixed(1)} frames/s` +
      (REC.got < REC.n ? ` (stopped at ${n} of ${REC.n})` : '') +
      (REC.store.saved ? ` · written to ${REC.store.saved}` : '') +
      (REC.store.cropped ? ` · ${REC.w} × ${REC.h} px: the centre ${REC.store.vw} × ${REC.store.vh} shown, ` +
        (REC.store.saved ? 'the file has all of it' : 'Save writes all of it') : '') +
      (REC.msg ? ' · ' + REC.msg : '');
    openPlayer(n);
  }
  else gtStop();   // (no recording: a ground truth's run begun with it let go)
  REC.store = null;
  syncButtons();
}

// ---- a finished recording plays in the live view (looping at the frame interval); Save .tif saves it ----
const PLAY = L.play = { on: false, store: null, w: 0, h: 0, n: 0, desc: '', i: 0, run: false, timer: 0 };
// The recording's TIFF description (ImageJ's lines, the spec)
function recDesc(n) {
  const exp = REC.exp || exposureSec();
  return [`ImageJ=1.11a`, `images=${n}`, `frames=${n}`, `unit=micron`, `finterval=${exp}`, `loop=false`,
    ...(REC.plan ? ['insiliscope.plan=a multi-D Record: the frames in the order taken, index.csv says each one\'s position, z, channel and time'] : []),
    ...(REC.live ? [`insiliscope=live recording`, `insiliscope.spec=${REC.spec}`, `insiliscope.version=${self.ISC_VERSION || ''}`, `insiliscope.sample-time-sec=${REC.tStart}`,
      `insiliscope.lit-sec-at-start=${REC.litStart}`, `insiliscope.recorded=${new Date().toISOString()}`,
      'insiliscope.note=the frames continue the live sample (its light history and drift); insiliscope_cli --spec renders a fresh sample']
      : [`insiliscope=recording`, `insiliscope.spec=${REC.spec}`, `insiliscope.version=${self.ISC_VERSION || ''}`, `insiliscope.recorded=${new Date().toISOString()}`,
        'insiliscope.note=a fresh sample (a movie from start-sec, no history); insiliscope_cli --spec renders the same movie'])].join('\n') + '\n';
}
const recPxUm = () => REC.pxUm || (+$('mv_pixel-nm').value || 100) / 1000;
function openPlayer(n) {
  const exp = REC.exp || exposureSec();   // the exposure the frames were taken at
  const desc = REC.desc || recDesc(n);
  closePlayer(true);   // (a ground truth's run begun with the Record goes on)
  // the recording's settings file: the movie reproducible from it (insiliscope_cli --settings)
  if (!REC.now && self.iscSettings && self.ISC_ENGINE_OPTIONS && REC.nowArgs) REC.now = self.iscSettings.current(...REC.nowArgs);
  let settings = !REC.live && REC.file && n === REC.n ? REC.file : null;
  try {
    if (!settings && REC.now && self.iscSettings) settings = self.iscSettings.recordFile({ mode: REC.live ? 'live' : 'fresh', now: REC.now, frames: n,
      log: REC.log || [], chunks: REC.chunks, tSec: L.t }, 'insiliscope-recording');
  } catch (e) { console.warn('the recording\'s settings file:', e); }
  Object.assign(PLAY, { on: true, store: REC.store, disk: REC.disk, saved: REC.store.saved || '', w: REC.w, h: REC.h, n, desc, spec: REC.spec,
    exp, pxUm: recPxUm(), i: 0, run: n > 1, settings, gpu: REC.live ? '' : REC.gpu || '',
    plan: REC.plan ? { ...REC.plan, place: REC.place.slice(0, n), t0: REC.tStart } : null });
  $('lv_playPos').max = n - 1;
  $('lv_play').hidden = false;
  $('liveView').classList.add('playing');
  L.hiSet = false;
  showPlayFrame(0);
  if (PLAY.run) schedulePlay();
  syncPlay();
}
function showPlayFrame(i) {
  PLAY.i = i;
  const f = PLAY.store.frame(i), store = PLAY.store;
  const w = store.vw || PLAY.w, h = store.vh || PLAY.h;   // (a large field: its centre)
  if (f && f.then) f.then(a => { if (PLAY.on && PLAY.store === store && PLAY.i === i) draw({ w, h, spec: PLAY.spec, frames: a }); }).catch(() => {});
  else draw({ w, h, spec: PLAY.spec, frames: f });
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
  Object.assign(PLAY, { on: false, store: null, run: false });
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
  if (PLAY.plan) { openSavePlan(); return; }
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
    `<div class="msg" id="svMsg">Movie alone: ${PLAY.saved ? `already saved as ${esc(PLAY.saved)} while recording` : 'a .tif'}. ` +
    'Otherwise a .zip; drop its <code>open_in_napari.py</code> on napari ' +
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
    L.gtWarm(playSettingsText(), min, dyeCsv, { min, vol: GT_VOL, dyeCsv }, PLAY.disk && PLAY.disk.dir);
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
  // (a Record written straight to a file: that file is the TIFF)
  const tif = PLAY.saved ? PLAY.store.file : writeTiff16(PLAY.store.arrays(PLAY.n), PLAY.w, PLAY.h, PLAY.n, PLAY.desc, PLAY.pxUm);
  const bigTiff = tif.size >= 2 ** 32 - 2 ** 26 ? ', a BigTIFF: Fiji with Bio-Formats, napari or tifffile open it' : '';
  if (!c.settings && !c.gt && PLAY.saved) {
    REC.msg = `the movie is in ${PLAY.saved} already (written while recording)`;
    showInfo();
    return;
  }
  if (!c.settings && !c.gt) {
    download(tif, name + '.tif');
    REC.msg = `saved ${name}.tif (${(tif.size / 1e6).toFixed(1)} MB${bigTiff})`;
    showInfo();
    return;
  }
  const settingsText = playSettingsText();
  const entries = [{ name: 'movie.tif', data: tif }];   // (a Blob: past 1 GB stored as it is, zip.js)
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
      files = await L.groundTruth({ text: settingsText, min: c.minCore, vol: GT_VOL, dyeCsv: !!c.dyes, dir: PLAY.disk && PLAY.disk.dir },
        (f, stage, info) => { Object.assign(P, { f, stage: stage || 'frames', info }); show(); });
    } finally { clearInterval(tick); }
    for (const [n, b] of files) entries.push({ name: 'gt/' + n, data: b });
    // the converter only beside the dye list it reads (the stacks need none; the page is also in ☰ › Tools)
    if (files.some(([n]) => n === 'dyes.csv'))
      try { const r = await fetch('voxelize.html'); if (r.ok) entries.push({ name: 'gt/voxelize.html', data: await r.text() }); } catch (e) { /* offline: without it */ }
    say(`Packing ${Math.round(entries.reduce((s, e) => s + (e.data.size || e.data.length || 0), 0) / 1e6)} MB ...`, null);
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
  // (a recording on the disk storage: files past 64 MB stored as they are, streamed from disk, never read into memory)
  const blob = await self.iscZip.zip(entries, PLAY.disk || PLAY.saved ? { blobInline: 64e6 } : {});
  download(blob, name + '.zip');
  self.iscLastSave = { name: name + '.zip', entries: entries.map(e => e.name), blob };   // (the browser test reads it)
  REC.msg = `saved ${name}.zip (${(blob.size / 1e6).toFixed(1)} MB${bigTiff})`;
  showInfo();
}
// ---- a multi-D Record's Save (2026-11-23): always a .zip of the movie file(s) in the format chosen, index.csv (a row
// per page: position, z, channel, t, block, the sample's time, blank or not), the settings file if ticked, README.txt.
// OME-TIFF (default): one file, the frames as taken then one blank frame every blank plane's page points at (no copy);
// an Image per position, XYTZC. ImageJ: a hyperstack per position (C x Z x T, c fastest; ImageJ reads the pages back
// to back, so each file is a Blob of the frames in that order, blank frames where a cell is short). Flat: the frames
// as taken. No ground truth for a plan yet (a run of blocks is not one movie job).
const PLAN_FORMATS = [['ome', 'OME-TIFF', 'movie.ome.tif: one file, a series per position (C, Z, T); Fiji (Bio-Formats), napari, tifffile'],
  ['imagej', 'ImageJ hyperstack per position', '<position>.tif: C × Z × T each; Fiji / ImageJ open them as hyperstacks'],
  ['flat', 'One flat stack', 'movie.tif: every frame in the order taken; index.csv says what each page is']];
function openSavePlan() {
  const D = self.iscSettings;
  const fmt0 = store.get('saveFormat', 'ome'), name0 = `insiliscope_mda_${PLAY.w}x${PLAY.h}x${PLAY.n}`;
  if (!D || !D.dialog) { savePlan({ settings: false, format: fmt0 }, name0); return; }
  const L0 = self.ISC_MDA.layout(PLAY.plan.blocks, PLAY.n, PLAY.plan.plan.positions);
  const box = D.dialog('<h3>Save the multi-D recording</h3>' +
    '<div class="row"><label for="svName">Name</label><input type="text" id="svName" maxlength="80"></div>' +
    `<div class="row"><label for="svFmt">Format</label><select id="svFmt">${PLAN_FORMATS.map(([v, t]) => `<option value="${v}">${esc(t)}</option>`).join('')}</select></div>` +
    '<div class="msg" id="svFmtNote"></div>' +
    `<label class="part"><input type="checkbox" checked disabled> <b>Index</b><span class="s">index.csv, a row per page: position, z, channel, t, block, time, blank</span></label>` +
    '<label class="part"><input type="checkbox" id="svSet"> <b>Settings file</b><span class="s">settings.isc.json: the settings, the plan and the ' +
    'sample\'s light (☰ › Load)</span></label>' +
    `<div class="msg">${L0.P.length} position${L0.P.length > 1 ? 's' : ''} × ${L0.Z.length} z × ${L0.C.length} channel${L0.C.length > 1 ? 's' : ''} × up to ${L0.T} frames` +
    ` (${PLAY.n} recorded${PLAY.n < PLAY.plan.blocks.reduce((s, b) => s + b.frames, 0) ? ', stopped early' : ''}). No ground truth for a plan yet. Saved as a .zip.</div>` +
    '<progress id="svBar" max="1" hidden style="width:100%"></progress>' +
    '<div class="row"><span class="sp"></span><button id="svCancel">Cancel</button><button id="svGo">Save</button></div>');
  const q = s => box.querySelector(s);
  q('#svName').value = name0;
  q('#svFmt').value = PLAN_FORMATS.some(f => f[0] === fmt0) ? fmt0 : 'ome';
  const note = () => { q('#svFmtNote').textContent = PLAN_FORMATS.find(f => f[0] === q('#svFmt').value)[2]; };
  q('#svFmt').addEventListener('change', note);
  note();
  q('#svSet').checked = saveOpts().settings && !!PLAY.settings;
  q('#svSet').disabled = !PLAY.settings;
  q('#svCancel').onclick = () => D.closeDialog();
  q('#svGo').onclick = async () => {
    const c = { settings: q('#svSet').checked, format: q('#svFmt').value };
    store.set('saveFormat', c.format);
    const name = (q('#svName').value.trim() || name0).replace(/[\\/:*?"<>|]+/g, '_');
    q('#svGo').disabled = true;
    try {
      await savePlan(c, name, t => { q('#svFmtNote').textContent = t; q('#svBar').hidden = false; q('#svBar').removeAttribute('value'); });
      D.closeDialog();
    } catch (e) {
      q('#svBar').hidden = true;
      q('#svFmtNote').innerHTML = `<span class="warn">${esc(e.message || e)}</span>`;
      q('#svGo').disabled = false;
    }
  };
}
// c = {settings, format}
async function savePlan(c, name, say = () => {}) {
  const M = self.ISC_MDA, P = PLAY.plan, n = PLAY.n, px = PLAY.w * PLAY.h, st = PLAY.store;
  iscTrack('save-tiff', { source: 'record', frames: iscTrack.framesBucket(n), settings: !!c.settings, gt: false, plan: c.format });
  const Lay = M.layout(P.blocks, n, P.plan.positions), files = M.files(Lay, c.format), blank = new Uint16Array(px);
  const extra = [`insiliscope.version=${self.ISC_VERSION || ''}`, `insiliscope.recorded=${new Date().toISOString()}`,
    'insiliscope.plan=a multi-D Record: index.csv says each page\'s position, z, channel, time'];
  say('Packing ...');
  const entries = [];
  for (const f of files) {
    let tif;
    if (c.format === 'ome') {
      const lay = M.pagedLayout(f, n, px);
      tif = writeTiffPaged([...st.arrays(n), blank], lay.dataBytes, PLAY.w, PLAY.h, f.pages.length,
        M.omeXml(Lay, P.blocks, f, { w: PLAY.w, h: PLAY.h, pxUm: PLAY.pxUm, version: self.ISC_VERSION || '', date: new Date().toISOString().slice(0, 19),
          place: P.place, t0: P.place.length ? P.place[0].t : 0 }), PLAY.pxUm, lay.pageAt);
    } else if (c.format === 'imagej') {
      const parts = M.pageParts(f, i => st.part(i), blank);
      tif = writeTiff16(parts, PLAY.w, PLAY.h, parts.length, M.imagejDesc(Lay, [`insiliscope.position=${Lay.P[f.p].name}`, ...extra]), PLAY.pxUm);
    } else tif = writeTiff16(st.arrays(n), PLAY.w, PLAY.h, n, PLAY.desc, PLAY.pxUm);
    entries.push({ name: f.name, data: tif });
  }
  entries.push({ name: 'index.csv', data: M.indexCsv(Lay, P.blocks, files, P.place) });
  if (c.settings && PLAY.settings) entries.push({ name: 'settings.isc.json', data: playSettingsText() });
  entries.push({ name: 'README.txt', data: `inSiliScope multi-D recording ${name}, saved ${new Date().toISOString()} (viewer ${self.ISC_VERSION || ''}).\n\n` +
    `${n} frames of ${PLAY.w} x ${PLAY.h} px, 16-bit, ${+(PLAY.pxUm * 1000).toFixed(2)} nm pixels, one continuing live sample.\n` +
    `${Lay.P.length} position(s) x ${Lay.Z.length} z x ${Lay.C.length} channel(s) x up to ${Lay.T} frames; blank planes where a ` +
    'channel has fewer frames (index.csv: padding 1).\n\n' +
    files.map(f => `${f.name.padEnd(18)} ${f.pages.length} pages`).join('\n') + '\n' +
    'index.csv          a row per page of every file: position, x/y/z (um), z index, channel, t, block, frame in block, frame taken, ' +
    'exposure (ms), the sample\'s time (s), padding, the block\'s settings\n' +
    'channels:\n' + Lay.C.map((ch, i) => `  ${i}: ${ch.name}`).join('\n') + '\n' +
    (c.settings && PLAY.settings ? 'settings.isc.json  the settings, the plan and the sample\'s light: ☰ › Load in the viewer\n' : '') });
  const blob = await self.iscZip.zip(entries, PLAY.disk ? { blobInline: 64e6 } : {});
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
  if (open) syncPlan();
  syncRecField();
  if (open && DISK.ok) DISK.room().then(r => { recRoom = r; if (!$('lv_recPop').hidden) syncRecMsg(); }).catch(() => {});
  if (open) { $('lv_recN').focus(); $('lv_recN').select(); }
};
$('lv_rec').addEventListener('click', () => { if (REC.on) startRec(); else recPop($('lv_recPop').hidden); });
$('lv_recGo').addEventListener('click', () => { recPop(false); startRec(); });
$('lv_recPop').addEventListener('keydown', ev => {
  if (ev.key === 'Enter') { ev.preventDefault(); for (const id of ['lv_recN', 'lv_recT', 'lv_recW', 'lv_recH']) $(id).dispatchEvent(new Event('change')); recPop(false); startRec(); }
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
  clearTimeout(L.reqTimer);
  if (on) { closePlayer(); L.lastShown = 0; L.nextDue = 0; L.fps = 0; L.worldDrift = null; REC.msg = ''; request(); }
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
// a new exposure takes effect at the next frame (a long one's request is not waited for)
$('mv_exposure-ms').addEventListener('change', () => { if (L.on && !L.pending && !REC.on) { clearTimeout(L.reqTimer); L.nextDue = 0; request(); } });
document.addEventListener('visibilitychange', () => { if (!document.hidden && L.on && !L.pending) { L.nextDue = 0; request(); } });
window.liveRedraw = () => { paint(); drawScale(); };
})();
