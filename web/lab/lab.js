// insiliscope lab: A/B iteration page on the JS prototype (working tree vs the baseline git ref).
import { PREVIEW_DEFAULTS } from './preview.js';
import { formatSummary } from './metrics.js';

const $ = id => document.getElementById(id);
const SKIP_GROUPS = /^3D view/;
const SKIP_IDS = /^(show|cytoContourStep$|mtLineWidth$|seed$)/;

// ---- state (mirrored in the URL hash) ----
const S = { seed: null, cx: 0, cy: 0, win: 0, ab: 'side', tweakBase: true, ov: {}, pv: { ...PREVIEW_DEFAULTS }, fov: null };
function readHash() {
  const q = new URLSearchParams(location.hash.slice(1));
  for (const [k, v] of q) {
    if (k.startsWith('o.')) S.ov[k.slice(2)] = v === 'true' ? true : v === 'false' ? false : +v;
    else if (k.startsWith('p.')) S.pv[k.slice(2)] = isNaN(+v) ? v : +v;
    else if (k === 'fov') S.fov = v.split(',').map(Number);
    else if (k === 'ab') S.ab = v;
    else if (k === 'tb') S.tweakBase = v !== '0';
    else if (k in S) S[k] = +v;
  }
}
function writeHash() {
  const q = new URLSearchParams();
  q.set('seed', S.seed); q.set('cx', S.cx); q.set('cy', S.cy); q.set('win', S.win);
  if (S.ab !== 'side') q.set('ab', S.ab);
  if (!S.tweakBase) q.set('tb', '0');
  if (S.fov) q.set('fov', S.fov.map(v => v.toFixed(2)).join(','));
  for (const [k, v] of Object.entries(S.ov)) q.set('o.' + k, v);
  for (const [k, v] of Object.entries(S.pv)) if (v !== PREVIEW_DEFAULTS[k]) q.set('p.' + k, v);
  history.replaceState(null, '', '#' + q);
}

// ---- workers ----
const panes = {
  work: { name: 'working tree', base: '../prototype/', w: null, ready: null, inputs: null, defaults: null, res: null, err: null },
  main: { name: 'baseline', base: '/baseline/web/prototype/', w: null, ready: null, inputs: null, defaults: null, res: null, err: null },
};
let runId = 0;
function startWorker(P) {
  if (P.w) P.w.terminate();
  P.w = new Worker('lab_worker.js', { type: 'module' });
  P.busy = false; P.queued = null;
  P.ready = new Promise((ok, bad) => {
    P.w.onmessage = e => {
      const m = e.data;
      if (m.type === 'ready') { P.inputs = m.inputs; P.defaults = m.defaults; P.err = null; ok(); }
      else if (m.type === 'error') {
        P.err = m.message; P.busy = false;
        if (!P.inputs) bad(new Error(m.message)); else { draw(); pump(P); }
      } else if (m.type === 'result') {
        P.busy = false;
        if (m.id >= (P.res?.id ?? -1)) { P.res = m; P.err = null; }
        if (findDir && P === panes.work) {
          findDir = 0;
          if (m.cx !== S.cx || m.cy !== S.cy) { S.cx = m.cx; S.cy = m.cy; $('cx').value = S.cx; $('cy').value = S.cy; S.fov = null; }
          schedule();
          return;
        }
        if (!S.fov && P === panes.work) {
          // First result: put the FOV on the selected cell's centre (next to its nucleus).
          const c = m.cells.find(c => c.cx === S.cx && c.cy === S.cy) || m.cells[0];
          if (c) { S.fov = [c.nuc.x + c.nuc.a, c.nuc.y]; schedule(); }
        }
        draw(); pump(P);
      }
    };
    P.w.postMessage({ type: 'init', base: P.base });
  });
  return P.ready;
}
function pump(P) {
  if (P.busy || !P.queued) return;
  P.busy = true; P.w.postMessage(P.queued); P.queued = null;
}
function overridesFor(P) {
  if (P === panes.work || S.tweakBase) return S.ov;
  return {};
}
let findDir = 0;   // pending "find a cell" request: 0 none, +-1 direction (the working pane resolves it)
function schedule() {
  writeHash();
  const id = ++runId;
  const win = [S.cx - S.win, S.cy - S.win, S.cx + S.win, S.cy + S.win];
  const pv = S.pv.mode === 'none' ? null : { ...S.pv, cx: S.fov?.[0] ?? 0, cy: S.fov?.[1] ?? 0 };
  for (const P of activePanes()) {
    if (!P.inputs) continue;
    P.queued = { type: 'run', id, seed: S.seed, overrides: overridesFor(P), win, cx: S.cx, cy: S.cy, preview: S.fov ? pv : null };
    if (findDir) { P.queued.find = findDir; if (P === panes.main) P.queued = null; }
    pump(P);
  }
  setStatus();
}
let debounce = 0;
const scheduleSoon = () => { clearTimeout(debounce); debounce = setTimeout(schedule, 120); };
const activePanes = () => S.ab === 'off' || !panes.main.inputs ? [panes.work] : [panes.work, panes.main];

function setStatus() {
  const busy = activePanes().filter(P => P.busy || P.queued).map(P => P.name);
  $('status').textContent = busy.length ? 'computing: ' + busy.join(', ') + '…' : 'ready';
  const r = panes.work.res, b = panes.main.res;
  $('timing').textContent = r ? `work ${r.msTotal.toFixed(0)} ms` + (b && S.ab !== 'off' ? ` · base ${b.msTotal.toFixed(0)} ms` : '') : '';
}

// ---- side panel: parameters, built from the working prototype's inputs ----
function buildParams() {
  const host = $('params');
  host.innerHTML = '';
  const W = panes.work, B = panes.main;
  const groups = new Map();
  for (const inp of W.inputs) {
    if (SKIP_GROUPS.test(inp.group) || SKIP_IDS.test(inp.id)) continue;
    if (!groups.has(inp.group)) groups.set(inp.group, []);
    groups.get(inp.group).push(inp);
  }
  for (const [g, list] of groups) {
    const fs = document.createElement('fieldset');
    fs.innerHTML = `<legend>${g}</legend>`;
    for (const inp of list) {
      const row = document.createElement('div');
      row.className = 'row';
      const def = W.defaults[inp.id];
      const bdef = B.defaults ? B.defaults[inp.id] : undefined;
      const isBool = inp.type === 'checkbox';
      const el = document.createElement('input');
      el.type = isBool ? 'checkbox' : 'number';
      if (!isBool) { if (inp.step) el.step = inp.step; if (inp.min != null) el.min = inp.min; if (inp.max != null) el.max = inp.max; }
      const cur = inp.id in S.ov ? S.ov[inp.id] : def;
      if (isBool) el.checked = !!cur; else el.value = cur;
      const k = document.createElement('span');
      k.className = 'k'; k.textContent = inp.label.replace(/ min\/max$/, '') + (inp.id.endsWith('Max') ? ' (max)' : inp.id.endsWith('Min') ? ' (min)' : '');
      k.title = inp.id;
      const bd = document.createElement('span');
      bd.className = 'bd';
      if (B.defaults) {
        if (bdef === undefined) { bd.textContent = 'new'; bd.classList.add('diff'); bd.title = 'not in the baseline prototype'; }
        else if (bdef !== def) { bd.textContent = String(bdef); bd.classList.add('diff'); bd.title = 'baseline default ' + bdef; }
      }
      const x = document.createElement('button');
      x.className = 'x'; x.textContent = '×'; x.title = 'reset to the working default ' + def;
      const sync = () => row.classList.toggle('chg', inp.id in S.ov);
      el.oninput = () => {
        const v = isBool ? el.checked : +el.value;
        if (v === def) delete S.ov[inp.id]; else S.ov[inp.id] = v;
        sync(); scheduleSoon();
      };
      x.onclick = () => { delete S.ov[inp.id]; if (isBool) el.checked = !!def; else el.value = def; sync(); scheduleSoon(); };
      row.append(k, el, bd, x);
      sync();
      fs.append(row);
    }
    host.append(fs);
  }
}

function bindSide() {
  const num = (id, key, obj = S) => { const el = $(id); el.value = obj[key]; el.oninput = () => { obj[key] = el.type === 'number' ? +el.value : el.value; scheduleSoon(); }; };
  num('seed', 'seed'); num('cx', 'cx'); num('cy', 'cy');
  $('win').value = S.win; $('win').onchange = () => { S.win = +$('win').value; schedule(); };
  $('abMode').value = S.ab; $('abMode').onchange = () => { S.ab = $('abMode').value; layout(); schedule(); };
  $('tweakBase').checked = S.tweakBase; $('tweakBase').onchange = () => { S.tweakBase = $('tweakBase').checked; schedule(); };
  for (const k of Object.keys(PREVIEW_DEFAULTS)) {
    const el = $('pv.' + k);
    if (!el) continue;
    el.value = S.pv[k];
    el.oninput = () => { S.pv[k] = el.tagName === 'SELECT' ? el.value : +el.value; scheduleSoon(); };
  }
  $('resetAll').onclick = () => { S.ov = {}; buildParams(); schedule(); };
  $('copyLink').onclick = () => navigator.clipboard?.writeText(location.href);
  const step = d => { S.cx += d; S.fov = null; findDir = d; schedule(); };
  $('prevCell').onclick = () => step(-1);
  $('nextCell').onclick = () => step(1);
  addEventListener('keydown', e => {
    if (e.code === 'Space' && S.ab === 'flip' && e.target.tagName !== 'INPUT') { e.preventDefault(); flipShow = flipShow === 'work' ? 'main' : 'work'; draw(); }
  });
}

// ---- drawing ----
let flipShow = 'work';
function layout() {
  const host = $('panes');
  host.innerHTML = '';
  const list = S.ab === 'side' ? ['work', 'main'] : ['work'];
  host.className = list.length === 1 ? 'one' : '';
  for (const key of list) {
    const d = document.createElement('div');
    d.className = 'pane'; d.id = 'pane-' + key;
    d.innerHTML = `<h3><b class="t"></b><span class="i"></span></h3><canvas class="xy"></canvas><canvas class="xz"></canvas>
      <div class="imgs"><canvas class="pv"></canvas><canvas class="pvz"></canvas></div><div class="cap note"></div><div class="err"></div>`;
    host.append(d);
    const cv = d.querySelector('canvas.xy');
    cv.onclick = e => {
      const v = viewFor(cv);
      if (!v) return;
      const r = cv.getBoundingClientRect();
      S.fov = [v.x0 + (e.clientX - r.left) / r.width * v.w, v.y0 + (e.clientY - r.top) / r.height * v.h];
      schedule();
    };
  }
  draw();
}
const views = new WeakMap();
const viewFor = cv => views.get(cv);

function zColor(z, zmax) {
  const t = Math.max(0, Math.min(1, z / Math.max(zmax, 1e-6)));
  // dark blue -> cyan -> yellow -> white-ish
  const r = Math.round(255 * Math.min(1, Math.max(0, 1.6 * t - 0.4)));
  const g = Math.round(255 * Math.min(1, 0.25 + 0.9 * t));
  const b = Math.round(255 * Math.max(0.2, 1 - 1.1 * t));
  return `rgb(${r},${g},${b})`;
}

function fitCanvas(cv, aspect) {
  const w = cv.clientWidth || 400;
  const dpr = devicePixelRatio || 1;
  cv.width = Math.round(w * dpr); cv.height = Math.round(w * aspect * dpr);
  cv.style.height = (w * aspect) + 'px';
  const ctx = cv.getContext('2d');
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  return { ctx, W: w, H: w * aspect };
}

// One view box for both panes (bounding box of all shown cells), so A/B line up.
function sharedView() {
  let x0 = Infinity, y0 = Infinity, x1 = -Infinity, y1 = -Infinity, zmax = 0;
  for (const P of activePanes()) for (const c of P.res?.cells || []) {
    for (const [x, y] of c.outline) { x0 = Math.min(x0, x); y0 = Math.min(y0, y); x1 = Math.max(x1, x); y1 = Math.max(y1, y); }
    zmax = Math.max(zmax, c.height);
  }
  if (!isFinite(x0)) { x0 = 0; y0 = 0; x1 = 30; y1 = 30; }
  return { x0: x0 - 1, y0: y0 - 1, vw: x1 - x0 + 2, vh: y1 - y0 + 2, zmax };
}

function drawPane(key, el) {
  const P = panes[key];
  const r = P.res;
  el.querySelector('.t').textContent = key === 'work' ? 'Working tree' : `Baseline (${refInfo.base || 'main'} @ ${refInfo.baseSha || '?'})`;
  el.querySelector('.err').textContent = P.err || '';
  if (!r) return;
  const nMt = r.cells.reduce((a, c) => a + c.mts.length, 0);
  el.querySelector('.i').textContent = `${r.cells.length} cells · ${nMt} MTs · removed ${r.removed} · geom ${r.msGeom.toFixed(0)} ms`;
  const { x0, y0, vw, vh, zmax } = sharedView();
  const cv = el.querySelector('canvas.xy');
  const { ctx, W, H } = fitCanvas(cv, vh / vw);
  views.set(cv, { x0, y0, w: vw, h: vh });
  const s = W / vw;
  const X = x => (x - x0) * s, Y = y => (y - y0) * s;
  ctx.clearRect(0, 0, W, H);
  for (const c of r.cells) {
    ctx.beginPath();
    c.outline.forEach(([x, y], i) => i ? ctx.lineTo(X(x), Y(y)) : ctx.moveTo(X(x), Y(y)));
    ctx.closePath(); ctx.fillStyle = 'rgba(90,169,230,0.07)'; ctx.fill(); ctx.strokeStyle = '#2d4a66'; ctx.stroke();
    const n = c.nuc;
    ctx.beginPath(); ctx.ellipse(X(n.x), Y(n.y), n.a * s, n.b * s, n.rot, 0, 2 * Math.PI);
    ctx.strokeStyle = 'rgba(230,162,60,0.6)'; ctx.stroke();
  }
  ctx.lineWidth = Math.max(0.6, 0.05 * s);
  for (const c of r.cells) for (const a of c.mts) {
    for (let i = 3; i < a.length; i += 3) {
      ctx.strokeStyle = zColor(a[i + 2], zmax);
      ctx.beginPath(); ctx.moveTo(X(a[i - 3]), Y(a[i - 2])); ctx.lineTo(X(a[i]), Y(a[i + 1])); ctx.stroke();
    }
  }
  if (S.fov) {
    const h = S.pv.fovUm / 2;
    ctx.strokeStyle = '#ff4d4d'; ctx.lineWidth = 1.5;
    ctx.strokeRect(X(S.fov[0] - h), Y(S.fov[1] - h), 2 * h * s, 2 * h * s);
  }
  // xz side view: MTs projected along y, z exaggerated to fill.
  const cz = el.querySelector('canvas.xz');
  const zs = Math.max(zmax, 1);
  const g2 = fitCanvas(cz, 0.22);
  const sz = (g2.H - 8) / zs;
  g2.ctx.clearRect(0, 0, g2.W, g2.H);
  g2.ctx.fillStyle = '#8b97a8'; g2.ctx.fillText(`x-z (z × ${(sz / s).toFixed(1)})`, 4, 10);
  g2.ctx.globalAlpha = 0.5; g2.ctx.lineWidth = 0.7;
  for (const c of r.cells) for (const a of c.mts) {
    g2.ctx.strokeStyle = zColor(a[2], zmax);
    g2.ctx.beginPath();
    for (let i = 0; i < a.length; i += 3) { const px = X(a[i]), py = g2.H - 4 - a[i + 2] * sz; i ? g2.ctx.lineTo(px, py) : g2.ctx.moveTo(px, py); }
    g2.ctx.stroke();
  }
  g2.ctx.globalAlpha = 1;
  // preview image
  const pv = el.querySelector('canvas.pv'), pvz = el.querySelector('canvas.pvz');
  if (r.preview) {
    drawImage(pv, r.preview, false);
    drawImage(pvz, r.preview, true);
    el.querySelector('.cap').textContent = `${modeName()} · ${r.preview.info} · ${r.preview.ms.toFixed(0)} ms` +
      (r.preview.capped ? ' · dye cap reached' : '') + ' · right: centre ×2';
  } else {
    for (const c of [pv, pvz]) { const g = fitCanvas(c, 1); g.ctx.fillStyle = '#8b97a8'; g.ctx.fillText('click the top view to place the FOV', 8, 16); }
  }
}

// Grey image, 0.1-99.9 percentile; `zoom` shows the centre quarter with nearest-neighbour upscaling.
function drawImage(cv, pv, zoom) {
  const { img, W, H } = pv;
  const sorted = Float32Array.from(img).sort();
  const lo = sorted[Math.floor(0.001 * sorted.length)], hi = sorted[Math.floor(0.999 * (sorted.length - 1))];
  const gamma = S.pv.mode === 'recon' ? 0.5 : 1;
  const sx = zoom ? W >> 2 : 0, sw = zoom ? W >> 1 : W;
  const id = new ImageData(sw, sw);
  for (let y = 0; y < sw; y++) for (let x = 0; x < sw; x++) {
    const v = img[(y + sx) * W + x + sx];
    const t = Math.pow(Math.max(0, Math.min(1, (v - lo) / Math.max(1e-9, hi - lo))), gamma);
    const o = 4 * (y * sw + x);
    id.data[o] = id.data[o + 1] = id.data[o + 2] = Math.round(255 * t); id.data[o + 3] = 255;
  }
  const g = fitCanvas(cv, 1);
  const tmp = document.createElement('canvas');
  tmp.width = sw; tmp.height = sw; tmp.getContext('2d').putImageData(id, 0, 0);
  g.ctx.imageSmoothingEnabled = false;
  g.ctx.drawImage(tmp, 0, 0, g.W, g.H);
}
const modeName = () => ({ wf: 'WideField preview', sr: 'SR frame preview', recon: 'SR reconstruction preview' })[S.pv.mode] || '';

function drawMetrics() {
  const a = panes.work.res?.metrics, b = S.ab !== 'off' ? panes.main.res?.metrics : null;
  if (!a) { $('metrics').innerHTML = ''; return; }
  const rows = formatSummary(a, b);
  let h = `<table><tr><th>metric (resampled at 0.1 µm)</th><th>working</th>${b ? '<th>baseline</th><th>Δ</th>' : ''}</tr>`;
  for (const r of rows) h += `<tr><td>${r.name}</td><td>${r.a}</td>${b ? `<td>${r.b}</td><td class="${r.d && r.d !== '0.0%' ? 'up' : ''}">${r.d}</td>` : ''}</tr>`;
  h += '</table><canvas id="hist"></canvas>';
  $('metrics').innerHTML = h;
  const g = fitCanvas($('hist'), 0.18);
  const hs = [[a.hist, '#e6c65a'], ...(b ? [[b.hist, '#5aa9e6']] : [])];
  const max = Math.max(1e-9, ...hs.flatMap(([x]) => x));
  const bw = g.W / 32;
  g.ctx.fillStyle = '#8b97a8';
  g.ctx.fillText('curvature histogram 0-8 rad/µm  (yellow working, blue baseline)', 4, 10);
  for (const [hist, col] of hs) {
    g.ctx.strokeStyle = col; g.ctx.beginPath();
    hist.forEach((v, i) => { const y = g.H - 2 - v / max * (g.H - 16); i ? g.ctx.lineTo(i * bw + bw / 2, y) : g.ctx.moveTo(bw / 2, y); });
    g.ctx.stroke();
  }
}

function draw() {
  if (S.ab === 'flip') {
    const el = $('pane-work');
    if (el) drawPane(flipShow, el);
  } else {
    for (const key of ['work', 'main']) { const el = $('pane-' + key); if (el) drawPane(key, el); }
  }
  drawMetrics();
  setStatus();
}

// ---- boot ----
let refInfo = {};
async function boot() {
  readHash();
  try { refInfo = await (await fetch('/__lab/info')).json(); } catch { refInfo = {}; }
  $('ref').textContent = refInfo.head ? `${refInfo.head} vs ${refInfo.base}@${refInfo.baseSha}` + (refInfo.prototypeDirty?.length ? ` · ${refInfo.prototypeDirty.length} uncommitted prototype file(s)` : '') : '(no serve.mjs: no baseline)';
  await startWorker(panes.work);
  if (refInfo.base) await startWorker(panes.main).catch(e => { panes.main.inputs = null; $('errors').textContent = 'baseline: ' + e.message; });
  if (S.seed == null) S.seed = panes.work.defaults.seed;
  bindSide(); buildParams(); layout();
  if (!location.hash.includes('cx=')) findDir = 1;
  schedule();
  addEventListener('resize', () => draw());
  // Hot reload: prototype edits re-load the working worker in place (state kept); lab edits reload the page.
  try {
    const es = new EventSource('/__lab/events');
    es.onmessage = async e => {
      const m = JSON.parse(e.data);
      if (m.dir === 'web/lab') return location.reload();
      $('status').textContent = 'prototype changed, reloading…';
      try { await startWorker(panes.work); buildParams(); schedule(); }
      catch (err) { panes.work.err = err.message; draw(); }
    };
  } catch {}
}
boot().catch(e => { $('errors').textContent = String(e.stack || e); });
