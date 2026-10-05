// SPDX-License-Identifier: BSD-3-Clause
// The viewer's Animation tab (issue 11): an editor for animation sequences (web/anim/sequence.js), a timeline under
// the view that previews them (the view's camera and layers follow the animation), and the export to MP4, WebM or GIF
// (frame i rendered off screen at t = i / fps: web/index.html captureFrame, web/encode/*). The sequence is kept in
// local storage, saved and opened as .json files, with undo/redo. Test hook: window.iscAnim.
'use strict';
(function () {
const S = window.iscScene, A = window.IscAnimSeq, E = window.IscEncode, UI = window.uiKit;
if (!S || !A || !UI) return;
const R = S.registry, $ = id => document.getElementById(id), h = UI.h;
const LS = 'isc.anim.v1';

// ---- styles (the panel's own; the tab bar is in index.html) ----
document.head.append(h('style', {}, `
#animPanel .an-sec{margin:10px 0 4px;font-size:10px;font-weight:600;letter-spacing:.08em;text-transform:uppercase;color:var(--muted);display:flex;align-items:center;gap:6px}
#animPanel .an-sec span{flex:1}
#animPanel .an-row{display:grid;grid-template-columns:var(--lab-w) minmax(0,1fr);align-items:center;gap:6px;margin:var(--row-gap) 0}
#animPanel .an-row .ctl{display:flex;gap:4px;align-items:center;min-width:0}
#animPanel .an-row .ctl>*{min-width:0}
#animPanel .an-row input[type=number]{width:64px}
#animPanel .an-row input[type=text]{width:100%}
#animPanel .an-row select{max-width:100%}
#animPanel .btns{display:flex;flex-wrap:wrap;gap:4px;margin:4px 0}
#animPanel .btns button{padding:3px 7px}
#animPanel button.mini{padding:1px 5px;font-size:11px;line-height:1.3}
#animPanel details.an-card{border:1px solid var(--border);border-radius:var(--radius);margin:6px 0;background:var(--grp-bg)}
#animPanel details.an-card>summary{cursor:pointer;padding:4px 6px;list-style:none;display:flex;align-items:center;gap:4px;background:var(--grp-head);border-radius:var(--radius)}
#animPanel details.an-card>summary::-webkit-details-marker{display:none}
#animPanel details.an-card>summary .t{flex:1;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
#animPanel details.an-card>summary .t b{font-weight:600}
#animPanel details.an-card .body{padding:4px 8px 6px}
#animPanel details.an-card.step{margin-left:6px;border-color:color-mix(in srgb,var(--border) 70%,transparent)}
#animPanel details.an-card.sel>summary{box-shadow:inset 3px 0 0 var(--accent)}
#animPanel table.layers{width:100%;border-collapse:collapse;margin:4px 0}
#animPanel table.layers th{font-weight:500;color:var(--muted);font-size:10px;text-align:center;padding:1px}
#animPanel table.layers th:first-child{text-align:left}
#animPanel table.layers td{padding:1px 2px;text-align:center}
#animPanel table.layers td:first-child{text-align:left;max-width:120px}
#animPanel table.layers select{max-width:120px}
#animPanel table.layers input[type=color]{width:22px;height:18px;padding:0;border:none;background:none}
#animPanel table.layers input[type=number]{width:44px}
#animPanel .warn{color:#e6a23c;font-size:11px;margin:4px 0}
#animPanel .note{color:var(--muted);font-size:11px;margin:4px 0;line-height:1.4}
#animPanel progress{width:100%}
#animBar{background:rgba(10,13,18,.96);border-top:1px solid #232a35;align-items:center;gap:8px;padding:0 10px;color:#dfe6ee;font-size:11px}
#animBar button{background:#1a212b;border-color:#2c3644;color:#dfe6ee;padding:3px 9px}
#animBar select{background:#1a212b;border-color:#2c3644;color:#dfe6ee}
#animBar canvas{flex:1;height:30px;cursor:pointer;background:transparent}
#animBar .tm{font-variant-numeric:tabular-nums;min-width:86px;text-align:right}
`));

// ---- state ----
let seq = null, C = null, target = null, frozenTarget = null, t = 0, playing = false, raf = 0, last = 0, mode = 'off';
const undo = [], redo = [];
let lastSnap = '', exportJob = null, caps = null, selected = { cycleId: null, stepId: null };
function load() {
  try { const s = localStorage.getItem(LS); if (s) return A.migrate(s).seq; } catch (e) { /* a broken entry: start fresh */ }
  return A.starter();
}
const save = () => { try { localStorage.setItem(LS, JSON.stringify(seq)); } catch (e) { /* no storage */ } };
function snapshot() { const s = JSON.stringify(seq); if (s !== lastSnap) { undo.push(lastSnap); if (undo.length > 100) undo.shift(); redo.length = 0; lastSnap = s; } }
function restore(s) { seq = A.migrate(s).seq; lastSnap = JSON.stringify(seq); changed(true); }

// ---- compile and evaluate ----
const outAspect = () => seq.output.width / seq.output.height;
function resolveTarget() {
  const tg = seq.scene.target || {};
  if (tg.mode === 'pick' && tg.cell) return tg.cell;
  if (frozenTarget) return frozenTarget;
  const v = S.getView();
  return S.nearestCell(v.cx, v.cy);
}
let boundsPrecise = false;
function compile(aspect) {
  target = resolveTarget();
  const b = target ? S.cellBounds(target) : null;
  boundsPrecise = !!(b && b.precise);
  return A.compileSequence(seq, { registry: R, bounds: b, aspect: aspect || outAspect(), target });
}
// A frame of the animation as the renderer's scene state.
function toState(f, screen) {
  const layers = {};
  for (const id in f.layers) layers[id] = Object.assign({}, f.layers[id], { style: Object.assign({}, seq.styles[id] || {}, f.layers[id].style || {}) });
  const camera = Object.assign({}, f.camera);
  if (screen) camera.frameAspect = outAspect();
  return { camera, scope: f.scope, target: f.target, detailCells: f.detailCells, theme: f.theme, ghostOpacity: f.ghostOpacity,
    zClip: null, sweep: f.sweep, slice: f.slice, srFrame: f.srFrame, data: seq.data, layers, grid: false, xz: false, ui: false, overlays: f.overlays };
}
const loopMode = () => (seq.output.loop || 'loop');
function frameAt(tt) { return C && C.steps.length ? A.evalCompiled(C, tt, { loop: playing ? loopMode() : 'once' }) : null; }

// ---- overlays: caption, readout, legend, scale bar (on the preview and on every exported frame) ----
function drawOverlays(g, r, f, cam, k, ov) {
  if (!f) return;
  const o = Object.assign({}, seq.output.overlays, ov || {});
  const fs = Math.max(11, Math.round(r.h / 26));
  g.save();
  g.beginPath(); g.rect(r.x, r.y, r.w, r.h); g.clip();
  g.textBaseline = 'alphabetic';
  if (o.captions && f.overlays.caption && f.overlays.caption.alpha > 0) {
    const c = f.overlays.caption;
    g.globalAlpha = c.alpha;
    g.font = `600 ${Math.round(fs * 1.25)}px system-ui, Segoe UI, sans-serif`;
    g.textAlign = 'left'; g.fillStyle = '#ffffff'; g.shadowColor = 'rgba(0,0,0,0.8)'; g.shadowBlur = 6 * k;
    g.fillText(c.text, r.x + fs, r.y + fs * 1.9);
    g.globalAlpha = 1; g.shadowBlur = 0;
  }
  if (o.readout && f.overlays.readout) {
    const ro = f.overlays.readout, name = { z: 'z', x: 'x', y: 'y', h: 'plane', d: 'plane' }[ro.axis] || 'plane';
    g.font = `${fs}px system-ui, Segoe UI, sans-serif`; g.textAlign = 'right'; g.fillStyle = 'rgba(223,230,238,0.9)';
    g.fillText(ro.axis === 'z' ? `z = ${ro.pos.toFixed(2)} µm` : `${name} ${ro.pos.toFixed(1)} µm`, r.x + r.w - fs, r.y + fs * 1.9);
  }
  if (o.legend) {
    g.font = `${Math.round(fs * 0.85)}px system-ui, Segoe UI, sans-serif`; g.textAlign = 'left';
    let y = r.y + r.h - fs * 0.9;
    const ids = Object.keys(f.layers).reverse();
    for (const id of ids) {
      const L = R.layers.get(id), st = IscScene.resolveStyle(f.theme, id, seq.styles[id]), c = st.color || [200, 200, 200];
      g.fillStyle = `rgb(${c[0]},${c[1]},${c[2]})`; g.fillRect(r.x + fs, y - fs * 0.7, fs * 0.7, fs * 0.7);
      g.fillStyle = 'rgba(223,230,238,0.9)'; g.fillText(L ? L.label : id, r.x + fs * 2, y);
      y -= fs * 1.2;
    }
  }
  if (o.scaleBar) { g.translate(r.x, r.y); S.scaleBar(g, r.w, r.h, cam.S, k); }
  g.restore();
}
S.setAfterDraw((g, st, cam) => {
  if (mode !== 'preview' || !lastFrame) return;
  drawOverlays(g, S.frameRect(cam, outAspect(), st.camera.fovUm), lastFrame, cam, devicePixelRatio);
});

// ---- preview ----
let lastFrame = null;
// Data an animation needs is prepared by itself: when a sequence that shows image layers is previewed or edited and
// its target cell lacks them (once per sequence data, target and settings; Prepare retries by hand).
const autoTried = new Set();
function autoPrepare() {
  if (preparing || exportJob || !C || !S.packingIdle()) return;
  const need = A.requiredData(C, R).map(n => n.layer).filter(id => S.dataKinds[id]), key = frozenTarget || resolveTarget();
  if (!need.length || !key) return;
  const st = S.dataStatus(need, key, seq.data);
  if (!st.length || st.every(x => x.ready)) return;
  const k = key + '|' + need.join(',') + '|' + JSON.stringify(seq.data);
  if (autoTried.has(k)) return;
  autoTried.add(k);
  prepare().catch(e => console.warn('auto-prepare:', e.message || e));
}
function show() {
  // while the cells around the view are still being packed, the cell nearest the centre may still change
  if (!S.packingIdle() && (seq.scene.target || {}).mode !== 'pick') { frozenTarget = null; frozenTarget = resolveTarget(); C = compile(); }
  if (!C) C = compile();
  if (!boundsPrecise && target) { const b = S.cellBounds(target); if (b && b.precise) C = compile(); }
  lastFrame = frameAt(t);
  if (!lastFrame) { S.setPreview(null); drawBar(); return; }
  autoPrepare();
  S.setPreview(toState(lastFrame, true));
  drawBar();
}
function enterPreview() {
  if (mode === 'preview') return;
  frozenTarget = resolveTarget();
  mode = 'preview';
  C = compile();
  show();
}
function exitPreview() {
  stop();
  if (mode !== 'preview') return;
  mode = 'free'; frozenTarget = null;
  S.setPreview(null);
  drawBar();
}
function play() {
  enterPreview();
  if (!C || !C.duration) return;
  if (t >= C.duration - 1e-6 && loopMode() === 'once') t = 0;
  playing = true; S.setPlaying(true); last = performance.now();
  const tick = now => {
    if (!playing) return;
    t += (now - last) / 1000; last = now;
    if (loopMode() === 'once' && t >= C.duration) { t = C.duration - 1e-6; stop(); show(); return; }
    show();
    raf = requestAnimationFrame(tick);
  };
  raf = requestAnimationFrame(tick);
  drawBar();
}
function stop() { playing = false; S.setPlaying(false); cancelAnimationFrame(raf); drawBar(); }
function seek(tt) {
  enterPreview();
  const D = C ? C.duration : 0, span = loopMode() === 'pingpong' ? 2 * D : D;
  t = Math.max(0, Math.min(Math.max(0, span - 1e-6), tt));
  show();
}

// ---- the timeline bar ----
const bar = $('animBar');
bar.append(
  h('button', { id: 'an_play', title: 'Play / pause (Space)' }, '▶'),
  h('select', { id: 'an_loop', title: 'Playback: once, loop, or forward then backward' }),
  h('canvas', { id: 'an_track' }),
  h('span', { class: 'tm', id: 'an_time' }),
  h('button', { id: 'an_exit', title: 'Back to the free view (Esc)' }, 'Free view'));
for (const [v, l] of [['once', 'once'], ['loop', 'loop'], ['pingpong', 'ping-pong']]) $('an_loop').append(h('option', { value: v }, l));
const track = $('an_track');
function drawBar() {
  if (!seq) return;
  $('an_play').textContent = playing ? '❚❚' : '▶';
  $('an_loop').value = loopMode();
  const D = C ? C.duration : 0, span = loopMode() === 'pingpong' ? 2 * D : D;
  $('an_time').textContent = `${t.toFixed(2)} / ${span.toFixed(2)} s`;
  $('an_exit').style.visibility = mode === 'preview' ? 'visible' : 'hidden';
  const dpr = devicePixelRatio, W = Math.round(track.clientWidth * dpr), H = Math.round(track.clientHeight * dpr);
  if (!W || !H) return;
  if (track.width !== W || track.height !== H) { track.width = W; track.height = H; }
  const g = track.getContext('2d');
  g.clearRect(0, 0, W, H);
  if (!C || !span) return;
  const X = tt => (tt / span) * W;
  const hues = [205, 35, 140, 290, 0, 95];
  const segs = loopMode() === 'pingpong' ? C.steps.concat(C.steps.map(s => ({ ...s, t0: 2 * D - s.t1, t1: 2 * D - s.t0, back: true }))) : C.steps;
  for (const s of segs) {
    const x0 = X(s.t0), x1 = X(s.t1), sel = s.stepId === selected.stepId;
    g.fillStyle = `hsla(${hues[s.ci % hues.length]},55%,${sel ? 55 : 42}%,${s.back ? 0.45 : 0.85})`;
    g.fillRect(x0 + 1, H * 0.3, Math.max(1, x1 - x0 - 2), H * 0.45);
    if (s.sweep && x1 - x0 > 14 * dpr) {
      g.fillStyle = 'rgba(255,255,255,0.8)'; g.font = `${10 * dpr}px system-ui`; g.textAlign = 'center'; g.textBaseline = 'middle';
      g.fillText((s.sweep.dir > 0) !== !!s.back ? '↑' : '↓', (x0 + x1) / 2, H * 0.53);
    }
  }
  const tp = loopMode() === 'loop' ? ((t % D) + D) % D : t;
  g.fillStyle = '#ffffff'; g.fillRect(X(tp) - dpr, 0, 2 * dpr, H);
}
let scrubbing = false;
const trackT = e => { const r = track.getBoundingClientRect(), D = C ? C.duration : 0, span = loopMode() === 'pingpong' ? 2 * D : D; return Math.max(0, Math.min(1, (e.clientX - r.left) / r.width)) * span; };
track.addEventListener('pointerdown', e => {
  if (!C) C = compile();
  scrubbing = true; track.setPointerCapture(e.pointerId); stop();
  let tt = trackT(e);
  if (e.shiftKey && C) tt = nearestBoundary(tt);
  seek(tt);
});
track.addEventListener('pointermove', e => { if (scrubbing) { let tt = trackT(e); if (e.shiftKey) tt = nearestBoundary(tt); seek(tt); } });
track.addEventListener('pointerup', () => { scrubbing = false; });
function nearestBoundary(tt) { let best = 0; for (const s of C.steps) for (const b of [s.t0, s.t1]) if (Math.abs(b - tt) < Math.abs(best - tt)) best = b; return best; }
$('an_play').addEventListener('click', () => (playing ? stop() : play()));
$('an_exit').addEventListener('click', exitPreview);
$('an_loop').addEventListener('change', e => { seq.output.loop = e.target.value; changed(false, true); });
new ResizeObserver(drawBar).observe(track);

// ---- tabs ----
function setTab(tab) {
  const anim = tab === 'anim';
  $('panel').hidden = anim; $('animPanel').hidden = !anim;
  for (const b of $('tabs').querySelectorAll('button')) b.classList.toggle('on', b.dataset.tab === tab);
  UI.store.set('tab', tab);
  if (anim) { if (mode === 'off') mode = 'free'; render(); S.setAnimBar(true); drawBar(); if (!C) C = compile(); setTimeout(autoPrepare, 600); }
  else { exitPreview(); mode = 'off'; S.setAnimBar(false); }
}
$('tabs').addEventListener('click', e => { const b = e.target.closest('button'); if (b) setTab(b.dataset.tab); });

// ---- editing ----
// changed(structural): the sequence was edited (undo snapshot on commit), recompile, refresh the preview and the panel.
function changed(structural, commit = true) {
  if (commit) snapshot();
  save();
  C = compile();
  if (structural) render(); else refreshSummaries();
  if (mode !== 'off') setTimeout(autoPrepare, 600);
  if (mode === 'preview') show(); else drawBar();
}
const findCycle = id => seq.cycles.find(c => c.id === id);
function move(arr, i, d) { const j = i + d; if (j < 0 || j >= arr.length) return; [arr[i], arr[j]] = [arr[j], arr[i]]; }

// small field builders (no element ids: web/lab/lab_html.js restores inputs by id)
function row(label, tip, ...ctl) {
  const r = h('div', { class: 'an-row' });
  r.append(UI.labelCell({ label, tip }, null), h('div', { class: 'ctl' }));
  r.lastChild.append(...ctl);
  return r;
}
function numIn(get, set, o) {
  const e = h('input', Object.assign({ type: 'number', step: 'any' }, o || {}));
  const v = get(); e.value = v === undefined || v === null ? '' : v;
  e.addEventListener('input', () => { set(e.value === '' ? null : +e.value); changed(false, false); });
  e.addEventListener('change', () => changed(false));
  return e;
}
function textIn(get, set, o) {
  const e = h('input', Object.assign({ type: 'text' }, o || {}));
  e.value = get() || '';
  e.addEventListener('input', () => { set(e.value); changed(false, false); });
  e.addEventListener('change', () => changed(false));
  return e;
}
function selIn(options, get, set, structural, o) {
  const e = h('select', o || {});
  for (const [v, l, dis] of options) e.append(h('option', { value: v, disabled: !!dis }, l));
  e.value = get();
  e.addEventListener('change', () => { set(e.value); changed(!!structural); });
  return e;
}
function checkIn(get, set, structural) {
  const e = h('input', { type: 'checkbox' });
  e.checked = !!get();
  e.addEventListener('change', () => { set(e.checked); changed(!!structural); });
  return e;
}
const btn = (label, title, fn, cls) => { const b = h('button', { title, class: cls || '' }, label); b.addEventListener('click', e => { e.preventDefault(); e.stopPropagation(); fn(); }); return b; };
const hex = c => '#' + c.map(v => Math.round(v).toString(16).padStart(2, '0')).join('');

// layer refs a step of a cycle bound to `structure` can use: [ref, label, disabled]
function layerOptions(structure) {
  const out = [], s = R.structures.get(structure);
  if (s) {
    out.push(['$.gt', `${s.label}: simulated`]);
    for (const id of s.layers) {
      const L = R.layers.get(id);
      if (L.rep === s.primary) continue;
      out.push(['$.' + L.rep, `${s.label}: ${L.label.replace(s.label + ' ', '')}${L.implemented ? '' : ' (coming)'}`, !L.implemented]);
    }
  }
  for (const L of R.layers.values()) if (L.structure !== structure && L.implemented) out.push([L.id, L.label]);
  return out;
}
const layerLabel = (ref, structure) => { const id = A.bind(ref, structure, R), L = R.layers.get(id); return L ? L.label : id; };

const AXES = [['none', 'no sweep (all at once)'], ['z', 'z (up / down)'], ['x', 'x'], ['y', 'y'], ['h', 'screen horizontal'], ['d', 'screen depth']];
const EASES = [['linear', 'linear'], ['inOut', 'ease in-out'], ['in', 'ease in'], ['out', 'ease out'], ['smooth', 'smooth']];
const POS = [['0', 'start (bottom)'], ['0.5', 'middle'], ['1', 'end (top)']];
function stepSummary(cy, st) {
  const sw = st.sweep && st.sweep.axis && st.sweep.axis !== 'none' ? st.sweep : null;
  const dir = sw ? ((sw.to && sw.to.rel !== undefined ? sw.to.rel : 1) >= (sw.from && sw.from.rel !== undefined ? sw.from.rel : 0) ? '↑' : '↓') : '';
  return `${(+st.duration || 0).toFixed(1)} s · ` + (sw ? `sweep ${sw.axis} ${dir}${sw.slab ? `, slab ${sw.slab} µm` : ''}` : 'all at once') +
    (st.caption ? ` · “${st.caption}”` : '');
}
function cycleSummary(cy) {
  const d = (cy.steps || []).reduce((s, st) => s + (+st.duration || 0), 0), s = R.structures.get(cy.structure);
  return `${(s && s.label) || cy.structure} · ${d.toFixed(1)} s`;
}
const sums = new Map();   // element -> () => text
function refreshSummaries() { for (const [el, fn] of sums) el.textContent = fn(); }

function stepCard(cy, st, si) {
  const det = h('details', { class: 'an-card step' + (selected.stepId === st.id ? ' sel' : '') });
  det.open = !!openState.get(st.id);
  det.addEventListener('toggle', () => openState.set(st.id, det.open));
  const sum = h('summary'), title = h('span', { class: 't' });
  title.append(h('b', {}, `Step ${si + 1}`), document.createTextNode(' '));
  const ts = h('span'); sums.set(ts, () => stepSummary(cy, st)); ts.textContent = stepSummary(cy, st);
  title.append(ts);
  sum.append(title,
    btn('▶', 'Preview from this step', () => { selected = { cycleId: cy.id, stepId: st.id }; if (!C) C = compile(); const S0 = C.steps.find(x => x.stepId === st.id); seek(S0 ? S0.t0 : 0); play(); }, 'mini'),
    btn('↑', 'Move up', () => { move(cy.steps, si, -1); changed(true); }, 'mini'),
    btn('↓', 'Move down', () => { move(cy.steps, si, 1); changed(true); }, 'mini'),
    btn('⧉', 'Duplicate', () => { const c = A.clone(st); c.id = A.newId('s'); cy.steps.splice(si + 1, 0, c); changed(true); }, 'mini'),
    btn('✕', 'Delete', () => { cy.steps.splice(si, 1); changed(true); }, 'mini'));
  sum.addEventListener('click', e => { if (e.target === sum || e.target.closest('.t')) { selected = { cycleId: cy.id, stepId: st.id }; if (C) { const S0 = C.steps.find(x => x.stepId === st.id); if (S0 && mode === 'preview') seek(S0.t0); } drawBar(); } });
  det.append(sum);
  const body = h('div', { class: 'body' });
  st.camera = st.camera || {};
  const cam = st.camera;
  body.append(
    row('Duration', 'Length of this step (s).', numIn(() => st.duration, v => { st.duration = Math.max(0.1, v || 0.1); }, { min: 0.1, step: 0.5 }), h('span', { class: 'u' }, 's')),
    row('Caption', 'Text shown on the frame during this step (fades in and out). Empty = none.', textIn(() => st.caption, v => { st.caption = v; })),
    row('Rotation', 'How the view turns during this step: by the cycle\'s orbit, to an angle, or by an amount (°).',
      selIn([['orbit', 'by the orbit'], ['to', 'to (°)'], ['by', 'by (°)']], () => (cam.az && cam.az.to !== undefined && cam.az.to !== null ? 'to' : cam.az && cam.az.by !== undefined && cam.az.by !== null ? 'by' : 'orbit'),
        v => { cam.az = v === 'orbit' ? undefined : { [v]: 0, ease: 'inOut' }; }, true),
      ...(cam.az && (cam.az.to !== undefined || cam.az.by !== undefined) ? [numIn(() => (cam.az.to !== undefined ? cam.az.to : cam.az.by), v => { if (cam.az.to !== undefined) cam.az.to = v || 0; else cam.az.by = v || 0; })] : [])),
    row('Tilt to', 'Tilt at the end of this step (0 = from above, 90 = from the side). Empty: keep the tilt.', numIn(() => cam.tilt && cam.tilt.to, v => { cam.tilt = v === null ? undefined : { to: v, ease: 'inOut' }; }, { min: 0, max: 90, step: 5 }), h('span', { class: 'u' }, '°')),
    row('Zoom to', 'Zoom at the end of this step: the cell fills 1/fit of the frame (1 = just fits, 2 = half). Empty: keep the zoom.', numIn(() => cam.fit && cam.fit.to, v => { cam.fit = v === null ? undefined : { to: v, ease: 'inOut' }; }, { min: 0.1, step: 0.05 }), h('span', { class: 'u' }, 'fit')),
    h('div', { class: 'btns' }, ''),
  );
  body.lastChild.append(btn('Use current view', 'Set this step\'s end tilt, rotation and zoom from the view (set it up in the free view first)', () => {
    const v = S.getView(), b = target && S.cellBounds(target);
    cam.tilt = { to: v.tilt, ease: 'inOut' }; cam.az = { to: v.az, ease: 'inOut' };
    if (b) { const Cx = compile(), fov = Math.min(v.cssW / v.scale, v.cssH / v.scale * outAspect()); cam.fit = { to: +(fov / Cx.fovOf(1, v.tilt)).toFixed(3), ease: 'inOut' }; }
    changed(true);
  }));
  // sweep
  const sw = st.sweep && st.sweep.axis && st.sweep.axis !== 'none' ? st.sweep : null;
  body.append(row('Sweep', 'A plane moving through the cell during this step; each layer below is shown ahead of it, inside its slab and/or behind it (its wake). Screen axes turn with the view.',
    selIn(AXES, () => (sw ? sw.axis : 'none'), v => { st.sweep = v === 'none' ? null : Object.assign({ from: { rel: 0 }, to: { rel: 1 }, slab: 0, ease: 'inOut' }, sw || {}, { axis: v }); }, true)));
  if (sw) {
    const posSel = (key, def) => {
      const p = sw[key] || { rel: def };
      return [selIn(POS, () => String(p.rel !== undefined ? p.rel : def), v => { sw[key] = Object.assign({}, sw[key] || {}, { rel: +v }); }),
        numIn(() => p.um || 0, v => { sw[key] = Object.assign({ rel: def }, sw[key] || {}, { um: v || 0 }); }, { step: 0.1, title: 'offset (µm)' }), h('span', { class: 'u' }, 'µm')];
    };
    body.append(
      row('From', 'Where the plane starts: the start, middle or end of the cell along the axis, plus an offset.', ...posSel('from', 0)),
      row('To', 'Where the plane ends.', ...posSel('to', 1)),
      row('Slab', 'Thickness of the slab around the plane (µm); 0 = a plane (nothing is "in the slab").', numIn(() => sw.slab || 0, v => { sw.slab = Math.max(0, v || 0); }, { min: 0, step: 0.1 }), h('span', { class: 'u' }, 'µm')),
      row('Easing', 'How the plane speeds up and slows down.', selIn(EASES, () => sw.ease || 'linear', v => { sw.ease = v; })));
  }
  body.append(row('Fade', 'Layers that appear or disappear at this step fade over this time (s).', numIn(() => (st.fadeSec !== undefined ? st.fadeSec : sw ? 0 : 0.3), v => { st.fadeSec = Math.max(0, v || 0); }, { min: 0, step: 0.1 }), h('span', { class: 'u' }, 's')));
  // layers
  const tb = h('table', { class: 'layers' }), head = h('tr');
  head.append(h('th', {}, 'Layer'), ...(sw ? ['Ahead', 'In slab', 'Behind'] : ['Shown']).map(x => h('th', {}, x)), h('th', {}, 'Colour'), h('th', {}, 'Opac.'), h('th', {}, ''));
  tb.append(head);
  const opts = layerOptions(cy.structure);
  (st.layers || []).forEach((L, li) => {
    const tr = h('tr'), td = () => h('td');
    const sel = selIn(opts.some(o => o[0] === L.ref) ? opts : [[L.ref, layerLabel(L.ref, cy.structure)]].concat(opts), () => L.ref, v => { L.ref = v; }, true);
    const c0 = td(); c0.append(sel); tr.append(c0);
    const zones = sw ? ['ahead', 'at', 'behind'] : ['all'];
    for (const z of zones) {
      const c = td(), on = sw ? L.zones.includes(z) || L.zones.includes('all') : true;
      const cb = h('input', { type: 'checkbox' }); cb.checked = on; cb.disabled = !sw;
      cb.addEventListener('change', () => { let zs = L.zones.includes('all') ? ['ahead', 'at', 'behind'] : L.zones.slice(); zs = cb.checked ? [...new Set(zs.concat([z]))] : zs.filter(x => x !== z); L.zones = zs.length === 3 ? ['all'] : zs; changed(false); });
      c.append(cb); tr.append(c);
    }
    const id = A.bind(L.ref, cy.structure, R), base = IscScene.resolveStyle(seq.scene.theme, id, seq.styles[id], L.style);
    const col = h('input', { type: 'color', title: 'Colour in this step (the theme\'s by default)' }); col.value = hex(base.color || [200, 200, 200]);
    col.addEventListener('input', () => { L.style = Object.assign({}, L.style || {}, { color: col.value }); changed(false, false); });
    col.addEventListener('change', () => changed(false));
    const cc = td(); cc.append(col); tr.append(cc);
    const RL = R.layers.get(id);
    if (RL && RL.prim === 'slice' && RL.rep !== 'srFrames') {   // image slices: black see-through (added as light) or opaque; SMLM frames are always opaque
      const bl = h('input', { type: 'checkbox', title: 'Black see-through: the image adds as light, so its black shows what lies behind. Off: the slice is opaque.' });
      bl.checked = base.blend === 'add';
      bl.addEventListener('change', () => { L.style = Object.assign({}, L.style || {}, { blend: bl.checked ? 'add' : 'alpha' }); changed(false); });
      cc.append(bl);
    }
    if (RL && RL.rep === 'wfSlice') {   // the z-stack summed over its planes instead of the plane at the sweep
      const sz = h('input', { type: 'checkbox', title: 'Sum over z: the WideField z-stack summed over all its planes instead of the plane at the sweep.' });
      sz.checked = !!base.sumZ;
      sz.addEventListener('change', () => { L.style = Object.assign({}, L.style || {}, { sumZ: sz.checked }); changed(false); });
      cc.append(sz);
    }
    const op = numIn(() => (L.style && L.style.opacity !== undefined ? L.style.opacity : ''), v => { L.style = Object.assign({}, L.style || {}); if (v === null) delete L.style.opacity; else L.style.opacity = Math.max(0, Math.min(1, v)); }, { min: 0, max: 1, step: 0.05, placeholder: (+base.opacity).toFixed(2) });
    const co = td(); co.append(op); tr.append(co);
    const cx = td(); cx.append(btn('✕', 'Remove', () => { st.layers.splice(li, 1); changed(true); }, 'mini')); tr.append(cx);
    tb.append(tr);
  });
  body.append(tb);
  const add = selIn([['', '+ layer…']].concat(opts), () => '', v => { if (v) st.layers.push({ ref: v, zones: sw ? ['behind'] : ['all'] }); }, true);
  body.append(h('div', { class: 'btns' }));
  body.lastChild.append(add);
  det.append(body);
  return det;
}
const openState = new Map();
const STEP_KINDS = {
  popIn: () => ({ name: 'Pop in', duration: 3, fadeSec: 0.4, layers: [{ ref: '$.gt', zones: ['all'] }] }),
  up: () => ({ name: 'Bottom to top', duration: 6, sweep: { axis: 'z', from: { rel: 0 }, to: { rel: 1 }, slab: 0, ease: 'inOut' }, layers: [{ ref: '$.gt', zones: ['behind'] }] }),
  down: () => ({ name: 'Top to bottom', duration: 6, sweep: { axis: 'z', from: { rel: 1 }, to: { rel: 0 }, slab: 0, ease: 'inOut' }, layers: [{ ref: '$.gt', zones: ['behind'] }] }),
  slab: () => ({ name: 'Moving slab', duration: 6, sweep: { axis: 'z', from: { rel: 1 }, to: { rel: 0 }, slab: 0.5, ease: 'linear' }, layers: [{ ref: '$.gt', zones: ['at'] }] }),
  side: () => ({ name: 'Side to side', duration: 5, sweep: { axis: 'h', from: { rel: 0 }, to: { rel: 1 }, slab: 0, ease: 'inOut' }, layers: [{ ref: '$.gt', zones: ['behind'] }] }),
  hold: () => ({ name: 'Hold', duration: 2, layers: [] }),
};
function cycleCard(cy, ci) {
  const det = h('details', { class: 'an-card' + (selected.cycleId === cy.id ? ' sel' : '') });
  det.open = openState.has(cy.id) ? openState.get(cy.id) : true;
  det.addEventListener('toggle', () => openState.set(cy.id, det.open));
  const sum = h('summary'), title = h('span', { class: 't' });
  title.append(h('b', {}, `${ci + 1} ${cy.name || 'Cycle'}`), document.createTextNode(' · '));
  const ts = h('span'); sums.set(ts, () => cycleSummary(cy)); ts.textContent = cycleSummary(cy);
  title.append(ts);
  const on = checkIn(() => cy.enabled !== false, v => { cy.enabled = v; }, true);
  on.title = 'Include this cycle';
  sum.append(on, title,
    btn('↑', 'Move up', () => { move(seq.cycles, ci, -1); changed(true); }, 'mini'),
    btn('↓', 'Move down', () => { move(seq.cycles, ci, 1); changed(true); }, 'mini'),
    btn('⧉', 'Duplicate (then pick another structure: its layers follow)', () => {
      const c = A.clone(cy); c.id = A.newId('c'); c.steps.forEach(s => { s.id = A.newId('s'); }); seq.cycles.splice(ci + 1, 0, c); changed(true);
    }, 'mini'),
    btn('✕', 'Delete', () => { seq.cycles.splice(ci, 1); changed(true); }, 'mini'));
  det.append(sum);
  const body = h('div', { class: 'body' });
  const structs = [...R.structures.values()].map(s => [s.id, s.label]);
  body.append(
    row('Name', '', textIn(() => cy.name, v => { cy.name = v; })),
    row('Structure', 'What this cycle animates: its steps\' "simulated" layer (and data layers) are this structure\'s. Duplicate a cycle and change this to repeat it for another structure.',
      selIn(structs, () => cy.structure, v => { cy.structure = v; }, true)),
    row('Orbit', 'How far the view turns over this cycle (°), unless a step sets its own rotation.', numIn(() => (cy.orbit && cy.orbit.deg) || 0, v => { cy.orbit = { deg: v || 0 }; }, { step: 15 }), h('span', { class: 'u' }, '° / cycle')));
  // keep after
  const keep = h('div', { class: 'btns' });
  keep.append(h('span', { class: 'note' }, 'Keep shown after:'));
  for (const [ref, label, dis] of layerOptions(cy.structure).filter(o => o[0].startsWith('$.'))) {
    if (dis) continue;
    const l = h('label', { class: 'note' }), cb = h('input', { type: 'checkbox' });
    cb.checked = (cy.keepAfter || []).includes(ref);
    cb.addEventListener('change', () => { cy.keepAfter = (cy.keepAfter || []).filter(r => r !== ref).concat(cb.checked ? [ref] : []); changed(false); });
    l.append(cb, document.createTextNode(' ' + label.split(': ').pop()));
    keep.append(l);
  }
  body.append(keep);
  (cy.steps || []).forEach((st, si) => body.append(stepCard(cy, st, si)));
  const add = selIn([['', '+ step…'], ['popIn', 'pop in'], ['up', 'sweep up'], ['down', 'sweep down'], ['slab', 'moving slab'], ['side', 'side to side'], ['hold', 'hold']],
    () => '', v => { if (v) { const s = STEP_KINDS[v](); s.id = A.newId('s'); cy.steps.push(s); openState.set(s.id, true); } }, true);
  body.append(h('div', { class: 'btns' }));
  body.lastChild.append(add);
  det.append(body);
  return det;
}

// ---- the panel ----
const panel = $('animPanel');
const SIZES = [['1280x720', '720p (1280 × 720)'], ['1920x1080', '1080p (1920 × 1080)'], ['1080x1080', 'square 1080'], ['1080x1920', 'portrait 1080 × 1920'], ['3840x2160', '4K (3840 × 2160)'], ['800x600', '800 × 600']];
function render() {
  if (panel.hidden) return;
  const scroll = panel.scrollTop;
  sums.clear();
  panel.textContent = '';
  // sequence
  const sec = (title, ...right) => { const d = h('div', { class: 'an-sec' }); d.append(h('span', {}, title), ...right); return d; };
  panel.append(sec('Sequence'));
  panel.append(row('Name', '', textIn(() => seq.name, v => { seq.name = v; })));
  const file = h('input', { type: 'file', accept: '.json,application/json', style: 'display:none' });
  file.addEventListener('change', async () => {
    const f = file.files[0]; if (!f) return;
    try { const m = A.migrate(await f.text()); snapshot(); seq = m.seq; if (m.warnings.length) alert(m.warnings.join('\n')); changed(true); }
    catch (e) { alert('could not open ' + f.name + ': ' + e.message); }
  });
  const presetSel = h('select', { title: 'Start over from a ready-made sequence' });
  for (const [v, l] of [['', 'New from…'], ...Object.entries(A.SEQUENCES).map(([k, S]) => [k, S.label]), ['empty', 'Empty']]) presetSel.append(h('option', { value: v }, l));
  presetSel.addEventListener('change', () => {
    if (!presetSel.value) return;
    if (!confirm('Replace the current animation? (Undo brings it back.)')) { presetSel.value = ''; return; }
    snapshot(); seq = A.SEQUENCES[presetSel.value] ? A.SEQUENCES[presetSel.value].make() : A.defaults(); changed(true);
  });
  const btns = h('div', { class: 'btns' });
  btns.append(presetSel,
    btn('Open…', 'Open a saved animation (.json)', () => file.click()),
    btn('Save…', 'Save this animation as a .json file', () => E.download(new Blob([JSON.stringify(seq, null, 1)], { type: 'application/json' }), (seq.name || 'animation').replace(/[^\w.-]+/g, '_') + '.isc-anim.json')),
    btn('Undo', 'Undo (Ctrl+Z)', doUndo), btn('Redo', 'Redo (Ctrl+Y)', doRedo), file);
  panel.append(btns);
  // scene
  panel.append(sec('Scene'));
  const tg = seq.scene.target;
  const tLabel = h('span', { class: 'note' }, tg.mode === 'pick' && tg.cell ? `cell ${tg.cell}` : `nearest the view centre${target ? ' (' + target + ')' : ''}`);
  panel.append(row('Target cell', 'The cell the animation is about: the camera turns about its middle, sweeps run through it, data layers are made for it.',
    selIn([['center', 'nearest the view centre'], ['pick', 'a picked cell']], () => tg.mode, v => { tg.mode = v; }, true)));
  const pickRow = h('div', { class: 'btns' });
  pickRow.append(btn('Pick on map', 'Then click a cell on the view', () => {
    S.armPick(key => { if (key) { snapshot(); tg.mode = 'pick'; tg.cell = key; frozenTarget = null; changed(true); } });
  }), tLabel);
  panel.append(pickRow);
  panel.append(
    row('Scope', 'Which cells are drawn: the target alone, the target with faint neighbours, or all (the target and nearby cells detailed).',
      selIn([['target', 'target only'], ['ghosts', 'target + faint neighbours'], ['all', 'all cells']], () => seq.scene.scope, v => { seq.scene.scope = v; })),
    row('Neighbours', 'Opacity of the faint neighbours (x the cytoplasm\'s).', numIn(() => seq.scene.ghostOpacity, v => { seq.scene.ghostOpacity = Math.max(0, Math.min(1, v || 0)); }, { min: 0, max: 1, step: 0.05 })),
    row('Look', 'Colours: bright on black (fluorescence-like) or the viewer\'s depth colours. Each layer\'s colour can be set per step.',
      selIn([['fluo', 'dark fluorescence'], ['viewer', 'depth colours']], () => seq.scene.theme, v => { seq.scene.theme = v; }, true)));
  // start camera
  panel.append(sec('Start camera'));
  panel.append(
    row('Tilt', '0 = from above, 90 = from the side.', numIn(() => seq.camera.tilt, v => { seq.camera.tilt = Math.max(0, Math.min(90, v || 0)); }, { min: 0, max: 90, step: 5 }), h('span', { class: 'u' }, '°')),
    row('Rotation', 'Starting angle about the vertical (°).', numIn(() => seq.camera.az, v => { seq.camera.az = v || 0; }, { step: 15 }), h('span', { class: 'u' }, '°')),
    row('Zoom', 'The cell fills 1/fit of the frame (1 = just fits).', numIn(() => seq.camera.fit, v => { seq.camera.fit = Math.max(0.05, v || 1); }, { min: 0.1, step: 0.05 }), h('span', { class: 'u' }, 'fit')));
  const cv = h('div', { class: 'btns' });
  cv.append(btn('Use current view', 'Start tilt, rotation and zoom from the free view', () => {
    const v = S.getView(), b = target && S.cellBounds(target);
    seq.camera.tilt = v.tilt; seq.camera.az = v.az;
    if (b) { const fov = Math.min(v.cssW / v.scale, v.cssH / v.scale * outAspect()); seq.camera.fit = +(fov / compile().fovOf(1, v.tilt)).toFixed(3); }
    changed(true);
  }));
  panel.append(cv);
  // cycles
  panel.append(sec('Cycles'));
  seq.cycles.forEach((cy, ci) => panel.append(cycleCard(cy, ci)));
  const addRow = h('div', { class: 'btns' });
  const presetPick = h('select'), structPick = h('select');
  presetPick.append(h('option', { value: '' }, '+ cycle from…'));
  for (const [k, P] of Object.entries(A.PRESETS)) presetPick.append(h('option', { value: k }, P.label));
  for (const s of R.structures.values()) structPick.append(h('option', { value: s.id }, s.label));
  presetPick.addEventListener('change', () => {
    const k = presetPick.value; if (!k) return;
    if (!A.presetFits(k, structPick.value, R)) { alert(`${A.PRESETS[k].label} needs layers ${R.structures.get(structPick.value).label} does not have yet.`); presetPick.value = ''; return; }
    const c = A.cycleFromPreset(k, { structure: structPick.value });
    seq.cycles.push(c); openState.set(c.id, true); changed(true);
  });
  addRow.append(presetPick, h('span', { class: 'note' }, 'for'), structPick);
  panel.append(addRow);
  const probs = A.validate(seq, R);
  for (const p of probs) panel.append(h('div', { class: 'warn' }, '⚠ ' + p.msg));
  // data (the z-stacks of image layers)
  const need = C ? A.requiredData(C, R).map(n => n.layer).filter(id => S.dataKinds[id]) : [];
  if (need.length) {
    panel.append(sec('Data'));
    const d = seq.data;
    panel.append(
      row('Focus step', 'Focus step of the WideField / BrightField z-stacks.', numIn(() => d.stepUm, v => { d.stepUm = Math.max(0.05, v || 0.2); }, { min: 0.05, step: 0.05 }), h('span', { class: 'u' }, 'µm')),
      row('SMLM step', 'Focus step of the SMLM frame stack.', numIn(() => d.srStepUm, v => { d.srStepUm = Math.max(0.1, v || 0.4); }, { min: 0.1, step: 0.1 }), h('span', { class: 'u' }, 'µm')),
      row('SMLM frames', 'Camera frames per focus position, cycling while the slice passes (at the SMLM rate).', numIn(() => d.srFrames, v => { d.srFrames = Math.max(1, Math.round(v || 10)); }, { min: 1, step: 1 }),
        numIn(() => d.srFps, v => { d.srFps = Math.max(1, v || 10); }, { min: 1, step: 1, title: 'frames per second of the animation' }), h('span', { class: 'u' }, 'fps')),
      row('Averaging', 'WideField / BrightField planes are the mean of this many frames.', numIn(() => d.wfAverage, v => { d.wfAverage = Math.max(1, Math.round(v || 4)); }, { min: 1, step: 1 }), h('span', { class: 'u' }, 'frames')),
      row('SMLM planes', 'Fresh: each focus position starts with all dyes unbleached; sequential: bleaching carries over from plane to plane.',
        selIn([['fresh', 'fresh dyes per plane'], ['sequential', 'sequential']], () => (d.sequential ? 'sequential' : 'fresh'), v => { d.sequential = v === 'sequential'; })));
    const crop = h('label', { class: 'note' }); crop.append(checkIn(() => d.crop !== false, v => { d.crop = v; }), document.createTextNode(' crop the data to the target cell'));
    panel.append(crop);
    const status = target ? S.dataStatus(need, target, d) : [];
    const ready = status.length && status.every(x => x.ready);
    const line = h('div', { class: 'note' }, target ? status.map(x => `${({ wf: 'WideField', sr: 'SMLM frames', bf: 'BrightField' })[x.kind]}: ${x.ready ? 'ready' : x.busy ? `${x.busy.done}/${x.busy.total}` : 'to make'}`).join(' · ') : 'no target cell yet');
    const pb = h('div', { class: 'btns' });
    pb.append(btn(ready ? 'Ready' : 'Prepare data', 'The z-stacks are made by themselves when the animation needs them (and before an export); this makes them now', () => prepare()), line);
    panel.append(pb);
  }
  // export
  panel.append(sec('Export'));
  const o = seq.output;
  const fmtOpts = [['mp4', 'MP4 (H.264)'], ['webm', 'WebM (VP9)'], ['gif', 'GIF']].map(([v, l]) => [v, caps && caps[v] && !caps[v].ok ? `${l} – not in this browser` : l]);
  panel.append(
    row('Format', 'MP4 plays everywhere (slides, players); WebM where H.264 cannot be encoded; GIF for quick looping previews (large, 256 colours).', selIn(fmtOpts, () => o.format, v => { o.format = v; }, true)),
    row('Size', '', selIn(SIZES, () => o.width + 'x' + o.height, v => { const [w, hh] = v.split('x').map(Number); o.width = w; o.height = hh; }, true)),
    row('Frame rate', '', selIn([[24, '24 fps'], [25, '25 fps'], [30, '30 fps'], [50, '50 fps'], [60, '60 fps']].map(([v, l]) => [String(v), l]), () => String(o.fps), v => { o.fps = +v; })),
    row('Quality', 'Video bit rate.', selIn([['low', 'low'], ['medium', 'medium'], ['high', 'high']], () => o.quality, v => { o.quality = v; })));
  if (o.format === 'gif') panel.append(
    row('GIF width', 'GIF frame width (px); the height follows the frame.', selIn([['360', '360'], ['480', '480'], ['640', '640'], ['800', '800']], () => String(o.gif.width), v => { o.gif.width = +v; })),
    row('GIF rate', '', selIn([['10', '10 fps'], ['15', '15 fps'], ['20', '20 fps'], ['25', '25 fps']], () => String(o.gif.fps), v => { o.gif.fps = +v; })),
    row('Dither', 'Ordered dither is steady between frames; Floyd–Steinberg is finer but shimmers.', selIn([['bayer4', 'ordered'], ['fs', 'Floyd–Steinberg'], ['none', 'none']], () => o.gif.dither, v => { o.gif.dither = v; })));
  const ovr = h('div', { class: 'btns' });
  for (const [k, l] of [['scaleBar', 'scale bar'], ['captions', 'captions'], ['readout', 'plane position'], ['legend', 'legend']]) {
    const lab = h('label', { class: 'note' }), cb = checkIn(() => o.overlays[k], v => { o.overlays[k] = v; });
    lab.append(cb, document.createTextNode(' ' + l)); ovr.append(lab);
  }
  panel.append(row('Overlays', 'Drawn on the preview and on every exported frame.'), ovr);
  const dur = C ? C.duration * (loopMode() === 'pingpong' ? 2 : 1) : 0;
  const est = o.format === 'gif' ? `~${Math.round(dur * o.gif.fps * o.gif.width * o.gif.width / (o.width / o.height) * 0.12 / 1e6)}–${Math.round(dur * o.gif.fps * o.gif.width * o.gif.width / (o.width / o.height) * 0.4 / 1e6)} MB`
    : `~${Math.max(1, Math.round(dur * ({ low: 0.05, medium: 0.1, high: 0.18 })[o.quality] * o.width * o.height * o.fps / 8 / 1e6))} MB`;
  panel.append(h('div', { class: 'note' }, `${dur.toFixed(1)} s${loopMode() === 'pingpong' ? ' (forward and back)' : ''} · ${est}`));
  const exp = h('div', { class: 'btns' });
  if (exportJob) {
    const pr = h('progress', { max: 1, value: exportJob.done || 0 });
    exportJob.bar = pr; exportJob.label = h('span', { class: 'note' }, exportJob.text || '');
    exp.append(pr, exportJob.label, btn('Cancel', '', () => exportJob && exportJob.abort.abort()));
  } else exp.append(btn('Export', 'Render every frame off screen and download the file', runExport));
  panel.append(exp);
  if (!S.hasGl()) panel.append(h('div', { class: 'warn' }, '⚠ exporting needs WebGL2 (this view runs on the 2D fallback)'));
  panel.append(h('p', { class: 'note' }, 'Space plays, Esc returns to the free view, ←/→ step a frame (Shift: a second), [ and ] jump between steps; Shift-click on the timeline snaps to a step. Set up the start view in the free view (drag, Shift-drag to turn), then "Use current view".'));
  panel.scrollTop = scroll;
}

// ---- undo / keys ----
function doUndo() { if (!undo.length) return; redo.push(JSON.stringify(seq)); const s = undo.pop(); seq = A.migrate(s || JSON.stringify(A.starter())).seq; lastSnap = JSON.stringify(seq); changed(true, false); }
function doRedo() { if (!redo.length) return; undo.push(JSON.stringify(seq)); seq = A.migrate(redo.pop()).seq; lastSnap = JSON.stringify(seq); changed(true, false); }
document.addEventListener('keydown', e => {
  if ($('animPanel').hidden) return;
  const tag = (e.target.tagName || '').toLowerCase();
  if ((e.ctrlKey || e.metaKey) && (e.key === 'z' || e.key === 'y') && tag !== 'input' && tag !== 'textarea') { e.preventDefault(); e.key === 'z' && !e.shiftKey ? doUndo() : doRedo(); return; }
  if (tag === 'input' || tag === 'select' || tag === 'textarea') return;
  if (!C) C = compile();
  const fps = seq.output.fps || 30;
  if (e.key === ' ') { e.preventDefault(); playing ? stop() : play(); }
  else if (e.key === 'Escape') exitPreview();
  else if (e.key === 'ArrowRight' || e.key === 'ArrowLeft') { e.preventDefault(); stop(); seek(t + (e.key === 'ArrowRight' ? 1 : -1) * (e.shiftKey ? 1 : 1 / fps)); }
  else if (e.key === '[' || e.key === ']') { stop(); const bs = [...new Set(C.steps.map(s => s.t0).concat([C.duration]))].sort((a, b) => a - b); seek(e.key === ']' ? (bs.find(b => b > t + 1e-6) ?? t) : ([...bs].reverse().find(b => b < t - 1e-6) ?? 0)); }
  else if (e.key === 'Home') { stop(); seek(0); }
  else if (e.key === 'End') { stop(); seek(C.duration); }
});

// ---- data ----
// The z-stacks the sequence's image layers need, for its target cell (with progress in the panel).
let preparing = null;
async function prepare(signal, onProgress) {
  if (preparing) return preparing;
  const run = async () => {
    const Cx = compile(), need = A.requiredData(Cx, R).map(n => n.layer).filter(id => S.dataKinds[id]);
    const key = frozenTarget || resolveTarget();
    if (!need.length || !key) return;
    await S.acquireData(need, key, seq.data, (d, n, label) => { if (onProgress) onProgress(d, n, label); if (!exportJob) render(); }, signal);
  };
  preparing = run().finally(() => { preparing = null; render(); if (mode === 'preview') show(); });
  return preparing;
}

// ---- export ----
const sleep = ms => new Promise(r => setTimeout(r, ms));
// Renders one frame off screen, waiting (re-rendering) until every cell, microtubule and dye of it is in.
async function readyFrame(st, W, H, cnv, signal, maxMs) {
  const t0 = performance.now();
  for (;;) {
    const r = S.captureFrame(st, { width: W, height: H, canvas: cnv });
    if (!r.info.loading && !r.info.dyes) return r;
    if (performance.now() - t0 > (maxMs || 60000)) { console.warn('animation export: a frame still loading after', maxMs || 60000, 'ms'); return r; }
    if (signal && signal.aborted) throw new DOMException('cancelled', 'AbortError');
    await sleep(80);
  }
}
// bytes of the export (the test hook uses it), or a download
async function exportBlob(opts) {
  opts = opts || {};
  const o = Object.assign({}, seq.output, opts.output || {}), signal = opts.signal;
  stop();
  // the target nearest the centre is only known once the cells around it are packed (a fresh page): wait for that
  for (let i = 0; i < 600 && !(resolveTarget() && (S.packingIdle() || (seq.scene.target || {}).mode === 'pick')); i++) { if (signal && signal.aborted) throw new DOMException('cancelled', 'AbortError'); S.requestDraw(); await sleep(100); }
  frozenTarget = resolveTarget();
  const gif = o.format === 'gif', W = gif ? E.even(o.gif.width) : E.even(o.width), H = gif ? E.even(Math.round(o.gif.width * o.height / o.width)) : E.even(o.height);
  const fps = gif ? o.gif.fps : o.fps;
  const cnv = document.createElement('canvas'), k = Math.max(1, W / 1280);
  // the framing depends on the target's outline: render the first frame until the cell is in, then compile for good
  const compileFor = () => A.compileSequence(seq, { registry: R, bounds: frozenTarget ? S.cellBounds(frozenTarget) : null, aspect: W / H, target: frozenTarget });
  let Cx = compileFor();
  for (let tries = 0; tries < 4 && frozenTarget && !(S.cellBounds(frozenTarget) || {}).precise; tries++) {
    await readyFrame(toState(A.evalCompiled(Cx, 0, { loop: 'once' }), false), W, H, cnv, signal);
    Cx = compileFor();
  }
  // the image layers' z-stacks (after the target's outline is in: their field of view is its box)
  await prepare(signal, (d, n, label) => opts.onProgress && opts.onProgress(0, 1, `${label}: focus ${d}/${n}`));
  const loop = o.loop === 'pingpong' ? 'pingpong' : 'once', D = Cx.duration * (loop === 'pingpong' ? 2 : 1);
  const N = Math.max(1, Math.min(9000, Math.round(D * fps)));
  const bg = IscScene.themeOf(seq.scene.theme).bg;
  const sink = await E.createSink({ format: o.format, width: W, height: H, fps, quality: o.quality, gif: { width: W, height: H, fps, dither: o.gif.dither, keys: [bg, [255, 255, 255]] } });
  const frame = async i => {
    const f = A.evalCompiled(Cx, i / fps, { loop }), st = toState(f, false);
    const r = await readyFrame(st, W, H, cnv, signal);
    drawOverlays(cnv.getContext('2d'), { x: 0, y: 0, w: W, h: H }, f, r.cam, k, o.overlays);
    return cnv;
  };
  try {
    if (sink.wantsPalette) {   // a global GIF palette from a dozen frames spread over the animation
      const samples = [];
      for (let j = 0; j < 12; j++) { const c = await frame(Math.floor(j * (N - 1) / 11)); const copy = document.createElement('canvas'); copy.width = W; copy.height = H; copy.getContext('2d').drawImage(c, 0, 0); samples.push(copy); }
      sink.palette(samples);
    }
    for (let i = 0; i < N; i++) {
      if (signal && signal.aborted) throw new DOMException('cancelled', 'AbortError');
      await sink.addFrame(await frame(i));
      if (opts.onProgress) opts.onProgress(i + 1, N);
      if (i % 4 === 3) await sleep(0);   // let the page breathe (progress, workers' replies)
    }
    const blob = await sink.finish();
    return { blob, ext: sink.ext, fellBack: sink.fellBack, frames: N, width: W, height: H };
  } catch (e) { sink.abort(); throw e; }
  finally { frozenTarget = null; if (mode === 'preview') show(); else S.requestDraw(); }
}
async function runExport() {
  if (exportJob) return;
  const abort = new AbortController(), t0 = performance.now();
  exportJob = { abort, done: 0, text: 'starting…' };
  render();
  try {
    const r = await exportBlob({ signal: abort.signal, onProgress: (i, n, label) => {
      if (label) { exportJob.text = label; if (exportJob.label) exportJob.label.textContent = label; return; }
      const el = (performance.now() - t0) / 1000, left = el / i * (n - i);
      exportJob.done = i / n; exportJob.text = `frame ${i}/${n} · ${(i / el).toFixed(1)} fps · ${Math.ceil(left)} s left`;
      if (exportJob.bar) { exportJob.bar.value = exportJob.done; exportJob.label.textContent = exportJob.text; }
    } });
    E.download(r.blob, `insiliscope_${(seq.name || 'animation').replace(/[^\w.-]+/g, '_')}_${r.width}x${r.height}.${r.ext}`);
    if (r.fellBack) alert('Saved as ' + r.ext.toUpperCase() + ': ' + r.fellBack + '.');
  } catch (e) { if (e.name !== 'AbortError') alert('export failed: ' + (e.message || e)); }
  finally { exportJob = null; render(); }
}

// ---- start ----
seq = load();
lastSnap = JSON.stringify(seq);
E.probe(1920, 1080, 30).then(c => { caps = c; render(); }).catch(() => {});
if (UI.store.get('tab', 'settings') === 'anim') setTimeout(() => setTab('anim'), 0);

window.iscAnim = {
  load(json) { seq = A.migrate(json).seq; lastSnap = JSON.stringify(seq); changed(true, false); },
  get: () => A.clone(seq),
  open: () => setTab('anim'),
  evalAt: tt => { if (!C) C = compile(); return A.evalCompiled(C, tt, { loop: 'once' }); },
  seek, play, stop, exitPreview, target: () => resolveTarget(),
  exportBytes: async opts => { const r = await exportBlob(opts); return new Uint8Array(await r.blob.arrayBuffer()); },
  exportBlob,
  caps: () => caps,
};
})();
