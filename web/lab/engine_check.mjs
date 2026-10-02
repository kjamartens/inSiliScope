// lab.html's JS engine (web/lab/engine.js) against the viewer's own WASM engine (iscEngine, taken from
// web/index.html, on the committed web/insiliscope_module.js): the same jobs, reply by reply. Needs no build.
//   node web/lab/engine_check.mjs [--quick]
// pack / cell / sites must be identical (field by field, every number); movies as tests/parity/scope_parity.mjs
// (SR identical, WideField >= 99.9%, BrightField >= 99.5% of pixels). In iteration mode (PORT_PENDING.md) the JS
// is ahead on purpose and differences are expected.
import fs from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';
import { loadWasmScope } from '../../tests/parity/wasm_scope.mjs';
import { parseInputs, inputDefaults, paramsSource } from '../../tests/parity/load_prototype.mjs';
import { createEngine } from './engine.js';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const read = f => fs.readFileSync(path.join(ROOT, f), 'utf8').replace(/\r\n/g, '\n');
const quick = process.argv.includes('--quick');

const viewer = read('web/index.html');
const C = await loadWasmScope(read('web/insiliscope_module.js'));
const s = viewer.indexOf('\nfunction iscEngine(M) {'), e = viewer.indexOf('\n}\n', s);
if (s < 0 || e < 0) throw new Error('iscEngine not found in web/index.html');
const wasm = new Function(viewer.slice(s, e + 3) + '\nreturn iscEngine;')()(C.M);
const js = await createEngine({ html: read('web/prototype/index.html'), mt: read('web/prototype/microtubules.js') });

// The viewer's params() at its own defaults (what its UI sends), optionally with overrides.
const viewerParams = new Function('vals',
  'const els = new Proxy({}, { get: (_, k) => ({ value: String(vals[k]), checked: !!vals[k] }) });\n' +
  paramsSource(viewer) + '\nreturn params();');
const VIEWER_DEFAULTS = inputDefaults(parseInputs(viewer));
const PACK_KEY_SKIP = /^(mt|showCytoContours$|cytoContourStep$|labelEfficiency$|labelNonBleaching$)/; // = index.html
const packParams = p => Object.fromEntries(Object.entries(p).filter(([k]) => !PACK_KEY_SKIP.test(k)));

let fails = 0;
const report = (ok, msg) => { console.log(`${ok ? 'ok  ' : 'FAIL'} ${msg}`); if (!ok) fails++; };

// First difference between two replies ('' = identical): every key, typed arrays element by element.
function diff(a, b, at = '') {
  if (ArrayBuffer.isView(a) || ArrayBuffer.isView(b)) {
    if (!ArrayBuffer.isView(a) || !ArrayBuffer.isView(b)) return `${at}: typed array vs ${typeof (ArrayBuffer.isView(a) ? b : a)}`;
    if (a.constructor !== b.constructor) return `${at}: ${a.constructor.name} vs ${b.constructor.name}`;
    if (a.length !== b.length) return `${at}: length ${a.length} vs ${b.length}`;
    for (let i = 0; i < a.length; i++) if (!Object.is(a[i], b[i])) return `${at}[${i}]: ${a[i]} vs ${b[i]}`;
    return '';
  }
  if (a && b && typeof a === 'object' && typeof b === 'object') {
    for (const k of new Set([...Object.keys(a), ...Object.keys(b)])) {
      if (k === 'ms') continue;
      const r = diff(a[k], b[k], at + '.' + k);
      if (r) return r;
    }
    return '';
  }
  return Object.is(a, b) ? '' : `${at}: ${JSON.stringify(a)} vs ${JSON.stringify(b)}`;
}
const both = job => [js.handle(structuredClone(job))[0], wasm.handle(structuredClone(job))[0]];

const WORLDS = [
  ['defaults, seed 1249', 1249, {}],
  ['seed 7, wobbly MTs, rough edges, 30% bleaching dyes', 7, { mtWobbleTurn: 1.6, cellRough: 0.4, labelEfficiency: 0.3 }],
];
for (const [name, seed, over] of quick ? WORLDS.slice(0, 1) : WORLDS) {
  const p = viewerParams({ ...VIEWER_DEFAULTS, ...over });
  const S = p.chunkSize, rect = [0, 0, 3 * S, 2 * S];
  const [pj, pw] = both({ type: 'pack', id: 1, key: 'k', win: [0, 0, 3, 2], rect, seed, p: packParams(p) });
  const nCells = pw.cells.length / 14;
  report(!diff(pj, pw) && nCells > 0, `${name}: pack, ${nCells} cells ${diff(pj, pw)}`);
  let bad = '', nMt = 0;
  for (let i = 0; i < Math.min(quick ? 2 : 4, nCells) && !bad; i++) {
    const cx = pw.cells[14 * i], cy = pw.cells[14 * i + 1];
    const [cj, cw] = both({ type: 'cell', key: cx + ',' + cy, sig: 's', seed, p, cx, cy, mt: true });
    bad = diff(cj, cw) && `cell ${cx},${cy}${diff(cj, cw)}`;
    nMt += cw.mts ? cw.mts.lens.length : 0;
  }
  const [mj, mw] = both({ type: 'cell', key: 'none', sig: 's', seed, p, cx: 9999, cy: 9999, mt: true });
  bad = bad || (diff(mj, mw) && `missing cell${diff(mj, mw)}`);
  report(!bad, `${name}: cell assets (outline, mesh, ${nMt} microtubules, a missing cell) ${bad}`);
  const c0 = [pw.cells[2], pw.cells[3]];
  const [sj, sw] = both({ type: 'sites', id: 2, key: 'k', rect: [c0[0] - 1.5, c0[1] - 1.5, c0[0] + 1.5, c0[1] + 1.5], seed, p });
  report(!diff(sj, sw) && sw.sites.length > 0, `${name}: sites, ${sw.sites.length / 4} dyes ${diff(sj, sw)}`);
}

// Movies through handle(), the viewer's spec form (p.* = its params()). WideField takes the WASM CPU path
// here (no WebGPU in Node), which is what the GPU path is checked against anyway.
const p = viewerParams(VIEWER_DEFAULTS), pSpec = Object.entries(p).map(([k, v]) => `p.${k}=${+v}`).join(' ');
const MOVIES = [
  ['SR', 'modality=0 size=40 frames=6', 1],
  ['WideField', 'modality=1 size=40 frames=3', 0.999],
  ['BrightField', 'modality=2 size=40 frames=2 bf-quality=1', 0.995],
];
for (const [name, opts, need] of quick ? MOVIES.slice(0, 1) : MOVIES) {
  const spec = `${opts} world-seed=1249 x=63 y=3 psf-kernel-half-width-nm=2500 ${pSpec}`;
  const [mj, mw] = both({ type: 'movie', id: 3, spec, rect: [0, 0, 1, 1] });
  if (mj.error || mw.error) { report(false, `${name} movie: ${mj.error || ''} ${mw.error || ''}`); continue; }
  let same = 0;
  for (let i = 0; i < mw.frames.length; i++) if (mj.frames[i] === mw.frames[i]) same++;
  const info = diff({ ...mj, frames: 0, gpu: 0 }, { ...mw, frames: 0, gpu: 0 });
  const frac = same / mw.frames.length;
  report(frac >= need && !info, `${name} movie: ${(100 * frac).toFixed(3)}% of ${mw.frames.length} pixels identical, ` +
    `JS ${(mj.ms / 1000).toFixed(1)} s, WASM ${(mw.ms / 1000).toFixed(1)} s ${info}`);
}
console.log(fails ? `lab engine check: ${fails} failure(s)` : 'lab engine check: PASS');
process.exit(fails ? 1 : 0);
