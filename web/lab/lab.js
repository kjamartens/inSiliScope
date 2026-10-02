// insiliscope lab: A/B iteration page. Geometry: the working-tree prototype vs the baseline ref's.
// Imaging: the working tree's JS reference (web/prototype/scope, = the C++ until you change it) vs the
// baseline ref's C++ (its viewer WASM module).
import { formatSummary } from './metrics.js';

const $ = id => document.getElementById(id);
const SKIP_GROUPS = /^3D view/;
const SKIP_IDS = /^(show|cytoContourStep$|mtLineWidth$|seed$)/;
// Imaging options the lab sets itself (seed, FOV, geometry) or that duplicate the geometry panel.
const IMG_HIDDEN = new Set(['world-seed', 'x', 'y', 'chunk-um', 'occupancy', 'cell-diam-min-um', 'cell-diam-max-um', 'mt-density', 'packing']);
// Lab defaults for speed (marked in the panel); everything else is the C++ default.
const IMG_LAB = { frames: 50, size: 96, 'psf-kernel-half-width-nm': 3000 };
const IMG_GROUPS = [
  ['acquisition', /^(modality|size|frames|exposure-ms|start-sec|pixel-nm|z|focus-um|z-range-um|seed)$/],
  ['dyes & photophysics', /^(photons-per-sec|on-sec|off-sec|bleach-prob|photon-cv|milli-activation-rate|labeling-)/],
  ['PSF', /^(psf-|wavelength-nm|na|immersion-index)/],
  ['WideField', /^wf-/],
  ['BrightField', /^bf-/],
  ['camera & background', /./],
];
const ENUMS = {
  modality: ['SuperRes', 'WideField', 'BrightField'], 'psf-model': ['Gaussian', null, null, 'GibsonLanniZernike'], 'psf-mask': ['None', 'DoubleHelix'],
  'psf-interp': ['Nearest', 'Linear', 'Cubic', 'Fft'],
  'psf-zernike-preset': ['None', 'AstigmatismWeak', 'AstigmatismModerate', 'AstigmatismStrong', 'ComaWeak', 'ComaStrong',
    'SphericalWeak', 'SphericalStrong', 'TrefoilModerate', 'MixedRealisticObjective', 'SaddlePoint', 'ExtendedRange', 'ExtendedRangeStrong'],
};

// ---- state (mirrored in the URL hash) ----
const S = { seed: null, cx: 0, cy: 0, win: 0, ab: 'side', tweakBase: true, ov: {}, im: {}, extra: '', imOn: 1, fov: null };
function readHash() {
  const q = new URLSearchParams(location.hash.slice(1));
  for (const [k, v] of q) {
    if (k.startsWith('o.')) S.ov[k.slice(2)] = v === 'true' ? true : v === 'false' ? false : +v;
    else if (k.startsWith('i.')) S.im[k.slice(2)] = +v;
    else if (k === 'fov') S.fov = v.split(',').map(Number);
    else if (k === 'ab') S.ab = v;
    else if (k === 'extra') S.extra = v;
    else if (k === 'tb') S.tweakBase = v !== '0';
    else if (k in S) S[k] = +v;
  }
}
function writeHash() {
  const q = new URLSearchParams();
  q.set('seed', S.seed); q.set('cx', S.cx); q.set('cy', S.cy); q.set('win', S.win);
  if (S.ab !== 'side') q.set('ab', S.ab);
  if (!S.tweakBase) q.set('tb', '0');
  if (!S.imOn) q.set('imOn', 0);
  if (S.fov) q.set('fov', S.fov.map(v => v.toFixed(2)).join(','));
  for (const [k, v] of Object.entries(S.ov)) q.set('o.' + k, v);
  for (const [k, v] of Object.entries(S.im)) q.set('i.' + k, v);
  if (S.extra) q.set('extra', S.extra);
  history.replaceState(null, '', '#' + q);
}

// ---- workers: geometry (work, main) and imaging (imWork = JS reference, imMain = baseline C++) ----
const panes = {
  work: { name: 'working tree', init: { base: '../prototype/' } },
  main: { name: 'baseline', init: { base: '/baseline/web/prototype/' } },
  imWork: { name: 'JS imaging', init: { base: '../prototype/' } },
  imMain: { name: 'baseline C++', init: { wasm: '/baseline/web/insiliscope_module.js' } },
};
let runId = 0;
function startWorker(P) {
  if (P.w) P.w.terminate();
  Object.assign(P, { w: new Worker('lab_worker.js', { type: 'module' }), busy: false, queued: null, ready: false, prog: '' });
  return new Promise((ok, bad) => {
    P.w.onmessage = e => {
      const m = e.data;
      if (m.type === 'ready') { Object.assign(P, { inputs: m.inputs, defaults: m.defaults, scopeOptions: m.scopeOptions, err: null, ready: true }); ok(); }
      else if (m.type === 'progress') { P.prog = `${m.stage} ${Math.round(100 * m.frac)}%`; setStatus(); }
      else if (m.type === 'error') {
        P.err = m.message; P.busy = false; P.prog = '';
        if (!P.ready) bad(new Error(m.message)); else { draw(); drawImaging(); pump(P); }
      } else if (m.type === 'result') onGeometry(P, m);
      else if (m.type === 'movie') { P.busy = false; P.prog = ''; P.movie = m; P.err = null; drawImaging(); pump(P); setStatus(); }
    };
    P.w.postMessage({ type: 'init', ...P.init });
  });
}
function pump(P) {
  if (P.busy || !P.queued) return;
  P.busy = true; P.w.postMessage(P.queued); P.queued = null;
  setStatus();
}
function onGeometry(P, m) {
  P.busy = false;
  if (m.id >= (P.res?.id ?? -1)) { P.res = m; P.err = null; }
  if (findDir && P === panes.work) {
    findDir = 0;
    if (m.cx !== S.cx || m.cy !== S.cy) { S.cx = m.cx; S.cy = m.cy; $('cx').value = S.cx; $('cy').value = S.cy; S.fov = null; }
    schedule();
    return;
  }
  if (!S.fov && P === panes.work) {
    // First result: put the FOV next to the selected cell's nucleus.
    const c = m.cells.find(c => c.cx === S.cx && c.cy === S.cy) || m.cells[0];
    if (c) { S.fov = [c.nuc.x + c.nuc.a, c.nuc.y]; schedule(); }
  }
  draw(); pump(P);
}

let findDir = 0;   // pending "find a cell" request: 0 none, +-1 direction (the working pane resolves it)
const geomPanes = () => S.ab === 'off' || !panes.main.ready ? [panes.work] : [panes.work, panes.main];
const overridesFor = P => (P === panes.work || P === panes.imWork || S.tweakBase ? S.ov : {});

function imagingSpec(P) {
  if (!S.fov) return null;
  const o = { 'world-seed': S.seed, x: +S.fov[0].toFixed(3), y: +S.fov[1].toFixed(3), ...IMG_LAB, ...S.im };
  let s = Object.entries(o).map(([k, v]) => `${k}=${v}`).join(' ');
  for (const [k, v] of Object.entries(overridesFor(P))) s += ` p.${k}=${v === true ? 1 : v === false ? 0 : v}`;
  return (s + ' ' + S.extra).trim();
}

function schedule(geometry = true) {
  writeHash();
  const id = ++runId;
  if (geometry) {
    const win = [S.cx - S.win, S.cy - S.win, S.cx + S.win, S.cy + S.win];
    for (const P of geomPanes()) {
      P.queued = { type: 'run', id, seed: S.seed, overrides: overridesFor(P), win, cx: S.cx, cy: S.cy };
      if (findDir) { P.queued.find = findDir; if (P === panes.main) P.queued = null; }
      pump(P);
    }
  }
  if (S.imOn && !findDir) for (const P of [panes.imWork, ...(S.ab !== 'off' && panes.imMain.ready ? [panes.imMain] : [])]) {
    const spec = imagingSpec(P);
    if (!spec || !P.ready || (P.movie && P.movie.spec === spec && !P.stale)) continue;
    P.stale = false;
    P.queued = { type: 'movie', id, spec };
    pump(P);
  }
  $('spec').value = imagingSpec(panes.imWork) || '';
  setStatus();
}
let debounce = 0;
const scheduleSoon = (geometry = true) => { clearTimeout(debounce); debounce = setTimeout(() => schedule(geometry), 150); };

function setStatus() {
  const busy = Object.values(panes).filter(P => P.busy || P.queued).map(P => P.name + (P.prog ? ` (${P.prog})` : ''));
  $('status').textContent = busy.length ? 'computing: ' + busy.join(', ') + '…' : 'ready';
  const r = panes.work.res, b = panes.main.res;
  $('timing').textContent = r ? `geometry ${r.msTotal.toFixed(0)} ms` + (b && S.ab !== 'off' ? ` · base ${b.msTotal.toFixed(0)} ms` : '') : '';
}

// ---- side panel ----
function numberRow(label, title, el, onReset, changed, note) {
  const row = document.createElement('div');
  row.className = 'row' + (changed ? ' chg' : '');
  const k = document.createElement('span');
  k.className = 'k'; k.textContent = label; k.title = title;
  const bd = document.createElement('span');
  bd.className = 'bd' + (note && note.diff ? ' diff' : ''); bd.textContent = note ? note.text : ''; if (note) bd.title = note.title;
  const x = document.createElement('button');
  x.className = 'x'; x.textContent = '×'; x.title = 'reset'; x.onclick = onReset;
  row.append(k, el, bd, x);
  return row;
}

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
      const def = W.defaults[inp.id], bdef = B.defaults ? B.defaults[inp.id] : undefined;
      const isBool = inp.type === 'checkbox';
      const el = document.createElement('input');
      el.type = isBool ? 'checkbox' : 'number';
      if (!isBool && inp.step) el.step = inp.step;
      const cur = inp.id in S.ov ? S.ov[inp.id] : def;
      if (isBool) el.checked = !!cur; else el.value = cur;
      let note = null;
      if (B.defaults) {
        if (bdef === undefined) note = { text: 'new', diff: true, title: 'not in the baseline prototype' };
        else if (bdef !== def) note = { text: String(bdef), diff: true, title: 'baseline default ' + bdef };
      }
      const label = inp.label.replace(/ min\/max\b/, '') + (inp.id.endsWith('Max') ? ' (max)' : inp.id.endsWith('Min') ? ' (min)' : '');
      const row = numberRow(label, inp.id, el, () => { delete S.ov[inp.id]; if (isBool) el.checked = !!def; else el.value = def; row.classList.remove('chg'); scheduleSoon(); }, inp.id in S.ov, note);
      el.oninput = () => {
        const v = isBool ? el.checked : +el.value;
        if (v === def) delete S.ov[inp.id]; else S.ov[inp.id] = v;
        row.classList.toggle('chg', inp.id in S.ov); scheduleSoon();
      };
      fs.append(row);
    }
    host.append(fs);
  }
}

function buildImagingParams() {
  const host = $('imParams');
  host.innerHTML = '';
  const opts = panes.imWork.scopeOptions;
  if (!opts) { host.innerHTML = '<p class="note">No JS imaging reference in this tree.</p>'; return; }
  const sets = new Map(IMG_GROUPS.map(([g]) => [g, []]));
  for (const o of opts) if (!IMG_HIDDEN.has(o[0])) sets.get(IMG_GROUPS.find(([, re]) => re.test(o[0]))[0]).push(o);
  for (const [g, list] of sets) {
    const fs = document.createElement('fieldset');
    fs.innerHTML = `<legend>imaging: ${g}</legend>`;
    fs.className = 'fold' + (g === 'acquisition' || list.some(([n]) => n in S.im) ? ' open' : '');
    for (const [name, cDef, help] of list) {
      const def = name in IMG_LAB ? IMG_LAB[name] : cDef;
      let el;
      if (ENUMS[name]) {
        el = document.createElement('select');
        ENUMS[name].forEach((n, i) => { if (n) el.add(new Option(n, i)); });
      } else { el = document.createElement('input'); el.type = 'number'; el.step = 'any'; }
      el.value = name in S.im ? S.im[name] : def;
      const note = name in IMG_LAB ? { text: 'lab', title: `lab default for speed; the C++ default is ${cDef}` } : null;
      const row = numberRow(name, help, el, () => { delete S.im[name]; el.value = def; row.classList.remove('chg'); scheduleSoon(false); }, name in S.im, note);
      el.oninput = () => {
        const v = +el.value;
        if (v === def) delete S.im[name]; else S.im[name] = v;
        row.classList.toggle('chg', name in S.im); scheduleSoon(false);
      };
      fs.append(row);
    }
    fs.querySelector('legend').onclick = () => fs.classList.toggle('open');
    host.append(fs);
  }
}

function bindSide() {
  const num = (id, key) => { const el = $(id); el.value = S[key]; el.oninput = () => { S[key] = +el.value; scheduleSoon(); }; };
  num('seed', 'seed'); num('cx', 'cx'); num('cy', 'cy');
  $('win').value = S.win; $('win').onchange = () => { S.win = +$('win').value; schedule(); };
  $('abMode').value = S.ab; $('abMode').onchange = () => { S.ab = $('abMode').value; layout(); schedule(); };
  $('tweakBase').checked = S.tweakBase; $('tweakBase').onchange = () => { S.tweakBase = $('tweakBase').checked; schedule(); };
  $('imOn').checked = !!S.imOn; $('imOn').onchange = () => { S.imOn = $('imOn').checked ? 1 : 0; layout(); schedule(false); };
  $('extra').value = S.extra; $('extra').onchange = () => { S.extra = $('extra').value.trim(); schedule(false); };
  $('rerun').onclick = () => { panes.imWork.stale = panes.imMain.stale = true; schedule(false); };
  $('resetAll').onclick = () => { S.ov = {}; S.im = {}; S.extra = ''; $('extra').value = ''; buildParams(); buildImagingParams(); schedule(); };
  $('copyLink').onclick = () => navigator.clipboard?.writeText(location.href);
  $('copySpec').onclick = () => navigator.clipboard?.writeText('insiliscope_cli ' + $('spec').value.split(' ').map(t => '--' + t).join(' '));
  const step = d => { S.cx += d; S.fov = null; findDir = d; schedule(); };
  $('prevCell').onclick = () => step(-1);
  $('nextCell').onclick = () => step(1);
  addEventListener('keydown', e => {
    if (e.code === 'Space' && S.ab === 'flip' && !/INPUT|SELECT/.test(e.target.tagName)) { e.preventDefault(); flipShow = flipShow === 'work' ? 'main' : 'work'; draw(); drawImaging(); }
  });
}

// ---- drawing: geometry ----
let flipShow = 'work';
function layout() {
  const list = S.ab === 'side' ? ['work', 'main'] : ['work'];
  const host = $('panes');
  host.innerHTML = '';
  host.className = list.length === 1 ? 'one' : '';
  for (const key of list) {
    const d = document.createElement('div');
    d.className = 'pane'; d.id = 'pane-' + key;
    d.innerHTML = `<h3><b class="t"></b><span class="i"></span></h3><canvas class="xy"></canvas><canvas class="xz"></canvas><div class="err"></div>`;
    host.append(d);
    const cv = d.querySelector('canvas.xy');
    cv.onclick = e => {
      const v = views.get(cv);
      if (!v) return;
      const r = cv.getBoundingClientRect();
      S.fov = [v.x0 + (e.clientX - r.left) / r.width * v.w, v.y0 + (e.clientY - r.top) / r.height * v.h];
      draw(); schedule(false);
    };
  }
  const ih = $('imPanes');
  ih.innerHTML = '';
  ih.className = list.length === 1 ? 'one' : '';
  $('imaging').style.display = S.imOn ? '' : 'none';
  for (const key of list) {
    const d = document.createElement('div');
    d.className = 'pane'; d.id = 'im-' + key;
    d.innerHTML = `<h3><b class="t"></b><span class="i"></span></h3><div class="imgs3"><canvas class="fr"></canvas><canvas class="sum"></canvas><canvas class="map"></canvas></div><div class="cap note"></div><div class="err"></div>`;
    ih.append(d);
  }
  draw(); drawImaging();
}
const views = new WeakMap();

function zColor(z, zmax) {
  const t = Math.max(0, Math.min(1, z / Math.max(zmax, 1e-6)));
  const r = Math.round(255 * Math.min(1, Math.max(0, 1.6 * t - 0.4)));
  const g = Math.round(255 * Math.min(1, 0.25 + 0.9 * t));
  const b = Math.round(255 * Math.max(0.2, 1 - 1.1 * t));
  return `rgb(${r},${g},${b})`;
}
function fitCanvas(cv, aspect) {
  const w = cv.clientWidth || 400, dpr = devicePixelRatio || 1;
  cv.width = Math.round(w * dpr); cv.height = Math.round(w * aspect * dpr);
  cv.style.height = (w * aspect) + 'px';
  const ctx = cv.getContext('2d');
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  return { ctx, W: w, H: w * aspect };
}
// One view box for both panes, so A/B line up.
function sharedView() {
  let x0 = Infinity, y0 = Infinity, x1 = -Infinity, y1 = -Infinity, zmax = 0;
  for (const P of geomPanes()) for (const c of P.res?.cells || []) {
    for (const [x, y] of c.outline) { x0 = Math.min(x0, x); y0 = Math.min(y0, y); x1 = Math.max(x1, x); y1 = Math.max(y1, y); }
    zmax = Math.max(zmax, c.height);
  }
  if (!isFinite(x0)) { x0 = 0; y0 = 0; x1 = 30; y1 = 30; }
  return { x0: x0 - 1, y0: y0 - 1, vw: x1 - x0 + 2, vh: y1 - y0 + 2, zmax };
}
const fovSizeUm = () => (S.im.size ?? IMG_LAB.size) * (S.im['pixel-nm'] ?? 100) / 1000;

function drawPane(key, el) {
  const P = panes[key], r = P.res;
  el.querySelector('.t').textContent = key === 'work' ? 'Working tree' : `Baseline (${refInfo.base || 'main'} @ ${refInfo.baseSha || '?'})`;
  el.querySelector('.err').textContent = P.err || '';
  if (!r) return;
  const nMt = r.cells.reduce((a, c) => a + c.mts.length, 0);
  el.querySelector('.i').textContent = `${r.cells.length} cells · ${nMt} MTs · removed ${r.removed} · ${r.msGeom.toFixed(0)} ms`;
  const { x0, y0, vw, vh, zmax } = sharedView();
  const cv = el.querySelector('canvas.xy');
  const { ctx, W, H } = fitCanvas(cv, vh / vw);
  views.set(cv, { x0, y0, w: vw, h: vh });
  const s = W / vw, X = x => (x - x0) * s, Y = y => (y - y0) * s;
  ctx.clearRect(0, 0, W, H);
  for (const c of r.cells) {
    ctx.beginPath();
    c.outline.forEach(([x, y], i) => i ? ctx.lineTo(X(x), Y(y)) : ctx.moveTo(X(x), Y(y)));
    ctx.closePath(); ctx.fillStyle = 'rgba(90,169,230,0.07)'; ctx.fill(); ctx.strokeStyle = '#2d4a66'; ctx.lineWidth = 1; ctx.stroke();
    const n = c.nuc;
    ctx.beginPath(); ctx.ellipse(X(n.x), Y(n.y), n.a * s, n.b * s, n.rot, 0, 2 * Math.PI);
    ctx.strokeStyle = 'rgba(230,162,60,0.6)'; ctx.stroke();
  }
  ctx.lineWidth = Math.max(0.6, 0.05 * s);
  for (const c of r.cells) for (const a of c.mts)
    for (let i = 3; i < a.length; i += 3) {
      ctx.strokeStyle = zColor(a[i + 2], zmax);
      ctx.beginPath(); ctx.moveTo(X(a[i - 3]), Y(a[i - 2])); ctx.lineTo(X(a[i]), Y(a[i + 1])); ctx.stroke();
    }
  if (S.fov) {
    const h = fovSizeUm() / 2;
    ctx.strokeStyle = '#ff4d4d'; ctx.lineWidth = 1.5;
    ctx.strokeRect(X(S.fov[0] - h), Y(S.fov[1] - h), 2 * h * s, 2 * h * s);
  }
  const cz = el.querySelector('canvas.xz'), zs = Math.max(zmax, 1), g2 = fitCanvas(cz, 0.22), sz = (g2.H - 8) / zs;
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
}

function drawMetrics() {
  const a = panes.work.res?.metrics, b = S.ab !== 'off' ? panes.main.res?.metrics : null;
  if (!a) { $('metrics').innerHTML = ''; return; }
  const rows = formatSummary(a, b);
  let h = `<table><tr><th>MT metric (resampled at 0.1 µm)</th><th>working</th>${b ? '<th>baseline</th><th>Δ</th>' : ''}</tr>`;
  for (const r of rows) h += `<tr><td>${r.name}</td><td>${r.a}</td>${b ? `<td>${r.b}</td><td class="${r.d && r.d !== '0.0%' ? 'up' : ''}">${r.d}</td>` : ''}</tr>`;
  h += '</table><canvas id="hist"></canvas>';
  $('metrics').innerHTML = h;
  const g = fitCanvas($('hist'), 0.18);
  const hs = [[a.hist, '#e6c65a'], ...(b ? [[b.hist, '#5aa9e6']] : [])];
  const max = Math.max(1e-9, ...hs.flatMap(([x]) => x)), bw = g.W / 32;
  g.ctx.fillStyle = '#8b97a8';
  g.ctx.fillText('curvature histogram 0-8 rad/µm  (yellow working, blue baseline)', 4, 10);
  for (const [hist, col] of hs) {
    g.ctx.strokeStyle = col; g.ctx.beginPath();
    hist.forEach((v, i) => { const y = g.H - 2 - v / max * (g.H - 16); i ? g.ctx.lineTo(i * bw + bw / 2, y) : g.ctx.moveTo(bw / 2, y); });
    g.ctx.stroke();
  }
}

function draw() {
  if (S.ab === 'flip') { const el = $('pane-work'); if (el) drawPane(flipShow, el); }
  else for (const key of ['work', 'main']) { const el = $('pane-' + key); if (el) drawPane(key, el); }
  drawMetrics();
  setStatus();
}

// ---- drawing: imaging ----
let frame = 0, playing = 0;
function grey(cv, data, w, h, lo, hi, gamma = 1) {
  const id = new ImageData(w, h), span = Math.max(1e-9, hi - lo);
  for (let i = 0; i < w * h; i++) {
    const t = Math.pow(Math.max(0, Math.min(1, (data[i] - lo) / span)), gamma), v = Math.round(255 * t), o = 4 * i;
    id.data[o] = id.data[o + 1] = id.data[o + 2] = v; id.data[o + 3] = 255;
  }
  const g = fitCanvas(cv, h / w), tmp = document.createElement('canvas');
  tmp.width = w; tmp.height = h; tmp.getContext('2d').putImageData(id, 0, 0);
  g.ctx.imageSmoothingEnabled = false;
  g.ctx.drawImage(tmp, 0, 0, g.W, g.H);
}
const pct = (arr, p) => { const s = Float32Array.from(arr).sort(); return s[Math.min(s.length - 1, Math.floor(p * s.length))]; };

function movieStats(m) {
  if (m.stats) return m.stats;
  const W = m.info.width, H = m.info.height, n = W * H, N = m.frames.length / n;
  const mean = new Float32Array(n);
  for (let f = 0; f < N; f++) for (let i = 0; i < n; i++) mean[i] += m.frames[f * n + i];
  for (let i = 0; i < n; i++) mean[i] /= N;
  const sample = m.frames.length > 2e6 ? m.frames.subarray(0, 2e6) : m.frames;
  return (m.stats = { W, H, N, n, mean, lo: pct(sample, 0.001), hi: pct(sample, 0.9995), slo: pct(mean, 0.001), shi: pct(mean, 0.999) });
}

function drawImaging() {
  if (!S.imOn) return;
  const keys = S.ab === 'flip' ? [flipShow] : S.ab === 'side' ? ['work', 'main'] : ['work'];
  const els = S.ab === 'flip' ? [$('im-work')] : keys.map(k => $('im-' + k));
  let N = 0;
  keys.forEach((key, i) => {
    const el = els[i];
    if (!el) return;
    const P = key === 'work' ? panes.imWork : panes.imMain, m = P.movie;
    el.querySelector('.t').textContent = key === 'work' ? 'Imaging: working tree (JS reference)' : `Imaging: baseline C++ (${refInfo.base || 'main'})`;
    el.querySelector('.err').textContent = P.err || (key === 'main' && !P.ready ? 'baseline C++ module not loaded (yet)' : '');
    if (!m) return;
    const st = movieStats(m);
    N = Math.max(N, st.N);
    const f = Math.min(frame, st.N - 1);
    grey(el.querySelector('canvas.fr'), m.frames.subarray(f * st.n, (f + 1) * st.n), st.W, st.H, st.lo, st.hi);
    grey(el.querySelector('canvas.sum'), st.mean, st.W, st.H, st.slo, st.shi);
    const mapCv = el.querySelector('canvas.map');
    if (m.map) { grey(mapCv, m.map.img, m.map.n, m.map.n, 0, pct(m.map.img, 0.999) || 1, 0.5); mapCv.style.display = ''; }
    else mapCv.style.display = 'none';
    const inf = m.info, parts = [`${st.W}×${st.H} × ${st.N}`];
    if (inf.blinks) parts.push(`${inf.blinks} blinks`);
    if (inf.dyes) parts.push(`${inf.dyes} dyes`);
    parts.push(`${(m.ms / 1000).toFixed(1)} s`);
    el.querySelector('.i').textContent = parts.join(' · ');
    el.querySelector('.cap').textContent = `frame ${f + 1}/${st.N} · mean projection` + (m.map ? ` · blink map (ideal localisations, ${Math.round(m.map.binUm * 1000)} nm bins)` : '');
  });
  $('frame').max = Math.max(0, N - 1);
  $('frame').value = Math.min(frame, Math.max(0, N - 1));
  $('frameN').textContent = N ? `${Math.min(frame, N - 1) + 1}/${N}` : '';
  // A/B: identical pixels.
  const a = panes.imWork.movie, b = panes.imMain.movie;
  let cmp = '';
  if (a && b && S.ab !== 'off' && a.frames.length === b.frames.length) {
    let same = 0, da = 0, db = 0;
    for (let i = 0; i < a.frames.length; i++) { if (a.frames[i] === b.frames[i]) same++; da += a.frames[i]; db += b.frames[i]; }
    const p = 100 * same / a.frames.length;
    cmp = `JS reference vs baseline C++: ${p === 100 ? '100' : p.toFixed(3)}% identical pixels · mean ADU ${(da / a.frames.length).toFixed(2)} vs ${(db / b.frames.length).toFixed(2)}` +
      (a.spec !== b.spec ? ' · specs differ (geometry tweaks not applied to the baseline)' : '');
  }
  $('imCompare').textContent = cmp;
}

// ---- boot ----
let refInfo = {};
async function boot() {
  readHash();
  try { refInfo = await (await fetch('/__lab/info')).json(); } catch { refInfo = {}; }
  $('ref').textContent = refInfo.head ? `${refInfo.head} vs ${refInfo.base}@${refInfo.baseSha}` + (refInfo.prototypeDirty?.length ? ` · ${refInfo.prototypeDirty.length} uncommitted prototype file(s)` : '') : '(no serve.mjs: no baseline)';
  await startWorker(panes.work);
  await startWorker(panes.imWork).catch(e => { panes.imWork.err = e.message; });
  if (refInfo.base) {
    await startWorker(panes.main).catch(e => { $('errors').textContent = 'baseline: ' + e.message; });
    startWorker(panes.imMain).then(() => schedule(false)).catch(e => { panes.imMain.err = 'baseline C++: ' + e.message; drawImaging(); });
  }
  if (S.seed == null) S.seed = panes.work.defaults.seed;
  bindSide(); buildParams(); buildImagingParams(); layout();
  if (!location.hash.includes('cx=')) findDir = 1;
  schedule();
  $('frame').oninput = () => { frame = +$('frame').value; drawImaging(); };
  $('play').onclick = () => {
    if (playing) { clearInterval(playing); playing = 0; $('play').textContent = '▶'; return; }
    $('play').textContent = '❚❚';
    playing = setInterval(() => { frame = (frame + 1) % (+$('frame').max + 1); drawImaging(); }, 80);
  };
  addEventListener('resize', () => { draw(); drawImaging(); });
  // Hot reload: prototype edits reload the working workers in place (state kept); lab edits reload the page.
  try {
    const es = new EventSource('/__lab/events');
    es.onmessage = async e => {
      const m = JSON.parse(e.data);
      if (m.dir === 'web/lab') return location.reload();
      if (m.dir === 'web') return; // the viewer (lab.html), not this page
      $('status').textContent = 'prototype changed, reloading…';
      try {
        await startWorker(panes.work);
        await startWorker(panes.imWork);
        panes.imWork.movie = null;
        buildParams(); buildImagingParams(); schedule();
      } catch (err) { panes.work.err = err.message; draw(); }
    };
  } catch {}
}
boot().catch(e => { $('errors').textContent = String(e.stack || e); });
