// One prototype instance per worker (working tree, or the baseline served by serve.mjs under /baseline/).
import { loadPrototype } from '../../tests/parity/load_prototype.mjs';
import { makeField } from './field.js';
import { render } from './preview.js';

let F = null;

async function init(base) {
  const bust = '?t=' + Date.now();
  const [html, mt] = await Promise.all(['index.html', 'microtubules.js'].map(async f => {
    const r = await fetch(base + f + bust);
    if (!r.ok) throw new Error(`${base}${f}: HTTP ${r.status}`);
    return r.text();
  }));
  const P = loadPrototype(html, mt);
  F = makeField(P);
  return { inputs: P.inputs, defaults: P.defaults };
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
  const msGeom = performance.now() - t0;
  const cells = r.cells.map(c => ({
    cx: c.cx, cy: c.cy, x: c.x, y: c.y, height: c.height, outline: c.outline, nuc: c.nuc,
    mts: c.mts.map(pts => { const a = new Float32Array(pts.length * 3); pts.forEach((q, i) => { a[3 * i] = q.x; a[3 * i + 1] = q.y; a[3 * i + 2] = q.z; }); return a; }),
  }));
  const out = { type: 'result', id: m.id, cx: (cx0 + cx1) / 2, cy: (cy0 + cy1) / 2, cells, removed: r.removed, metrics: F.metrics(r.cells), msGeom, msPack: r.msPack };
  const transfer = cells.flatMap(c => c.mts.map(a => a.buffer));
  if (m.preview) {
    const t1 = performance.now();
    const o = m.preview, h = o.fovUm / 2;
    const rect = [o.cx - h, o.cy - h, o.cx + h, o.cy + h];
    const d = F.dyes(m.seed, r.cells, rect, o.labelPct / 100);
    const im = render(d, rect, o);
    out.preview = { ...im, rect, nDyes: d.x.length, capped: d.capped, ms: performance.now() - t1 };
    transfer.push(im.img.buffer);
  }
  out.msTotal = performance.now() - t0;
  postMessage(out, transfer);
}

onmessage = async e => {
  const m = e.data;
  try {
    if (m.type === 'init') postMessage({ type: 'ready', ...(await init(m.base)) });
    else if (m.type === 'run') run(m);
  } catch (err) {
    postMessage({ type: 'error', id: m.id, message: String(err && err.stack || err) });
  }
};
