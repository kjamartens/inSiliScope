// Lab worker. init {base}: one prototype instance (working tree, or the baseline under /baseline/) for
// geometry and the JS imaging reference (web/prototype/scope). init {wasm: url}: the C++ of a ref (the
// viewer's WASM module) for its movie, the imaging baseline.
import { loadPrototype } from '../../tests/parity/load_prototype.mjs';
import { loadWasmScope } from '../../tests/parity/wasm_scope.mjs';
import { makeField } from './field.js';

let F = null, P = null, scope = null, C = null;

async function text(url) {
  const r = await fetch(url + (url.includes('?') ? '&' : '?') + 't=' + Date.now());
  if (!r.ok) throw new Error(`${url}: HTTP ${r.status}`);
  return r.text();
}

async function init(m) {
  if (m.wasm) {
    C = await loadWasmScope(await text(m.wasm));
    return { inputs: [], defaults: {} };
  }
  const [html, mt] = await Promise.all([text(m.base + 'index.html'), text(m.base + 'microtubules.js')]);
  P = loadPrototype(html, mt);
  F = makeField(P);
  // The JS imaging reference of the same tree (absent in a baseline that predates it).
  try { scope = await import(m.base + 'scope/scope_movie.js?t=' + Date.now()); } catch { scope = null; }
  return { inputs: P.inputs, defaults: P.defaults, scopeOptions: scope ? scope.SCOPE_OPTIONS : null };
}

function run(m) {
  const t0 = performance.now();
  if (m.find != null) {
    const [fx, fy] = F.findCell(m.seed, m.overrides, m.cx, m.cy, m.find || 1);
    const dx = fx - m.cx, dy = fy - m.cy;
    m.win = m.win.map((v, i) => v + (i % 2 ? dy : dx));
  }
  const [cx0, cy0, cx1, cy1] = m.win;
  const r = F.cells(m.seed, m.overrides, cx0, cy0, cx1, cy1);
  const cells = r.cells.map(c => ({
    cx: c.cx, cy: c.cy, x: c.x, y: c.y, height: c.height, outline: c.outline, nuc: c.nuc,
    mts: c.mts.map(pts => { const a = new Float32Array(pts.length * 3); pts.forEach((q, i) => { a[3 * i] = q.x; a[3 * i + 1] = q.y; a[3 * i + 2] = q.z; }); return a; }),
  }));
  const out = { type: 'result', id: m.id, cx: (cx0 + cx1) / 2, cy: (cy0 + cy1) / 2, cells, removed: r.removed,
    metrics: F.metrics(r.cells), msGeom: performance.now() - t0 };
  out.msTotal = performance.now() - t0;
  postMessage(out, cells.flatMap(c => c.mts.map(a => a.buffer)));
}

// A movie of `spec` (the scope option string): frames (Uint16Array, frame-major) and, for the JS reference,
// the blink map (ideal localisations: each blink's ON time in the movie at its position, 20 nm bins).
function movie(m) {
  const t0 = performance.now();
  const progress = (stage, frac) => postMessage({ type: 'progress', id: m.id, stage, frac });
  let frames, info, map = null;
  if (C) {
    const r = C.movie(m.spec);
    frames = r.frames; info = r.info;
  } else {
    if (!scope) throw new Error('no JS imaging reference (web/prototype/scope) in this tree');
    const d = scope.scopeDims(scope.parseSpec(m.spec));
    const all = new Uint16Array(d.width * d.height * d.frames);
    info = scope.renderScopeMovie(P, m.spec, (f, adu) => { all.set(adu, f * adu.length); }, {
      onProgress: progress,
      onEvents: (events, S) => {
        const um = S.p.pixelSizeNm / 1000, size = S.W * um, bin = 0.02, n = Math.ceil(size / bin);
        const img = new Float32Array(n * n);
        for (const e of events) {
          const ov = Math.min(S.N, e.tEnd) - Math.max(0, e.tStart);
          if (ov <= 0) continue;
          // Pixel X's centre is at origin + X um, so the FOV spans [-um/2, size - um/2).
          const ix = Math.floor((e.xUm + um / 2) / bin), iy = Math.floor((e.yUm + um / 2) / bin);
          if (ix >= 0 && iy >= 0 && ix < n && iy < n) img[iy * n + ix] += ov * e.brightness;
        }
        map = { img, n, binUm: bin, blinks: events.length };
      },
    });
    frames = all;
  }
  const out = { type: 'movie', id: m.id, spec: m.spec, frames, info, map, ms: performance.now() - t0 };
  postMessage(out, [frames.buffer, ...(map ? [map.img.buffer] : [])]);
}

onmessage = async e => {
  const m = e.data;
  try {
    if (m.type === 'init') postMessage({ type: 'ready', ...(await init(m)) });
    else if (m.type === 'run') run(m);
    else if (m.type === 'movie') movie(m);
  } catch (err) {
    postMessage({ type: 'error', id: m.id, kind: m.type, message: String(err && err.stack || err) });
  }
};
