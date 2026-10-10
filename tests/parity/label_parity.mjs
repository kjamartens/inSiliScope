// Issue 16 labels: the C++ core (WASM build, C ABI 11) against the JS reference (web/prototype/scope/world.js),
// number for number and in order: fluorescent dyes (sitesInWindow), blinks (eventsInWindow) and continuous windows
// (continuousInWindow) for every label mode, a fluorescent fraction, a pre state and an initial ON, over a query
// history (late window first, then a long one, then early ones); then ABI 11 kinetics histories (rate steps per mode,
// two histories alternating on one world).
//   node tests/parity/label_parity.mjs [path to build/wasm/core/insiliscope.js]
import fs from 'fs';
import path from 'path';
import { createRequire } from 'module';
import { fileURLToPath, pathToFileURL } from 'url';
import { loadPrototype } from './load_prototype.mjs';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const require = createRequire(import.meta.url);
const M = await require(process.argv[2] || path.join(ROOT, 'build/wasm/core/insiliscope.js'))();
const read = f => fs.readFileSync(path.join(ROOT, f), 'utf8');
const P = loadPrototype(read('web/prototype/index.html'), read('web/prototype/microtubules.js'));
const { World } = await import(pathToFileURL(path.join(ROOT, 'web/prototype/scope/world.js')).href);
const D = await import(pathToFileURL(path.join(ROOT, 'web/prototype/scope/dyes.js')).href);
if (M._isc_abi_version() !== 11) throw new Error('core ABI ' + M._isc_abi_version() + ', expected 11');

const seed = 1249, geom = P.paramsFrom(P.defaults);
const MODES = { dSTORM: 0, PALM: 1, 'DNA-PAINT': 2, WideField: 3 };
const ORIENT = { Free: 0, Fixed: 1, Random: 2 };
// isc_world_set_label's layout (core/include/insiliscope/insiliscope.h ISC_LABEL_*).
const labelVector = l => [l.density, l.fluorescentFraction, MODES[l.mode], l.kinetics.activationRatePerSec, l.kinetics.onSec,
  l.kinetics.offSec, l.kinetics.bleachProb, l.kinetics.photonCV, l.kinetics.initialOnSec, l.preState ? 1 : 0,
  ORIENT[l.orientation.mode], l.orientation.polarDeg, l.orientation.azimuthDeg, l.orientation.wobbleDeg, 0, 0];

let fails = 0;
const report = (ok, msg) => { console.log(`${ok ? 'ok  ' : 'FAIL'} ${msg}`); if (!ok) fails++; };

function cWorld(label) {
  const p = M._isc_params_new();
  const w = M._isc_world_new(seed, p);
  M._isc_params_free(p);
  const v = labelVector(label), buf = M._malloc(v.length * 8);
  M.HEAPF64.set(v, buf / 8);
  const r = M._isc_world_set_label(w, 0, buf, v.length);
  M._free(buf);
  if (r !== 0) throw new Error('isc_world_set_label: ' + r);
  return w;
}
// The ABI's cap/total convention, rows of `stride` doubles.
function rows(fn, stride) {
  const n = fn(0, 0);
  if (n < 0) throw new Error('query failed');
  const buf = M._malloc(Math.max(1, n) * stride * 8);
  const n2 = fn(buf, n);
  const out = [];
  for (let i = 0; i < n2; i++) out.push(Array.from(M.HEAPF64.subarray(buf / 8 + i * stride, buf / 8 + (i + 1) * stride)));
  M._free(buf);
  return out;
}
const evRow = e => [e.x, e.y, e.z, e.tOn, e.tOff, e.brightness, e.id, e.structure, e.state, e.aux];
function same(what, a, b) {
  let at = -1;
  if (a.length === b.length) {
    for (let i = 0; i < a.length && at < 0; i++) if (!a[i].every((v, j) => Object.is(v, b[i][j]))) at = i;
  }
  const ok = a.length === b.length && at < 0;
  report(ok, `${what}: ${a.length} C++ vs ${b.length} JS` + (ok ? '' : at >= 0 ? ` (first difference at ${at}: ${a[at]} vs ${b[at]})` : ''));
}

const win = [64, 4, 70, 10, -5, 50];
const kin = { activationRatePerSec: 0.05, onSec: 0.05, offSec: 1.0, bleachProb: 0.5, photonCV: 0.5 };
const CASES = [
  ['DNA-PAINT 70 %', { mode: 'DNA-PAINT', density: 0.7, kinetics: { ...kin, activationRatePerSec: 0.00143 } }],
  ['dSTORM 10 %, initial ON 2 s', { mode: 'dSTORM', density: 0.1, kinetics: { ...kin, initialOnSec: 2 } }],
  ['PALM 10 %, pre state, fraction 0.6', { mode: 'PALM', density: 0.1, fluorescentFraction: 0.6, preState: true, kinetics: kin }],
  ['WideField 30 %', { mode: 'WideField', density: 0.3, kinetics: kin }],
];
for (const [name, opts] of CASES) {
  const label = D.makeLabel(opts);
  const js = new World(P, seed, geom, [label]), w = cWorld(label);
  same(`${name}: dyes`, rows((b, c) => M._isc_sites_in_window(w, ...win, b, c), 5),
    js.sitesInWindow(...win).map(d => [d.x, d.y, d.z, d.id, d.structure]));
  // A query history: late, long, early, frame-sized.
  for (const [t0, t1] of [[120, 120.05], [0, 200], [3, 3.05], [60, 62]]) {
    same(`${name}: blinks [${t0}, ${t1})`, rows((b, c) => M._isc_events_in_window(w, ...win, t0, t1, b, c), 10),
      js.eventsInWindow(...win, t0, t1).map(evRow));
  }
  for (const tMin of [-Infinity, 1.0]) {
    same(`${name}: continuous windows ending after ${tMin}`, rows((b, c) => M._isc_continuous_in_window(w, ...win, tMin, b, c), 10),
      js.continuousInWindow(...win, tMin).map(evRow));
  }
  M._isc_world_free(w);
}
// ABI 11: kinetics histories. Each case: segments [tStart, kinetics] (the label's mode, density, pre state stay).
const KIN_KEYS = ['activationRatePerSec', 'onSec', 'offSec', 'bleachProb', 'photonCV', 'initialOnSec'];
function setHistory(w, js, segs) {
  const tStart = segs.map(x => x[0]), kins = segs.map(x => [{ initialOnSec: 0, ...x[1] }]);
  js.setKineticsHistory(tStart, kins);
  const v = segs.flatMap((x, i) => [x[0], ...KIN_KEYS.map(k => kins[i][0][k])]);
  const buf = M._malloc(Math.max(1, v.length) * 8);
  M.HEAPF64.set(v, buf / 8);
  const r = M._isc_world_set_kinetics_history(w, buf, segs.length);
  M._free(buf);
  if (r !== 0) throw new Error('isc_world_set_kinetics_history: ' + r);
}
const HCASES = [
  ['dSTORM initial ON 2 s, 405 x10 at 30 s, off at 90 s', { mode: 'dSTORM', density: 0.1, kinetics: { ...kin, initialOnSec: 2 } },
    [[[0, { ...kin, initialOnSec: 2 }], [1.5, { ...kin, initialOnSec: 0.5 }], [30, { ...kin, activationRatePerSec: 0.5, initialOnSec: 0.5 }],
      [90, { ...kin, activationRatePerSec: 0, initialOnSec: 0.5 }]],
     [[0, { ...kin, initialOnSec: 2 }], [30, { ...kin, onSec: 0.005, offSec: 0.1 }]]]],
  ['PALM pre state, UV off at 50 s, high at 100 s', { mode: 'PALM', density: 0.1, fluorescentFraction: 0.6, preState: true, kinetics: kin },
    [[[0, kin], [50, { ...kin, activationRatePerSec: 0 }], [100, { ...kin, activationRatePerSec: 0.5 }]],
     [[0, { ...kin, activationRatePerSec: 0.2 }]]]],
  ['DNA-PAINT, imager x10 mid bin', { mode: 'DNA-PAINT', density: 0.7, kinetics: { ...kin, activationRatePerSec: 0.00143 } },
    [[[0, { ...kin, activationRatePerSec: 0.00143 }], [60.4, { ...kin, activationRatePerSec: 0.0143, onSec: 0.2 }]],
     [[0, { ...kin, activationRatePerSec: 0.00143 }], [10, { ...kin, activationRatePerSec: 0 }]]]],
  ['WideField, rates step', { mode: 'WideField', density: 0.3, kinetics: kin },
    [[[0, kin], [40, { ...kin, onSec: 0.5 }]], [[0, kin]]]],
];
for (const [name, opts, hists] of HCASES) {
  const label = D.makeLabel(opts);
  const js = new World(P, seed, geom, [label]), w = cWorld(label);
  // History A, B, A again on the same worlds (the C++ keeps a schedule per history).
  for (const [hi, h] of [[0, hists[0]], [1, hists[1]], [0, hists[0]]]) {
    setHistory(w, js, h);
    for (const [t0, t1] of [[120, 120.05], [0, 200], [29.9, 30.2], [60, 62]]) {
      same(`${name} (history ${'AB'[hi]}): blinks [${t0}, ${t1})`, rows((b, c) => M._isc_events_in_window(w, ...win, t0, t1, b, c), 10),
        js.eventsInWindow(...win, t0, t1).map(evRow));
    }
    same(`${name} (history ${'AB'[hi]}): continuous windows`, rows((b, c) => M._isc_continuous_in_window(w, ...win, -Infinity, b, c), 10),
      js.continuousInWindow(...win, -Infinity).map(evRow));
  }
  M._isc_world_free(w);
}
console.log(fails ? `label parity: ${fails} FAILED` : 'label parity: PASS (C++ core = JS reference)');
process.exit(fails ? 1 : 0);
