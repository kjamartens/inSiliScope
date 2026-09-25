// Runs the parity cases through the JS prototype (the reference
// implementation) and prints them in parity_main.cpp's line format.
//   node js_reference.mjs <prototype index.html> <cases.txt> <out.txt>
//
// Loads the DOM-free generator half of index.html (everything above the
// `// ---- viewer ---` marker) into a vm context. Default params come from
// the page's own <input> defaults, normalised like its params().
import fs from 'fs';
import vm from 'vm';

const [protoPath, casesPath, outPath] = process.argv.slice(2);
const html = fs.readFileSync(protoPath, 'utf8');

const start = html.indexOf('<script id="mainScript">') + '<script id="mainScript">'.length;
const end = html.indexOf('// ---- viewer ---');
if (start < 30 || end < 0) throw new Error('generator markers not found in ' + protoPath);
const ctx = vm.createContext({});
vm.runInContext(html.slice(start, end) +
  '\n;globalThis.__gen = { pcg4d, hashUnit, hashStream, buildCandidateMap, packMap };', ctx);
const g = ctx.__gen;

// ---- defaults from the page's inputs ----
const PARAM_KEYS = ['chunkSize', 'jitter', 'density', 'cellDiamMin', 'cellDiamMax', 'cellElongMin', 'cellElongMax',
  'cellBlob', 'cellHeightMin', 'cellHeightMax', 'nucLongMin', 'nucLongMax', 'nucRatioMin', 'nucRatioMax',
  'nucHeightMin', 'nucHeightMax', 'nucOffsetFrac', 'nucMargin', 'cytoRimHeightMin', 'cytoRimHeightMax',
  'cytoEdgeRiseMin', 'cytoEdgeRiseMax', 'cytoMidHeightMin', 'cytoMidHeightMax', 'cytoMidDistanceMin',
  'cytoMidDistanceMax', 'cytoMaxSlope', 'cytoDomeSlope', 'cytoSmoothPasses', 'cytoRings', 'cytoTheta',
  'enablePacking', 'allowPackRotation', 'packFrac', 'relaxIters', 'relaxDamping'];
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
    ['cytoMidHeightMin', 'cytoMidHeightMax'], ['cytoMidDistanceMin', 'cytoMidDistanceMax']])
    p[hi] = Math.max(p[lo], p[hi]);
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
        `${hex(Math.atan(x))} ${hex(Math.exp((u1 - 0.5) * 1500))} ${hex(Math.log(u2 * s))}`);
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
