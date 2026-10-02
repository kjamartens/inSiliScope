// Runs the parity cases through the JS prototype (the reference
// implementation) and prints them in parity_main.cpp's line format.
//   node js_reference.mjs <prototype index.html> <cases.txt> <out.txt>
//
// Loads the DOM-free generator half of index.html (everything above the
// `// ---- viewer ---` marker) plus the body of microtubules.js (a sibling of
// index.html) into one vm context. Default params come from the page's own
// <input> defaults, normalised like its params().
import fs from 'fs';
import path from 'path';
import vm from 'vm';

const [protoPath, casesPath, outPath] = process.argv.slice(2);
// V8 < 13 computes Math.pow with its own fdlibm port; 13+ calls std::pow
// (--use-std-math-pow), which is what core's jsm::pow is. The cytoplasm mesh
// uses pow, so an older Node gives a reference core cannot match exactly.
if (+process.versions.v8.split('.')[0] < 13)
  console.warn(`warning: V8 ${process.versions.v8} uses fdlibm Math.pow; use Node >= 24 for the reference`);
const html = fs.readFileSync(protoPath, 'utf8');

const start = html.indexOf('<script id="mainScript">') + '<script id="mainScript">'.length;
const end = html.indexOf('// ---- viewer ---');
if (start < 30 || end < 0) throw new Error('generator markers not found in ' + protoPath);
const ctx = vm.createContext({});
vm.runInContext(html.slice(start, end), ctx);
// microtubules.js: `window.__MT_SRC = function () { <body> };`, run the body as a script (as index.html does).
const mtSrc = fs.readFileSync(path.join(path.dirname(protoPath), 'microtubules.js'), 'utf8');
const mtOpen = mtSrc.indexOf('window.__MT_SRC = function () {');
if (mtOpen < 0) throw new Error('microtubules.js wrapper not found');
vm.runInContext(mtSrc.slice(mtSrc.indexOf('{', mtOpen) + 1, mtSrc.lastIndexOf('}')), ctx);
vm.runInContext('globalThis.__gen = { pcg4d, hashUnit, hashStream, buildCandidateMap, packMap, rawCandidate,' +
  ' getCytoGeometry, sampleCytoMeshHeight, getMtCellGeometry, buildMicrotubulesForCell, buildMicrotubuleLabelPoints, cytoCache };', ctx);
const g = ctx.__gen;

// ---- defaults from the page's inputs ----
const PARAM_KEYS = ['chunkSize', 'jitter', 'density', 'cellDiamMin', 'cellDiamMax', 'cellElongMin', 'cellElongMax',
  'cellBlob', 'cellRough', 'cellFractalDim', 'cellHeightMin', 'cellHeightMax', 'nucLongMin', 'nucLongMax', 'nucRatioMin', 'nucRatioMax',
  'nucHeightMin', 'nucHeightMax', 'nucOffsetFrac', 'nucMargin', 'cytoRimHeightMin', 'cytoRimHeightMax',
  'cytoEdgeRiseMin', 'cytoEdgeRiseMax', 'cytoMidHeightMin', 'cytoMidHeightMax', 'cytoMidDistanceMin',
  'cytoMidDistanceMax', 'cytoMaxSlope', 'cytoDomeSlope', 'cytoRelaxUm', 'cytoRings', 'cytoTheta',
  'enablePacking', 'allowPackRotation', 'packFrac', 'relaxIters', 'relaxDamping',
  'mtDensity', 'mtStartFracMin', 'mtStartFracMax', 'mtStartOffsetXY', 'mtEndFracMin', 'mtEndFracMax',
  'mtEndJitterDeg', 'mtWobbleTurn', 'mtWobbleFactor', 'mtStepLen', 'mtSmoothLen', 'mtMinTurnRadius',
  'mtMinSeparation', 'mtMaxZSlope'];
const defaults = {};
for (const m of html.matchAll(/<input\b[^>]*>/g)) {
  const tag = m[0];
  const id = /\bid="(\w+)"/.exec(tag)?.[1];
  if (!PARAM_KEYS.includes(id)) continue;
  if (/type="checkbox"/.test(tag)) defaults[id] = /\bchecked\b/.test(tag);
  else defaults[id] = +/\bvalue="([^"]*)"/.exec(tag)[1];
}
const missing = PARAM_KEYS.filter(k => !(k in defaults));
if (missing.length) throw new Error('defaults not found in page: ' + missing.join(','));

function normalise(p) {
  for (const [lo, hi] of [['cellDiamMin', 'cellDiamMax'], ['cellElongMin', 'cellElongMax'],
    ['cellHeightMin', 'cellHeightMax'], ['nucLongMin', 'nucLongMax'], ['nucRatioMin', 'nucRatioMax'],
    ['nucHeightMin', 'nucHeightMax'], ['cytoRimHeightMin', 'cytoRimHeightMax'], ['cytoEdgeRiseMin', 'cytoEdgeRiseMax'],
    ['cytoMidHeightMin', 'cytoMidHeightMax'], ['cytoMidDistanceMin', 'cytoMidDistanceMax'],
    ['mtStartFracMin', 'mtStartFracMax'], ['mtEndFracMin', 'mtEndFracMax']])
    p[hi] = Math.max(p[lo], p[hi]);
  // floors from params()
  p.mtWobbleFactor = Math.max(1, p.mtWobbleFactor);
  p.mtStepLen = Math.max(0.02, p.mtStepLen);
  p.mtSmoothLen = Math.max(0, p.mtSmoothLen);
  p.mtMinTurnRadius = Math.max(0, p.mtMinTurnRadius);
  p.mtMaxZSlope = Math.max(1, p.mtMaxZSlope);
  return p;
}

// ---- run ----
const f64 = new Float64Array(1), u64 = new BigUint64Array(f64.buffer);
const hex = x => { f64[0] = x; return u64[0].toString(16).padStart(16, '0'); };
const BOOL_KEYS = new Set(['enablePacking', 'allowPackRotation']);

const out = [];
const paramSets = {};
for (const line of fs.readFileSync(casesPath, 'utf8').split('\n')) {
  const t = line.trim().split(/\s+/);
  const kind = t[0];
  if (kind === 'params') {
    const p = { ...defaults };
    for (const kv of t.slice(2)) {
      const [k, v] = kv.split('=');
      p[k] = BOOL_KEYS.has(k) ? +v !== 0 : +v;
    }
    paramSets[t[1]] = normalise(p);
  } else if (kind === 'rng') {
    const [s, cx, cy, k] = t.slice(1).map(Number);
    const r = g.pcg4d(s | 0, cx | 0, cy | 0, k | 0);
    out.push(`rng ${t[1]} ${t[2]} ${t[3]} ${t[4]} ${r.join(' ')} ${g.hashUnit(s, cx, cy, k)}`);
  } else if (kind === 'stream') {
    const [s, cx, cy, base, count] = t.slice(1).map(Number);
    const next = g.hashStream(s, cx, cy, base);
    const vals = [];
    for (let i = 0; i < count; i++) vals.push(next());
    out.push(`stream ${t[1]} ${t[2]} ${t[3]} ${t[4]} ${vals.join(' ')}`);
  } else if (kind === 'math') {
    const n = +t[1];
    const SCALE = [8 * Math.PI, 200, 1e5, 1e9];
    for (let i = 0; i < n; i++) {
      const u1 = g.hashUnit(7, i, 0, 0), u2 = g.hashUnit(7, i, 1, 0), u3 = g.hashUnit(7, i, 2, 0);
      const s = SCALE[i % 4];
      const x = (u1 - 0.5) * 2 * s;
      const ay = (u2 - 0.5) * s, ax = (u3 - 0.5) * s;
      out.push(`m ${i} ${hex(Math.sin(x))} ${hex(Math.cos(x))} ${hex(Math.atan2(ay, ax))} ${hex(Math.hypot(ay, ax))} ` +
        `${hex(Math.atan(x))} ${hex(Math.exp((u1 - 0.5) * 1500))} ${hex(Math.log(u2 * s))} ` +
        `${hex(Math.asin(u1 * 2 - 1))} ${hex(Math.cbrt(x))} ${hex(Math.hypot(ay, ax, x))}`);
    }
  } else if (kind === 'cells') {
    const [id, seedS, pname, ...rest] = t.slice(1);
    const [cx0, cy0, cx1, cy1, maxCells] = rest.map(Number);
    const p = paramSets[pname], seed = +seedS;
    const f = x => String(x);
    let done = 0;
    for (let cx = cx0; cx <= cx1 && done < maxCells; cx++) {
      for (let cy = cy0; cy <= cy1 && done < maxCells; cy++) {
        const c = g.rawCandidate(seed, cx, cy, p);
        if (!c.present) continue;
        done++;
        g.cytoCache.clear();   // keyed by chunk only: stale across seeds/params
        const t0 = performance.now();
        const mts = g.buildMicrotubulesForCell(seed, cx, cy, c, p);
        const ms = performance.now() - t0;
        out.push(`cell ${id} ${cx} ${cy} nmt ${mts.length} ms ${ms.toFixed(3)}`);
        const { mesh } = g.getCytoGeometry(c, p);
        const hs = [];
        for (const row of mesh.grid) for (const v of row) hs.push(v.h);
        const line = [`mesh ${id} ${cx} ${cy} ${mesh.rings} ${mesh.n}`, g.getMtCellGeometry(c).areaUm2];
        for (let v = 0; v < hs.length; v += 97) line.push(hs[v]);
        for (let k = 0; k < 32; k++) {
          const r = g.hashUnit(99, cx, cy, 2 * k) * c.rOuter * 1.1;
          const a = g.hashUnit(99, cx, cy, 2 * k + 1) * Math.PI * 2;
          line.push(g.sampleCytoMeshHeight(c, p, r * Math.cos(a), r * Math.sin(a)));
        }
        out.push(line.map(f).join(' '));
        mts.forEach((pts, i) => {
          let sx = 0, sy = 0, sz = 0;
          for (const q of pts) { sx += q.x; sy += q.y; sz += q.z; }
          const l = [`mtp ${id} ${cx} ${cy} ${i} ${pts.length}`, sx, sy, sz];
          for (let j = 0; j < pts.length; j += 37) l.push(pts[j].x, pts[j].y, pts[j].z);
          if (pts.length) { const q = pts[pts.length - 1]; l.push(q.x, q.y, q.z); }
          out.push(l.map(f).join(' '));
          // Frames + lattice geometry via the label builder: a constant rng
          // (0.5) makes every site labelled and fixes the linker draws.
          if (i % 16 === 0 && pts.length >= 2) {
            const L = g.buildMicrotubuleLabelPoints(pts, { startUm: 0.5, lenUm: 0.1, phase: 0.3 }, () => 0.5);
            const m = [`mtl ${id} ${cx} ${cy} ${i} ${L.length}`];
            for (const q of L.slice(0, 13)) m.push(q.ax, q.ay, q.az, q.bx, q.by, q.bz, q.x, q.y, q.z);
            out.push(m.map(f).join(' '));
          }
        });
      }
    }
  } else if (kind === 'case') {
    const [id, seedS, pname, ...win] = t.slice(1);
    const [cx0, cy0, cx1, cy1] = win.map(Number);
    const p = paramSets[pname];
    const t0 = performance.now();
    const map = g.buildCandidateMap(+seedS, cx0, cy0, cx1, cy1, p);
    const raw = [];
    for (const c of map.values())
      raw.push(`raw ${c.cx} ${c.cy} ${c.x} ${c.y} ${c.semiMajor} ${c.semiMinor} ${c.rot} ${c.height} ${c.modFloor} ${c.nucZ} ${c.rOuter} ${c.priority}`);
    const ncand = map.size;
    const removed = g.packMap(map, p);
    const ms = performance.now() - t0;
    out.push(`case ${id} ncand ${ncand} removed ${removed} ms ${ms.toFixed(3)}`);
    out.push(...raw);
    for (const c of map.values()) out.push(`pk ${c.cx} ${c.cy} ${c.x} ${c.y} ${c.packRot}`);
  }
}
fs.writeFileSync(outPath, out.join('\n') + '\n');
console.log(`JS reference: ${out.length} lines -> ${outPath} (node ${process.version}, V8 ${process.versions.v8})`);
