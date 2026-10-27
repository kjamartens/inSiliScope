// The C++ core (WASM build, C ABI 16) against the JS reference (web/prototype/scope/world.js), number for number and in
// order, on one world per specimen (the cell field with its lamina on; the nanorulers), its labels switched per case:
//   labels      dyes (sitesInWindow), blinks (eventsInWindow) and continuous windows (continuousInWindow) for every label
//               mode, a fluorescent fraction, a pre state, an initial ON, the label linkage (ISC_LABEL_BINDER_NM ..;
//               none = NaN = the structure's built-in numbers) and a Fixed orientation, on each structure, over a query
//               history (late, long, early, frame-sized);
//   histories   ABI 11 kinetics histories (rate steps per mode; two histories alternating on one world), every structure
//               with its own kinetics row;
//   maps        ABI 14 clock maps (isc_events_in_map / isc_continuous_in_map against eventsInMap / continuousInMap): dose
//               clocks with shear, a depth table, an unlit band and epochs, activation clocks with steps, DNA-PAINT and
//               dSTORM read on T, a global clock, a photon-budget bleach; a moved present; the map removed; each
//               structure on a clock of its own, and the prefetch (isc_world_prefetch_map) changing no answer;
//   C ABI       isc_pack_window = the prototype's packMap, isc_cell_nucleus_rings = nucleusRingsLocal, the cap
//               convention (a short buffer still returns the total).
// A case fails with its name, the query and the first row that differs.
//   node tests/parity/label_parity.mjs [path to build/wasm/core/insiliscope.js]
import fs from 'fs';
import path from 'path';
import { createRequire } from 'module';
import { fileURLToPath, pathToFileURL } from 'url';
import { loadPrototype } from './load_prototype.mjs';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const require = createRequire(import.meta.url);
const modulePath = process.argv.slice(2).find(a => !a.startsWith('--'));
const M = await require(modulePath || path.join(ROOT, 'build/wasm/core/insiliscope.js'))();
const read = f => fs.readFileSync(path.join(ROOT, f), 'utf8');
const P = loadPrototype(read('web/prototype/index.html'), read('web/prototype/microtubules.js'));
const imp = f => import(pathToFileURL(path.join(ROOT, f)).href);
const { World, STRUCTURES, STRUCTURE_NPC, STRUCTURE_LAMIN } = await imp('web/prototype/scope/world.js');
const D = await imp('web/prototype/scope/dyes.js');
const { STRUCTURE_ORIGAMI } = await imp('web/prototype/scope/origami.js');
const CM = await imp('web/prototype/scope/clockmap.js');
if (M._isc_abi_version() !== 16) throw new Error('core ABI ' + M._isc_abi_version() + ', expected 16');
if (STRUCTURES.length !== 4 || STRUCTURE_NPC !== 1 || STRUCTURE_LAMIN !== 2 || STRUCTURE_ORIGAMI !== 3)
  throw new Error('structure ids moved');
const T0 = performance.now();

const seed = 1249, geom = P.paramsFrom(P.defaults);
const MT = 0, NPC = STRUCTURE_NPC, LAM = STRUCTURE_LAMIN, ORI = STRUCTURE_ORIGAMI;
const MODES = { dSTORM: 0, PALM: 1, 'DNA-PAINT': 2, WideField: 3 };
const ORIENT = { Free: 0, Fixed: 1, Random: 2 };
// isc_world_set_label's layout (core/include/insiliscope/insiliscope.h ISC_LABEL_*).
const labelVector = l => [l.density, l.fluorescentFraction, MODES[l.mode], l.kinetics.activationRatePerSec, l.kinetics.onSec,
  l.kinetics.offSec, l.kinetics.bleachProb, l.kinetics.photonCV, l.kinetics.initialOnSec, l.preState ? 1 : 0,
  ORIENT[l.orientation.mode], l.orientation.polarDeg, l.orientation.azimuthDeg, l.orientation.wobbleDeg, 0, 0,
  ...(l.linkage ? [l.linkage.binderNm, l.linkage.binderFlexNm, l.linkage.linkerMinNm, l.linkage.linkerMaxNm] : [NaN, NaN, NaN, NaN])];

let fails = 0, checks = 0;
const report = (ok, msg) => { checks++; if (!ok) { console.log(`FAIL ${msg}`); fails++; } };

// The two worlds, C and JS, of one specimen.
function worlds(specimen, extra) {
  const p = M._isc_params_new();
  for (const [k, v] of Object.entries({ specimen, ...extra })) {
    const n = M.lengthBytesUTF8(k) + 1, sp = M._malloc(n);
    M.stringToUTF8(k, sp, n);
    if (M._isc_params_set(p, sp, v) !== 0) throw new Error('isc_params_set ' + k);
    M._free(sp);
  }
  const w = M._isc_world_new(seed, p);
  M._isc_params_free(p);
  return { w, js: new World(P, seed, { ...geom, specimen, ...extra }) };
}
// Labels on every structure (a map structure -> label; the rest off), in both.
const OFF = D.makeLabel({ density: 0 });
function setLabels({ w, js }, on) {
  const labels = STRUCTURES.map((_, i) => on[i] ?? OFF);
  js.setLabels(labels);
  labels.forEach((l, i) => {
    const v = labelVector(l), buf = M._malloc(v.length * 8);
    M.HEAPF64.set(v, buf / 8);
    const r = M._isc_world_set_label(w, i, buf, v.length);
    M._free(buf);
    if (r !== 0) throw new Error('isc_world_set_label: ' + r);
  });
}
// The ABI's cap/total convention, rows of `stride` doubles; once per query kind, a 1-row buffer must return the total.
const capChecked = new Set();
function rows(kind, fn, stride) {
  const n = fn(0, 0);
  if (n < 0) throw new Error(kind + ': query failed');
  const buf = M._malloc(Math.max(1, n) * stride * 8);
  if (n > 1 && !capChecked.has(kind)) { capChecked.add(kind); report(fn(buf, 1) === n, `${kind}: a 1-row buffer returns the total ${n}`); }
  const n2 = fn(buf, n);
  const out = [];
  for (let i = 0; i < n2; i++) out.push(Array.from(M.HEAPF64.subarray(buf / 8 + i * stride, buf / 8 + (i + 1) * stride)));
  M._free(buf);
  return out;
}
const evRow = e => [e.x, e.y, e.z, e.tOn, e.tOff, e.brightness, e.id, e.structure, e.state, e.aux];
const mapRow = e => [...evRow(e), e.clock, e.rate];
// (an empty answer on both sides checks nothing: a case that should hold rows says so with `want`)
function same(what, a, b, want = true) {
  let at = -1;
  if (a.length === b.length) for (let i = 0; i < a.length && at < 0; i++) if (!a[i].every((v, j) => Object.is(v, b[i][j]))) at = i;
  const ok = a.length === b.length && at < 0 && (!want || a.length > 0);
  report(ok, `${what}: ${a.length} C++ vs ${b.length} JS` + (at >= 0 ? ` (first difference at row ${at}: ${a[at]} vs ${b[at]})` :
    !a.length && want ? ' (none: the case checks nothing)' : ''));
  return a.length;
}

const CELLS = worlds(0, { laminDensity: 50 });   // (the core's default lamina density is 0: none)
const RULERS = worlds(1, {});
// A 4 x 4 um window over the nucleus of cell (2, 0) (microtubules, pores and lamina all in it), and one over a few
// nanorulers.
const CWIN = [65, 5, 69, 9, -5, 50], RWIN = [0, 0, 8, 8, -5, 50];
const WIN = W => W === CELLS ? CWIN : RWIN;

// ---- C ABI: packing and the nucleus rings ----
{
  const [cx0, cy0, cx1, cy1] = [-2, -2, 3, 3];
  const map = P.gen.buildCandidateMap(seed, cx0, cy0, cx1, cy1, geom);
  const removed = P.gen.packMap(map, geom);
  const p = M._isc_params_new(), buf = M._malloc(4096 * 5 * 8), rem = M._malloc(4);
  const n = M._isc_pack_window(seed, cx0, cy0, cx1, cy1, p, buf, 4096, rem);
  const got = new Map();
  for (let i = 0; i < n; i++) got.set(`${M.HEAPF64[buf / 8 + i * 5]},${M.HEAPF64[buf / 8 + i * 5 + 1]}`, Array.from(M.HEAPF64.subarray(buf / 8 + i * 5 + 2, buf / 8 + i * 5 + 5)));
  let diff = null;
  for (const c of map.values()) {
    const g = got.get(`${c.cx},${c.cy}`), v = [c.x, c.y, c.packRot];
    if (!diff && (!g || !v.every((x, j) => Object.is(x, g[j])))) diff = `cell (${c.cx}, ${c.cy}): ${g} vs ${v}`;
  }
  report(n === map.size && M.HEAP32[rem / 4] === removed && !diff,
    `isc_pack_window = packMap: ${n} C++ vs ${map.size} JS cells, removed ${M.HEAP32[rem / 4]} vs ${removed}` + (diff ? `, first difference ${diff}` : ''));
  M._free(buf); M._free(rem); M._isc_params_free(p);

  const c = CELLS.js.cellsInRect(...CWIN.slice(0, 4))[0];
  const want = P.gen.nucleusRingsLocal(c, 17, 24).flat().map(q => [q.x, q.y, q.z]);
  const rb = M._malloc(17 * 24 * 3 * 8);
  const nr = M._isc_cell_nucleus_rings(CELLS.w, c.cx, c.cy, 17, 24, rb, 17 * 24);
  const ring = Array.from({ length: Math.max(0, nr) }, (_, i) => Array.from(M.HEAPF64.subarray(rb / 8 + 3 * i, rb / 8 + 3 * i + 3)));
  M._free(rb);
  same(`isc_cell_nucleus_rings = nucleusRingsLocal, cell (${c.cx}, ${c.cy})`, ring, want);
}

// ---- labels ----
const kin = { activationRatePerSec: 0.05, onSec: 0.05, offSec: 1.0, bleachProb: 0.5, photonCV: 0.5 };
const paintKin = { ...kin, activationRatePerSec: 0.00143 };
const L = {
  paint: { mode: 'DNA-PAINT', density: 0.3, kinetics: paintKin },
  storm: { mode: 'dSTORM', density: 0.1, kinetics: { ...kin, initialOnSec: 2 } },
  palm: { mode: 'PALM', density: 0.1, fluorescentFraction: 0.6, preState: true, kinetics: kin },
  wf: { mode: 'WideField', density: 0.3, kinetics: kin },
};
const FLEX = { linkage: { binderNm: 10, binderFlexNm: 4, linkerMinNm: 1, linkerMaxNm: 6 }, orientation: { mode: 'Fixed', polarDeg: 30, azimuthDeg: 40 } };
const IGG2 = { linkage: { binderNm: 19.3, binderFlexNm: 15.3, linkerMinNm: 0.5, linkerMaxNm: 2 }, orientation: { mode: 'Fixed', polarDeg: 30, azimuthDeg: 40 } };
const IGG = { linkage: { binderNm: 17.5, binderFlexNm: 5, linkerMinNm: 0, linkerMaxNm: 2 } };
const lab = (...o) => D.makeLabel(Object.assign({}, ...o));
// The microtubules carry ~10x the pores' and lamina's sites in the window, the nanorulers few: their densities scaled
// to a few hundred dyes each.
const mt = l => ({ ...l, density: l.density / 10 }), ru = l => ({ ...l, density: 0.8 });
// [name, world, structure -> label]
const LCASES = [
  ...Object.entries(L).map(([k, l]) => [`microtubules ${l.mode}`, CELLS, { [MT]: lab(mt(l)) }]),
  ['microtubules dSTORM, IgG-like linkage', CELLS, { [MT]: lab(mt(L.storm), IGG) }],
  ['pores DNA-PAINT (built-in linkage)', CELLS, { [NPC]: lab(L.paint) }],
  ['pores PALM, flexible linkage, Fixed', CELLS, { [NPC]: lab(L.palm, FLEX) }],
  ['lamina WideField (built-in linkage)', CELLS, { [LAM]: lab(L.wf) }],
  ['lamina dSTORM, IgG2-PAINT linkage, Fixed', CELLS, { [LAM]: lab(L.storm, IGG2) }],
  ['all three: microtubules dSTORM, pores DNA-PAINT, lamina PALM', CELLS, { [MT]: lab(mt(L.storm)), [NPC]: lab(L.paint), [LAM]: lab(L.palm) }],
  ...Object.entries(L).map(([k, l]) => [`nanorulers ${l.mode}`, RULERS, { [ORI]: lab(ru(l)) }]),
];
const tl = performance.now();
for (const [name, W, on] of LCASES) {
  const n0 = fails, win = WIN(W), tc = performance.now();
  const pre = s => `${name}: ${s}`;
  setLabels(W, on);
  const sameN = (what, a, b, want) => same(pre(what), a, b, want);
  sameN('dyes', rows('sites', (b, c) => M._isc_sites_in_window(W.w, ...win, b, c), 5),
    W.js.sitesInWindow(...win).map(d => [d.x, d.y, d.z, d.id, d.structure]));
  const continuous = Object.values(on).some(l => l.mode === 'WideField' || l.preState || l.kinetics.initialOnSec);
  for (const [t0, t1] of [[120, 120.05], [0, 20], [3, 3.05], [60, 62]])
    sameN(`blinks [${t0}, ${t1})`, rows('events', (b, c) => M._isc_events_in_window(W.w, ...win, t0, t1, b, c), 10),
      W.js.eventsInWindow(...win, t0, t1).map(evRow), t1 - t0 > 1 && !Object.values(on).some(l => l.mode === 'WideField'));
  for (const tMin of [-Infinity, 1.0])
    sameN(`continuous windows ending after ${tMin}`, rows('continuous', (b, c) => M._isc_continuous_in_window(W.w, ...win, tMin, b, c), 10),
      W.js.continuousInWindow(...win, tMin).map(evRow), continuous && tMin < 0);
  if (fails === n0) console.log(`ok   ${name} (${(performance.now() - tc).toFixed(0)} ms)`);
}
console.log(`labels: ${LCASES.length} cases, ${((performance.now() - tl) / 1000).toFixed(1)} s`);

// ---- ABI 11: kinetics histories ----
// Each case: two histories of segments [tStart, kinetics] (the label's mode, density, pre state stay). Every structure
// gets its own row (onSec x (1 + 0.25 s)), so a row read for the wrong structure differs.
const KIN_KEYS = ['activationRatePerSec', 'onSec', 'offSec', 'bleachProb', 'photonCV', 'initialOnSec'];
function setHistory({ w, js }, segs) {
  const tStart = segs.map(x => x[0]);
  const kins = segs.map(x => STRUCTURES.map((_, s) => ({ initialOnSec: 0, ...x[1], onSec: x[1].onSec * (1 + 0.25 * s) })));
  js.setKineticsHistory(tStart, kins);
  const v = segs.flatMap((x, i) => [x[0], ...kins[i].flatMap(q => KIN_KEYS.map(k => q[k]))]);
  const buf = M._malloc(Math.max(1, v.length) * 8);
  M.HEAPF64.set(v, buf / 8);
  const r = M._isc_world_set_kinetics_history(w, buf, segs.length);
  M._free(buf);
  if (r !== 0) throw new Error('isc_world_set_kinetics_history: ' + r);
}
const HCASES = [
  ['microtubules dSTORM initial ON 2 s, 405 x10 at 30 s, off at 90 s', CELLS, MT, mt(L.storm),
    [[[0, { ...kin, initialOnSec: 2 }], [1.5, { ...kin, initialOnSec: 0.5 }], [30, { ...kin, activationRatePerSec: 0.5, initialOnSec: 0.5 }],
      [90, { ...kin, activationRatePerSec: 0, initialOnSec: 0.5 }]],
     [[0, { ...kin, initialOnSec: 2 }], [30, { ...kin, onSec: 0.005, offSec: 0.1 }]]]],
  ['pores PALM pre state, UV off at 50 s, high at 100 s', CELLS, NPC, L.palm,
    [[[0, kin], [50, { ...kin, activationRatePerSec: 0 }], [100, { ...kin, activationRatePerSec: 0.5 }]],
     [[0, { ...kin, activationRatePerSec: 0.2 }]]]],
  ['lamina DNA-PAINT, imager x10 mid bin', CELLS, LAM, L.paint,
    [[[0, paintKin], [29.95, { ...kin, activationRatePerSec: 0.0143, onSec: 0.2 }]],
     [[0, paintKin], [10, { ...kin, activationRatePerSec: 0 }]]]],
  ['microtubules WideField, rates step', CELLS, MT, mt(L.wf), [[[0, kin], [40, { ...kin, onSec: 0.5 }]], [[0, kin]]]],
  ['nanorulers DNA-PAINT, imager x10 mid bin', RULERS, ORI, ru(L.paint),
    [[[0, paintKin], [29.95, { ...kin, activationRatePerSec: 0.0143, onSec: 0.2 }]],
     [[0, paintKin], [10, { ...kin, activationRatePerSec: 0 }]]]],
];
const th = performance.now();
for (const [name, W, s, l, hists] of HCASES) {
  const n0 = fails, win = WIN(W), tc = performance.now();
  setLabels(W, { [s]: lab(l) });
  let n = 0;
  // History A, B, A again on the same worlds (the C++ keeps a schedule per history).
  for (const [hi, h] of [[0, hists[0]], [1, hists[1]], [0, hists[0]]]) {
    setHistory(W, h);
    const pre = s => `${name} (history ${'AB'[hi]}): ${s}`;
    for (const [t0, t1] of [[29.9, 30.2], [60, 62]])
      n += same(pre(`blinks [${t0}, ${t1})`), rows('events', (b, c) => M._isc_events_in_window(W.w, ...win, t0, t1, b, c), 10),
        W.js.eventsInWindow(...win, t0, t1).map(evRow), false);
    n += same(pre('continuous windows'), rows('continuous', (b, c) => M._isc_continuous_in_window(W.w, ...win, -Infinity, b, c), 10),
      W.js.continuousInWindow(...win, -Infinity).map(evRow), false);
  }
  setHistory(W, []);
  report(n > 0, `${name}: no rows at all (the case checks nothing)`);
  if (fails === n0) console.log(`ok   ${name} (${(performance.now() - tc).toFixed(0)} ms)`);
}
console.log(`kinetics histories: ${HCASES.length} cases, ${((performance.now() - th) / 1000).toFixed(1)} s`);

// ---- ABI 14: clock maps ----
// Two grids (one ending inside the window: the dyes past 3.75 um from its left edge are unlit), a depth table; per case the
// labelled structure's clock, every other structure on a global clock; then the present moved on (tOpen + 10, the past
// rate folded into the total, the beam 0.3 um over; an activation clock gets a step); the map removed once.
const wavy = (nx, ny, f) => Array.from({ length: nx * ny }, (_, k) => f(k % nx, Math.floor(k / nx)));
const GRIDS = win => [
  { x0: win[0] - 2, y0: win[1] - 2, h: 0.25, nx: 23, ny: 30, v: wavy(23, 30, (i, j) => 0.6 + 0.4 * Math.sin(0.7 * i + 0.3 * j)) },
  { x0: win[0] - 5, y0: win[1] - 5, h: 0.5, nx: 30, ny: 30, v: wavy(30, 30, (i, j) => 1 + 0.5 * Math.cos(0.4 * i - 0.5 * j)) },
];
const DEPTHS = [{ z0: 0, dz: 0.5, v: [1, 0.7, 0.4, 0.2] }];
const hist = segs => ({ tStart: segs.map(x => x[0]), kin: segs.map(x => ({ initialOnSec: 0, ...x[1] })) });
const RATE = [{ grid: 0, depth: 0, sx: 0.3, zp: 0.2, scale: 1 }];
const dose = (segs, bound = []) => ({ mode: CM.CLOCK_DOSE, hist: hist(segs), bound, total: [{ grid: 1, scale: 50 }], totalC: 0,
  rate: RATE, rateC: 0, tOpen: 50 });
const activation = segs => ({ mode: CM.CLOCK_ACTIVATION, hist: hist(segs), total: [], rate: [{ grid: 0, depth: 0, scale: 0.02 }],
  rateC: 0, tOpen: 30, steps: [{ t0: 0, t1: 10, c: 0, field: [{ grid: 1, scale: 0.05 }] }, { t0: 10, t1: 25, c: 0.01, field: [] },
    { t0: 25, t1: 30, c: 0, field: [{ grid: 0, scale: 0.3 }] }] });
const global = segs => ({ mode: CM.CLOCK_GLOBAL, hist: hist(segs), total: [], rate: [], tOpen: 50 });
// The present 10 s on: a dose clock's last 10 s of rate folded into its total, the beam moved; an activation clock's
// open rate becomes a step.
function moved(c) {
  const n = structuredClone(c);
  if (c.mode === CM.CLOCK_DOSE) {
    n.total = [...c.total, ...c.rate.map(t => ({ ...t, scale: t.scale * 10 }))];
    n.totalC = c.totalC + 10 * c.rateC;
    n.rate = c.rate.map(t => ({ ...t, ox: 0.3 }));
  } else if (c.mode === CM.CLOCK_ACTIVATION) {
    n.steps = [...c.steps, { t0: c.tOpen, t1: c.tOpen + 10, c: 10 * c.rateC, field: c.rate.map(t => ({ ...t, scale: t.scale * 10 })) }];
    n.rate = c.rate.map(t => ({ ...t, ox: 0.3 }));
  }
  if (c.bleachRate) n.bleachTotal = [...c.bleachTotal, ...c.bleachRate.map(t => ({ ...t, scale: t.scale * 10 }))];
  n.tOpen = c.tOpen + 10;
  return n;
}
// A photon-budget bleach (the hazard by tOpen, then per second): a budgeted window ends where it reaches its draw.
const bleach = c => ({ ...c, bleachTotal: [{ grid: 1, scale: 0.6 }], bleachRate: [{ grid: 0, depth: 0, sx: 0.3, zp: 0.2, scale: 0.05 }] });
const OTHER = global([[0, kin], [20, { ...kin, activationRatePerSec: 0.1 }]]);
const sparse = l => ({ ...l, density: 0.05 });   // (the microtubules' at a tenth of it)
// [name, world, structure, label, clock]; the other structures of the cell field carry a sparse dSTORM label on OTHER.
const MCASES = [
  ['microtubules dSTORM initial ON 2 s, dose (shear, depth, unlit band)', CELLS, MT, sparse(L.storm), dose([[0, { ...kin, initialOnSec: 2 }]])],
  ['pores dSTORM, dose with three epochs (one giving way)', CELLS, NPC, sparse(L.storm),
    dose([[0, { ...kin, initialOnSec: 2 }], [20, { ...kin, activationRatePerSec: 0.5, initialOnSec: 2 }],
      [40, { ...kin, activationRatePerSec: 0.01, onSec: 0.02, initialOnSec: 2 }]], [[{ grid: 1, scale: 20 }], [{ grid: 0, scale: 45 }]])],
  ['lamina PALM pre state, activation steps then the open rate', CELLS, LAM, sparse(L.palm), activation([[0, kin], [60, { ...kin, onSec: 0.1 }]])],
  ['microtubules PALM pre state, dose', CELLS, MT, sparse(L.palm), dose([[0, kin]])],
  ['pores PALM pre state with a bleach, activation', CELLS, NPC, sparse(L.palm), bleach(activation([[0, kin]]))],
  ['lamina WideField, dose (unlit band)', CELLS, LAM, { ...L.wf, density: 0.1 }, dose([[0, kin]])],
  ['microtubules WideField with a bleach, dose', CELLS, MT, { ...L.wf, density: 0.1 }, bleach(dose([[0, kin]]))],
  ['pores DNA-PAINT under a dose clock (on T, two epochs)', CELLS, NPC, { ...L.paint, density: 0.2 },
    dose([[0, paintKin], [60.4, { ...paintKin, activationRatePerSec: 0.0143, onSec: 0.2 }]], [[{ grid: 1, scale: 20 }]])],
  ['microtubules dSTORM under an activation clock (on T)', CELLS, MT, sparse(L.storm), activation([[0, kin], [30, { ...kin, activationRatePerSec: 0.2 }]])],
  ['lamina dSTORM, global clock with two segments', CELLS, LAM, sparse(L.storm), global([[0, kin], [30, { ...kin, activationRatePerSec: 0.2 }]])],
  ['nanorulers DNA-PAINT under a dose clock (on T, two epochs)', RULERS, ORI, { ...L.paint, density: 0.2 },
    dose([[0, paintKin], [60.4, { ...paintKin, activationRatePerSec: 0.0143, onSec: 0.2 }]], [[{ grid: 1, scale: 20 }]])],
  ['nanorulers dSTORM, dose', RULERS, ORI, sparse(L.storm), dose([[0, { ...kin, initialOnSec: 2 }]])],
];
function setMap({ w, js }, m) {
  const v = m ? CM.encodeClockMap(m) : [];
  js.setClockMap(v);
  const buf = M._malloc(Math.max(1, v.length) * 8);
  M.HEAPF64.set(v, buf / 8);
  const r = M._isc_world_set_clock_map(w, buf, v.length);
  M._free(buf);
  if (r !== 0) throw new Error('isc_world_set_clock_map: ' + r);
}
const MFRAMES = [[50, 50.05], [52, 54], [30, 31], [61, 61.05]];
const tm = performance.now();
for (const [ci, [name, W, s, l, clock]] of MCASES.entries()) {
  const n0 = fails, win = WIN(W), tc = performance.now();
  setLabels(W, W === CELLS ? { [MT]: lab(mt(sparse(L.storm))), [NPC]: lab(sparse(L.storm)), [LAM]: lab(sparse(L.storm)),
    [s]: lab(s === MT ? mt(l) : l) } : { [s]: lab(ru(l)) });
  let n = 0;
  for (const [mi, c] of [[0, clock], [1, moved(clock)], ...(ci === 0 ? [[2, null]] : [])]) {
    setMap(W, c && { grids: GRIDS(win), depths: DEPTHS, s: STRUCTURES.map((_, i) => i === s ? c : OTHER) });
    const pre = q => `${name} (${['map', 'moved map', 'no map'][mi]}): ${q}`;
    // (the prefetch fills caches only: the answers after it are the JS's)
    if (mi === 0 && M._isc_world_prefetch_map(W.w, ...win, MFRAMES[0][0], MFRAMES[0][1], 50) < 0) report(false, pre('isc_world_prefetch_map failed'));
    // (a bleach changes the windows only)
    for (const [t0, t1] of clock.bleachRate ? [] : MFRAMES) {
      n += same(pre(`blinks [${t0}, ${t1})`), rows('events in map', (b, n) => M._isc_events_in_map(W.w, ...win, t0, t1, b, n), 10),
        W.js.eventsInMap(...win, t0, t1).map(evRow), false);
    }
    for (const tMin of [-Infinity, 55]) {
      n += same(pre(`continuous windows ending after ${tMin}`),
        rows('continuous in map', (b, n) => M._isc_continuous_in_map(W.w, ...win, tMin, b, n), 12), W.js.continuousInMap(...win, tMin).map(mapRow), false);
    }
  }
  setMap(W, null);
  report(n > 0, `${name}: no rows at all (the case checks nothing)`);
  if (fails === n0) console.log(`ok   ${name} (${(performance.now() - tc).toFixed(0)} ms)`);
}
console.log(`clock maps: ${MCASES.length} cases, ${((performance.now() - tm) / 1000).toFixed(1)} s`);
for (const W of [CELLS, RULERS]) M._isc_world_free(W.w);
console.log(fails ? `label parity: ${fails} of ${checks} checks FAILED` :
  `label parity: PASS (${checks} checks, C++ core = JS reference; ${((performance.now() - T0) / 1000).toFixed(1)} s)`);
process.exit(fails ? 1 : 0);
